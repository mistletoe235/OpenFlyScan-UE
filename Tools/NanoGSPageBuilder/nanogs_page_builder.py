#!/usr/bin/env python3
"""Build and verify streamable NanoGS Page v1 containers.

The converter intentionally uses only Python's standard library.  Its two-pass
PLY scan and external Morton sort keep memory proportional to one input chunk,
one sort run, and one output page rather than to the source splat count.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import hashlib
import heapq
import json
import math
import os
import shutil
import struct
import sys
import tempfile
import time
import zlib
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator, Sequence


MAGIC = b"NGSPAGE1"
VERSION = 1
ENDIAN_TAG = 0x01020304
PAYLOAD_FORMAT = 1  # LosslessGeometry_SH3Half_RGBA8_SoA

HEADER_BYTES = 256
PAGE_RECORD_BYTES = 160
STREAM_RECORD_BYTES = 80
PAYLOAD_ALIGNMENT = 64
FILE_PAYLOAD_ALIGNMENT = 4096
SUPPORT_SIGMA = 2.0
SH_C0 = 0.28209479177387814
SH_C0_F32 = struct.unpack("<f", struct.pack("<f", SH_C0))[0]

HEADER_FLAG_STREAM_CRC32 = 1 << 0
HEADER_FLAG_DENSE_SOURCE_IDS = 1 << 1
HEADER_FLAG_PAYLOAD_ALIGNED_64 = 1 << 2
PAGE_FLAG_ROTATED_GAUSSIAN_AABB_2SIGMA = 1 << 0

SEMANTIC_POSITION = 1
SEMANTIC_ROTATION = 2
SEMANTIC_SCALE = 3
SEMANTIC_COLOR_OPACITY = 4
SEMANTIC_SH = 5
SEMANTIC_ORIGINAL_ID = 6

ENCODING_FLOAT32 = 1
ENCODING_UNORM8 = 2
ENCODING_FLOAT16 = 3
ENCODING_UINT64 = 4

COORDINATE_SYSTEM = 1  # NanoGSLocalCentimeters_SourceXYZ
SH_ORDER = 3
SH_LAYOUT = 1  # coefficient-major RGB, DC first

STREAM_SPECS = (
    (SEMANTIC_POSITION, ENCODING_FLOAT32, 3, 12, "position_f32x3"),
    (SEMANTIC_ROTATION, ENCODING_FLOAT32, 4, 16, "rotation_f32x4_xyzw"),
    (SEMANTIC_SCALE, ENCODING_FLOAT32, 3, 12, "scale_f32x3_cm"),
    (SEMANTIC_COLOR_OPACITY, ENCODING_UNORM8, 4, 4, "rgba8_unorm"),
    (SEMANTIC_SH, ENCODING_FLOAT16, 48, 96, "sh3_f16_coeff_major_rgb"),
    (SEMANTIC_ORIGINAL_ID, ENCODING_UINT64, 1, 8, "original_id_u64"),
)

SEMANTIC_NAMES = {spec[0]: spec[4] for spec in STREAM_SPECS}
STREAM_SPEC_BY_SEMANTIC = {spec[0]: spec for spec in STREAM_SPECS}

# External-sort record: big-endian Morton and source ID make byte strings sort
# numerically, followed by the exact 140 bytes needed by resident rendering.
SPOOL_PREFIX = struct.Struct(">QQ")
SPOOL_GEOMETRY_COLOR = struct.Struct("<3f4f3f4B")
SH_HALF_STRUCT = struct.Struct("<48e")
SPOOL_BODY_BYTES = 140
SPOOL_RECORD_BYTES = SPOOL_PREFIX.size + SPOOL_BODY_BYTES
assert SPOOL_GEOMETRY_COLOR.size == 44
assert SPOOL_RECORD_BYTES == 156

PAGE_STRUCT = struct.Struct("<5Q12d2d2I")
STREAM_STRUCT = struct.Struct("<QII6QIIQ")
assert PAGE_STRUCT.size == PAGE_RECORD_BYTES
assert STREAM_STRUCT.size == STREAM_RECORD_BYTES

PLY_SCALARS = {
    "char": ("b", 1),
    "int8": ("b", 1),
    "uchar": ("B", 1),
    "uint8": ("B", 1),
    "short": ("h", 2),
    "int16": ("h", 2),
    "ushort": ("H", 2),
    "uint16": ("H", 2),
    "int": ("i", 4),
    "int32": ("i", 4),
    "uint": ("I", 4),
    "uint32": ("I", 4),
    "float": ("f", 4),
    "float32": ("f", 4),
    "double": ("d", 8),
    "float64": ("d", 8),
}


class FormatError(RuntimeError):
    """Input or container format error."""


class VerificationError(RuntimeError):
    """Container verification failure."""


@dataclasses.dataclass(frozen=True)
class PlyProperty:
    name: str
    scalar_type: str
    offset: int
    value_index: int


@dataclasses.dataclass(frozen=True)
class PlyHeader:
    path: Path
    header_bytes: bytes
    data_offset: int
    vertex_count: int
    vertex_stride: int
    properties: tuple[PlyProperty, ...]
    vertex_struct: struct.Struct
    property_indices: dict[str, int]
    property_offsets: dict[str, int]
    expected_vertex_end: int
    file_size: int


@dataclasses.dataclass
class StreamRecord:
    page_id: int
    semantic: int
    encoding: int
    element_count: int
    component_count: int
    element_stride_bytes: int
    file_offset: int
    byte_size: int
    uncompressed_byte_size: int
    payload_crc32: int
    flags: int = 0
    reserved: int = 0

    def pack(self) -> bytes:
        return STREAM_STRUCT.pack(
            self.page_id,
            self.semantic,
            self.encoding,
            self.element_count,
            self.component_count,
            self.element_stride_bytes,
            self.file_offset,
            self.byte_size,
            self.uncompressed_byte_size,
            self.payload_crc32,
            self.flags,
            self.reserved,
        )


@dataclasses.dataclass
class PageRecord:
    page_id: int
    first_output_index: int
    splat_count: int
    stream_start: int
    stream_count: int
    center_min: tuple[float, float, float]
    center_max: tuple[float, float, float]
    support_min: tuple[float, float, float]
    support_max: tuple[float, float, float]
    support_sigma: float
    max_scale_cm: float
    flags: int = PAGE_FLAG_ROTATED_GAUSSIAN_AABB_2SIGMA
    reserved: int = 0

    def pack(self) -> bytes:
        return PAGE_STRUCT.pack(
            self.page_id,
            self.first_output_index,
            self.splat_count,
            self.stream_start,
            self.stream_count,
            *self.center_min,
            *self.center_max,
            *self.support_min,
            *self.support_max,
            self.support_sigma,
            self.max_scale_cm,
            self.flags,
            self.reserved,
        )


def _f32(value: float) -> float:
    try:
        return struct.unpack("<f", struct.pack("<f", value))[0]
    except OverflowError:
        # C++ float arithmetic and expf overflow to signed infinity.
        return math.copysign(math.inf, value)


def _exp_f32(value: float) -> float:
    try:
        result = math.exp(value)
    except OverflowError:
        result = math.inf
    return _f32(result)


def _sigmoid_f32(value: float) -> float:
    exp_value = _exp_f32(_f32(-value))
    return _f32(1.0 / _f32(1.0 + exp_value))


def _half_bits(value: float) -> bytes:
    try:
        return struct.pack("<e", value)
    except OverflowError:
        return struct.pack("<H", 0x7C00 if value >= 0.0 else 0xFC00)


def _half_roundtrip(value: float) -> float:
    return struct.unpack("<e", _half_bits(value))[0]


def _round_unorm8(value: float) -> int:
    clamped = min(1.0, max(0.0, value))
    # FMath::RoundToInt is half-away-from-zero.  Inputs here are non-negative.
    return min(255, max(0, int(math.floor(_f32(clamped * 255.0) + 0.5))))


def _normalize_quaternion_wxyz(
    qw: float, qx: float, qy: float, qz: float
) -> tuple[float, float, float, float]:
    # Match GaussianSplattingUtils::NormalizeQuat: operations are float32 and
    # the output layout changes from source WXYZ to NanoGS XYZW.
    xx = _f32(qx * qx)
    yy = _f32(qy * qy)
    zz = _f32(qz * qz)
    ww = _f32(qw * qw)
    length_sq = _f32(_f32(_f32(xx + yy) + zz) + ww)
    length = _f32(math.sqrt(max(0.0, length_sq)))
    if length > 1.0e-8:
        inv = _f32(1.0 / length)
        return (
            _f32(qx * inv),
            _f32(qy * inv),
            _f32(qz * inv),
            _f32(qw * inv),
        )
    return (0.0, 0.0, 0.0, 1.0)


def _rgba_lane(sh_dc: Sequence[float], opacity: float) -> tuple[int, int, int, int]:
    colors = []
    for coefficient in sh_dc:
        linear = _f32(0.5 + _f32(SH_C0_F32 * coefficient))
        colors.append(_half_roundtrip(linear))
    opacity_half = _half_roundtrip(opacity)
    return (
        _round_unorm8(colors[0]),
        _round_unorm8(colors[1]),
        _round_unorm8(colors[2]),
        _round_unorm8(opacity_half),
    )


def _align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def _write_alignment(handle: BinaryIO, alignment: int) -> None:
    padding = _align_up(handle.tell(), alignment) - handle.tell()
    if padding:
        handle.write(b"\0" * padding)


def _is_power_of_two(value: int) -> bool:
    return value > 0 and (value & (value - 1)) == 0


def _next_power_of_two(value: int) -> int:
    return 1 if value <= 1 else 1 << (value - 1).bit_length()


def _part1by2(value: int) -> int:
    value &= 0x1FFFFF
    value = (value | (value << 32)) & 0x1F00000000FFFF
    value = (value | (value << 16)) & 0x1F0000FF0000FF
    value = (value | (value << 8)) & 0x100F00F00F00F00F
    value = (value | (value << 4)) & 0x10C30C30C30C30C3
    value = (value | (value << 2)) & 0x1249249249249249
    return value


def _morton63(
    position: Sequence[float],
    bounds_min: Sequence[float],
    bounds_max: Sequence[float],
) -> int:
    quantized = []
    max_quantized = (1 << 21) - 1
    for axis in range(3):
        span = bounds_max[axis] - bounds_min[axis]
        normalized = 0.5 if span <= 0.0 else (position[axis] - bounds_min[axis]) / span
        normalized = min(1.0, max(0.0, normalized))
        quantized.append(int(normalized * max_quantized + 0.5))
    return (
        _part1by2(quantized[0])
        | (_part1by2(quantized[1]) << 1)
        | (_part1by2(quantized[2]) << 2)
    )


def parse_ply_header(path: Path | str) -> PlyHeader:
    path = Path(path)
    file_size = path.stat().st_size
    if file_size < 4:
        raise FormatError("PLY file is too small")

    header = bytearray()
    with path.open("rb") as handle:
        while len(header) <= 1024 * 1024:
            line = handle.readline()
            if not line:
                break
            header.extend(line)
            if line.rstrip(b"\r\n") == b"end_header":
                break
        else:
            raise FormatError("PLY header exceeds 1 MiB")

    if not header.endswith((b"end_header\n", b"end_header\r\n")):
        raise FormatError("PLY header has no end_header line")
    try:
        lines = bytes(header).decode("ascii").splitlines()
    except UnicodeDecodeError as exc:
        raise FormatError("PLY header must be ASCII") from exc
    if not lines or lines[0].strip() != "ply":
        raise FormatError("PLY magic is missing")

    binary_little_endian = False
    vertex_count: int | None = None
    in_vertex = False
    properties: list[PlyProperty] = []
    offset = 0
    format_codes: list[str] = []

    for raw_line in lines[1:]:
        parts = raw_line.strip().split()
        if not parts or parts[0] in ("comment", "obj_info"):
            continue
        if parts[0] == "format":
            if len(parts) < 3 or parts[1] != "binary_little_endian" or parts[2] != "1.0":
                raise FormatError("only 'format binary_little_endian 1.0' is supported")
            binary_little_endian = True
        elif parts[0] == "element":
            if len(parts) != 3:
                raise FormatError(f"invalid element line: {raw_line}")
            in_vertex = parts[1] == "vertex"
            if in_vertex:
                if vertex_count is not None:
                    raise FormatError("multiple vertex elements are not supported")
                try:
                    vertex_count = int(parts[2])
                except ValueError as exc:
                    raise FormatError("invalid vertex count") from exc
                if vertex_count <= 0 or vertex_count > 0xFFFFFFFFFFFFFFFF:
                    raise FormatError("vertex count is outside uint64 range")
        elif parts[0] == "property" and in_vertex:
            if len(parts) != 3 or parts[1] == "list":
                raise FormatError("list properties in the vertex element are unsupported")
            scalar_type, name = parts[1], parts[2]
            if scalar_type not in PLY_SCALARS:
                raise FormatError(f"unsupported PLY scalar type: {scalar_type}")
            if any(item.name == name for item in properties):
                raise FormatError(f"duplicate vertex property: {name}")
            code, size = PLY_SCALARS[scalar_type]
            properties.append(PlyProperty(name, scalar_type, offset, len(properties)))
            format_codes.append(code)
            offset += size

    if not binary_little_endian:
        raise FormatError("binary_little_endian PLY format declaration is missing")
    if vertex_count is None:
        raise FormatError("PLY has no vertex element")
    if not properties or offset <= 0:
        raise FormatError("PLY vertex element has no fixed-width properties")

    property_by_name = {item.name: item for item in properties}
    required = [
        "x", "y", "z",
        "f_dc_0", "f_dc_1", "f_dc_2",
        "opacity",
        "scale_0", "scale_1", "scale_2",
        "rot_0", "rot_1", "rot_2", "rot_3",
        *(f"f_rest_{index}" for index in range(45)),
    ]
    missing = [name for name in required if name not in property_by_name]
    if missing:
        raise FormatError("full SH3 PLY is required; missing properties: " + ", ".join(missing))
    non_float = [
        name for name in required
        if property_by_name[name].scalar_type not in ("float", "float32")
    ]
    if non_float:
        raise FormatError(
            "NanoGS PLY parity requires float32 GS properties; non-float32: "
            + ", ".join(non_float)
        )

    vertex_struct = struct.Struct("<" + "".join(format_codes))
    if vertex_struct.size != offset:
        raise AssertionError("internal PLY stride mismatch")
    expected_end = len(header) + vertex_count * offset
    if expected_end > file_size:
        raise FormatError(
            f"truncated PLY: vertices end at {expected_end}, file has {file_size} bytes"
        )
    return PlyHeader(
        path=path,
        header_bytes=bytes(header),
        data_offset=len(header),
        vertex_count=vertex_count,
        vertex_stride=offset,
        properties=tuple(properties),
        vertex_struct=vertex_struct,
        property_indices={item.name: item.value_index for item in properties},
        property_offsets={item.name: item.offset for item in properties},
        expected_vertex_end=expected_end,
        file_size=file_size,
    )


def _iter_vertex_chunks(
    handle: BinaryIO, header: PlyHeader, chunk_points: int
) -> Iterator[tuple[int, bytes]]:
    handle.seek(header.data_offset)
    first_id = 0
    while first_id < header.vertex_count:
        count = min(chunk_points, header.vertex_count - first_id)
        byte_count = count * header.vertex_stride
        data = handle.read(byte_count)
        if len(data) != byte_count:
            raise FormatError(f"short read at source vertex {first_id}")
        yield first_id, data
        first_id += count


def scan_source(
    header: PlyHeader, chunk_points: int
) -> tuple[tuple[float, float, float], tuple[float, float, float], str]:
    bounds_min = [math.inf, math.inf, math.inf]
    bounds_max = [-math.inf, -math.inf, -math.inf]
    sha256 = hashlib.sha256()
    sha256.update(header.header_bytes)
    x_offset = header.property_offsets["x"]
    y_offset = header.property_offsets["y"]
    z_offset = header.property_offsets["z"]
    stride = header.vertex_stride

    with header.path.open("rb") as handle:
        for first_id, data in _iter_vertex_chunks(handle, header, chunk_points):
            sha256.update(data)
            for local_index in range(len(data) // stride):
                base = local_index * stride
                position = (
                    _f32(struct.unpack_from("<f", data, base + x_offset)[0] * 100.0),
                    _f32(struct.unpack_from("<f", data, base + y_offset)[0] * 100.0),
                    _f32(struct.unpack_from("<f", data, base + z_offset)[0] * 100.0),
                )
                if not all(math.isfinite(value) for value in position):
                    raise FormatError(f"non-finite position at source vertex {first_id + local_index}")
                for axis in range(3):
                    bounds_min[axis] = min(bounds_min[axis], position[axis])
                    bounds_max[axis] = max(bounds_max[axis], position[axis])
        while True:
            trailing = handle.read(8 * 1024 * 1024)
            if not trailing:
                break
            sha256.update(trailing)

    return tuple(bounds_min), tuple(bounds_max), sha256.hexdigest()


def _decode_spool_record(
    values: Sequence[float | int],
    indices: dict[str, int],
    source_id: int,
    center_bounds_min: Sequence[float],
    center_bounds_max: Sequence[float],
) -> tuple[int, bytes]:
    def value(name: str) -> float:
        return float(values[indices[name]])

    position = (
        _f32(value("x") * 100.0),
        _f32(value("y") * 100.0),
        _f32(value("z") * 100.0),
    )
    rotation = _normalize_quaternion_wxyz(
        value("rot_0"), value("rot_1"), value("rot_2"), value("rot_3")
    )
    scale = tuple(_f32(_exp_f32(value(f"scale_{axis}")) * 100.0) for axis in range(3))
    opacity = _sigmoid_f32(value("opacity"))
    sh_dc = tuple(value(f"f_dc_{channel}") for channel in range(3))
    sh_values: list[float] = [*sh_dc]
    for coefficient in range(15):
        for channel in range(3):
            sh_values.append(value(f"f_rest_{coefficient + channel * 15}"))

    finite_values = (*position, *rotation, *scale, opacity, *sh_values)
    if not all(math.isfinite(item) for item in finite_values):
        raise FormatError(f"non-finite decoded value at source vertex {source_id}")
    rgba = _rgba_lane(sh_dc, opacity)
    morton = _morton63(position, center_bounds_min, center_bounds_max)
    # Pack SH values one-by-one so finite values outside the half range follow
    # FFloat16's infinity behavior instead of Python struct's OverflowError.
    body = SPOOL_GEOMETRY_COLOR.pack(*position, *rotation, *scale, *rgba)
    try:
        body += SH_HALF_STRUCT.pack(*sh_values)
    except OverflowError:
        # Rare out-of-range coefficients follow FFloat16's signed-infinity
        # behavior; keep the common 48-value conversion in one C-level call.
        body += b"".join(_half_bits(item) for item in sh_values)
    if len(body) != SPOOL_BODY_BYTES:
        raise AssertionError("internal spool body size mismatch")
    return morton, SPOOL_PREFIX.pack(morton, source_id) + body


class BucketWriters:
    """LRU-capped buffered append handles for Morton buckets."""

    def __init__(self, root: Path, bucket_count: int, max_open: int = 64) -> None:
        self.root = root
        self.bucket_count = bucket_count
        self.max_open = max_open
        self.handles: collections.OrderedDict[int, BinaryIO] = collections.OrderedDict()
        self.counts = [0] * bucket_count

    def _path(self, bucket: int) -> Path:
        return self.root / f"bucket-{bucket:05d}.spool"

    def write(self, bucket: int, record: bytes) -> None:
        handle = self.handles.pop(bucket, None)
        if handle is None:
            if len(self.handles) >= self.max_open:
                _, old_handle = self.handles.popitem(last=False)
                old_handle.close()
            handle = self._path(bucket).open("ab", buffering=1024 * 1024)
        handle.write(record)
        self.handles[bucket] = handle
        self.counts[bucket] += 1

    def close(self) -> None:
        while self.handles:
            _, handle = self.handles.popitem(last=False)
            handle.close()

    def __enter__(self) -> "BucketWriters":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def bucket_source(
    header: PlyHeader,
    temp_root: Path,
    bucket_count: int,
    chunk_points: int,
    center_bounds_min: Sequence[float],
    center_bounds_max: Sequence[float],
) -> list[int]:
    bucket_bits = int(math.log2(bucket_count)) if bucket_count > 1 else 0
    with header.path.open("rb") as source, BucketWriters(temp_root, bucket_count) as writers:
        for first_id, data in _iter_vertex_chunks(source, header, chunk_points):
            count = len(data) // header.vertex_stride
            for local_index in range(count):
                values = header.vertex_struct.unpack_from(data, local_index * header.vertex_stride)
                source_id = first_id + local_index
                morton, record = _decode_spool_record(
                    values,
                    header.property_indices,
                    source_id,
                    center_bounds_min,
                    center_bounds_max,
                )
                bucket = morton >> (63 - bucket_bits) if bucket_bits else 0
                writers.write(bucket, record)
            _progress("bucket", first_id + count, header.vertex_count)
        return writers.counts


def _read_fixed_records(handle: BinaryIO, max_records: int) -> list[bytes]:
    data = handle.read(max_records * SPOOL_RECORD_BYTES)
    if not data:
        return []
    if len(data) % SPOOL_RECORD_BYTES:
        raise FormatError("temporary spool file has a partial record")
    return [
        data[offset:offset + SPOOL_RECORD_BYTES]
        for offset in range(0, len(data), SPOOL_RECORD_BYTES)
    ]


def _iter_fixed_records(path: Path) -> Iterator[bytes]:
    with path.open("rb", buffering=1024 * 1024) as handle:
        while True:
            record = handle.read(SPOOL_RECORD_BYTES)
            if not record:
                return
            if len(record) != SPOOL_RECORD_BYTES:
                raise FormatError(f"partial record in temporary run {path}")
            yield record


def _merge_runs(paths: Sequence[Path], output: Path | None = None) -> Iterator[bytes] | None:
    def merged() -> Iterator[bytes]:
        handles = [path.open("rb", buffering=1024 * 1024) for path in paths]
        heap: list[tuple[bytes, int, bytes]] = []
        try:
            for index, handle in enumerate(handles):
                record = handle.read(SPOOL_RECORD_BYTES)
                if record:
                    if len(record) != SPOOL_RECORD_BYTES:
                        raise FormatError(f"partial record in temporary run {paths[index]}")
                    heapq.heappush(heap, (record[:16], index, record))
            while heap:
                _, index, record = heapq.heappop(heap)
                yield record
                next_record = handles[index].read(SPOOL_RECORD_BYTES)
                if next_record:
                    if len(next_record) != SPOOL_RECORD_BYTES:
                        raise FormatError(f"partial record in temporary run {paths[index]}")
                    heapq.heappush(heap, (next_record[:16], index, next_record))
        finally:
            for handle in handles:
                handle.close()

    if output is None:
        return merged()
    with output.open("wb", buffering=1024 * 1024) as target:
        target.writelines(merged())
    return None


def sort_bucket(
    bucket_path: Path,
    temp_root: Path,
    bucket_index: int,
    run_points: int,
    merge_fan_in: int,
) -> list[Path]:
    runs: list[Path] = []
    with bucket_path.open("rb", buffering=1024 * 1024) as handle:
        run_index = 0
        while True:
            records = _read_fixed_records(handle, run_points)
            if not records:
                break
            records.sort()
            run_path = temp_root / f"bucket-{bucket_index:05d}-run-{run_index:05d}.bin"
            with run_path.open("wb", buffering=1024 * 1024) as run:
                run.writelines(records)
            runs.append(run_path)
            run_index += 1
    bucket_path.unlink()

    pass_index = 0
    while len(runs) > merge_fan_in:
        next_runs: list[Path] = []
        for group_index in range(0, len(runs), merge_fan_in):
            group = runs[group_index:group_index + merge_fan_in]
            merged_path = temp_root / (
                f"bucket-{bucket_index:05d}-merge-{pass_index:03d}-{group_index // merge_fan_in:05d}.bin"
            )
            _merge_runs(group, merged_path)
            next_runs.append(merged_path)
            for path in group:
                path.unlink()
        runs = next_runs
        pass_index += 1
    return runs


def _rotated_gaussian_extent(
    rotation: Sequence[float], scale: Sequence[float], sigma: float = SUPPORT_SIGMA
) -> tuple[float, float, float]:
    x, y, z, w = rotation
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    matrix = (
        (1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz), 2.0 * (xz + wy)),
        (2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx)),
        (2.0 * (xz - wy), 2.0 * (yz + wx), 1.0 - 2.0 * (xx + yy)),
    )
    scale_squared = (scale[0] * scale[0], scale[1] * scale[1], scale[2] * scale[2])
    return tuple(
        sigma * math.sqrt(sum(row[column] * row[column] * scale_squared[column] for column in range(3)))
        for row in matrix
    )


def _page_stream_payloads(records: Sequence[bytes]) -> tuple[list[bytearray], PageRecord]:
    count = len(records)
    payloads = [
        bytearray(count * 12),
        bytearray(count * 16),
        bytearray(count * 12),
        bytearray(count * 4),
        bytearray(count * 96),
        bytearray(count * 8),
    ]
    center_min = [math.inf, math.inf, math.inf]
    center_max = [-math.inf, -math.inf, -math.inf]
    support_min = [math.inf, math.inf, math.inf]
    support_max = [-math.inf, -math.inf, -math.inf]
    max_scale_cm = 0.0

    for output_index, record in enumerate(records):
        if len(record) != SPOOL_RECORD_BYTES:
            raise FormatError("invalid in-memory spool record")
        _, source_id = SPOOL_PREFIX.unpack_from(record)
        body = memoryview(record)[SPOOL_PREFIX.size:]
        position = struct.unpack_from("<3f", body, 0)
        rotation = struct.unpack_from("<4f", body, 12)
        scale = struct.unpack_from("<3f", body, 28)
        extent = _rotated_gaussian_extent(rotation, scale)
        max_scale_cm = max(max_scale_cm, *scale)
        for axis in range(3):
            center_min[axis] = min(center_min[axis], position[axis])
            center_max[axis] = max(center_max[axis], position[axis])
            support_min[axis] = min(support_min[axis], position[axis] - extent[axis])
            support_max[axis] = max(support_max[axis], position[axis] + extent[axis])

        payloads[0][output_index * 12:(output_index + 1) * 12] = body[0:12]
        payloads[1][output_index * 16:(output_index + 1) * 16] = body[12:28]
        payloads[2][output_index * 12:(output_index + 1) * 12] = body[28:40]
        payloads[3][output_index * 4:(output_index + 1) * 4] = body[40:44]
        payloads[4][output_index * 96:(output_index + 1) * 96] = body[44:140]
        struct.pack_into("<Q", payloads[5], output_index * 8, source_id)

    placeholder = PageRecord(
        page_id=0,
        first_output_index=0,
        splat_count=count,
        stream_start=0,
        stream_count=len(STREAM_SPECS),
        center_min=tuple(center_min),
        center_max=tuple(center_max),
        support_min=tuple(support_min),
        support_max=tuple(support_max),
        support_sigma=SUPPORT_SIGMA,
        max_scale_cm=max_scale_cm,
    )
    return payloads, placeholder


def _write_page(
    target: BinaryIO,
    records: Sequence[bytes],
    page_id: int,
    first_output_index: int,
    streams: list[StreamRecord],
) -> tuple[PageRecord, dict[str, object]]:
    payloads, page = _page_stream_payloads(records)
    page.page_id = page_id
    page.first_output_index = first_output_index
    page.stream_start = len(streams)
    page_hasher = hashlib.sha256()
    manifest_streams: list[dict[str, object]] = []

    for spec, payload in zip(STREAM_SPECS, payloads):
        semantic, encoding, components, stride, name = spec
        _write_alignment(target, PAYLOAD_ALIGNMENT)
        offset = target.tell()
        target.write(payload)
        payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
        page_hasher.update(payload)
        stream = StreamRecord(
            page_id=page_id,
            semantic=semantic,
            encoding=encoding,
            element_count=len(records),
            component_count=components,
            element_stride_bytes=stride,
            file_offset=offset,
            byte_size=len(payload),
            uncompressed_byte_size=len(payload),
            payload_crc32=payload_crc,
        )
        streams.append(stream)
        manifest_streams.append(_stream_to_dict(stream, name))

    manifest_page = _page_to_dict(page)
    manifest_page["payload_sha256"] = page_hasher.hexdigest()
    manifest_page["streams"] = manifest_streams
    return page, manifest_page


def _pack_header(
    *,
    file_bytes: int,
    page_count: int,
    page_directory_offset: int,
    stream_count: int,
    stream_directory_offset: int,
    payload_offset: int,
    payload_bytes: int,
    total_splat_count: int,
    scene_bounds_min: Sequence[float],
    scene_bounds_max: Sequence[float],
    directory_crc32: int,
) -> bytes:
    data = bytearray(HEADER_BYTES)
    data[0:8] = MAGIC
    struct.pack_into("<4I", data, 8, VERSION, ENDIAN_TAG, PAYLOAD_FORMAT,
                     HEADER_FLAG_STREAM_CRC32 | HEADER_FLAG_DENSE_SOURCE_IDS | HEADER_FLAG_PAYLOAD_ALIGNED_64)
    struct.pack_into(
        "<11Q",
        data,
        24,
        HEADER_BYTES,
        file_bytes,
        page_count,
        page_directory_offset,
        PAGE_RECORD_BYTES,
        stream_count,
        stream_directory_offset,
        STREAM_RECORD_BYTES,
        payload_offset,
        payload_bytes,
        total_splat_count,
    )
    struct.pack_into("<6d", data, 112, *scene_bounds_min, *scene_bounds_max)
    struct.pack_into("<2d", data, 160, SUPPORT_SIGMA, 1.0)
    struct.pack_into(
        "<5I", data, 176, COORDINATE_SYSTEM, SH_ORDER, SH_LAYOUT, directory_crc32, 0
    )
    header_crc = zlib.crc32(data) & 0xFFFFFFFF
    struct.pack_into("<I", data, 192, header_crc)
    return bytes(data)


def _layout_estimate(total: int, page_points: int) -> dict[str, int]:
    page_count = (total + page_points - 1) // page_points
    stream_count = page_count * len(STREAM_SPECS)
    page_directory_offset = HEADER_BYTES
    stream_directory_offset = page_directory_offset + page_count * PAGE_RECORD_BYTES
    payload_offset = _align_up(
        stream_directory_offset + stream_count * STREAM_RECORD_BYTES,
        FILE_PAYLOAD_ALIGNMENT,
    )
    cursor = payload_offset
    remaining = total
    for _ in range(page_count):
        count = min(page_points, remaining)
        for _, _, _, stride, _ in STREAM_SPECS:
            cursor = _align_up(cursor, PAYLOAD_ALIGNMENT)
            cursor += count * stride
        remaining -= count
    return {
        "page_count": page_count,
        "stream_count": stream_count,
        "page_directory_offset": page_directory_offset,
        "stream_directory_offset": stream_directory_offset,
        "payload_offset": payload_offset,
        "payload_bytes": cursor - payload_offset,
        "file_bytes": cursor,
        "raw_payload_bytes": total * 148,
    }


def _auto_bucket_count(total: int, page_points: int) -> int:
    # Average eight pages per bucket; skew is handled by external sort runs.
    desired = max(1, (total + page_points * 8 - 1) // (page_points * 8))
    return min(4096, _next_power_of_two(desired))


def estimate(path: Path | str, page_points: int = 65536, bucket_count: int | None = None) -> dict[str, object]:
    if page_points <= 0:
        raise ValueError("page_points must be positive")
    header = parse_ply_header(path)
    bucket_count = bucket_count or _auto_bucket_count(header.vertex_count, page_points)
    if not _is_power_of_two(bucket_count):
        raise ValueError("bucket_count must be a power of two")
    layout = _layout_estimate(header.vertex_count, page_points)
    return {
        "source": str(header.path),
        "source_file_bytes": header.file_size,
        "source_vertex_count": header.vertex_count,
        "source_vertex_stride": header.vertex_stride,
        "page_points": page_points,
        "bucket_count": bucket_count,
        **layout,
        "peak_page_payload_bytes": min(page_points, header.vertex_count) * 148,
        "temporary_spool_bytes": header.vertex_count * SPOOL_RECORD_BYTES,
        "temporary_peak_note": "spool plus sorted runs for the active bucket; not full decoded payload in RAM",
    }


def build_container(
    source_path: Path | str,
    output_path: Path | str,
    *,
    page_points: int = 65536,
    bucket_count: int | None = None,
    chunk_points: int = 32768,
    sort_run_points: int = 262144,
    merge_fan_in: int = 64,
    temp_dir: Path | str | None = None,
    keep_temp: bool = False,
    force: bool = False,
    compute_file_sha256: bool = True,
    manifest_path: Path | str | None = None,
) -> dict[str, object]:
    if page_points <= 0 or chunk_points <= 0 or sort_run_points <= 0:
        raise ValueError("page_points, chunk_points, and sort_run_points must be positive")
    if merge_fan_in < 2:
        raise ValueError("merge_fan_in must be at least 2")
    source_path = Path(source_path)
    output_path = Path(output_path)
    manifest_path = Path(manifest_path) if manifest_path else Path(str(output_path) + ".json")
    partial_path = Path(str(output_path) + ".partial")
    if not force and (output_path.exists() or partial_path.exists() or manifest_path.exists()):
        raise FileExistsError("output, partial output, or manifest already exists; pass --force")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    if force:
        partial_path.unlink(missing_ok=True)

    header = parse_ply_header(source_path)
    bucket_count = bucket_count or _auto_bucket_count(header.vertex_count, page_points)
    if not _is_power_of_two(bucket_count) or bucket_count > 4096:
        raise ValueError("bucket_count must be a power of two no greater than 4096")
    layout = _layout_estimate(header.vertex_count, page_points)
    started = time.monotonic()
    print("Pass 1/2: scanning source bounds and SHA-256", file=sys.stderr)
    center_bounds_min, center_bounds_max, source_sha256 = scan_source(header, chunk_points)

    temp_parent = Path(temp_dir) if temp_dir else output_path.parent
    temp_parent.mkdir(parents=True, exist_ok=True)
    temp_root = Path(tempfile.mkdtemp(prefix="nanogs-page-v1-", dir=temp_parent))
    pages: list[PageRecord] = []
    streams: list[StreamRecord] = []
    manifest_pages: list[dict[str, object]] = []
    global_support_min = [math.inf, math.inf, math.inf]
    global_support_max = [-math.inf, -math.inf, -math.inf]

    try:
        print(
            f"Pass 2/2: decoding into {bucket_count} external Morton buckets",
            file=sys.stderr,
        )
        bucket_counts = bucket_source(
            header,
            temp_root,
            bucket_count,
            chunk_points,
            center_bounds_min,
            center_bounds_max,
        )
        if sum(bucket_counts) != header.vertex_count:
            raise FormatError("bucket point conservation failed")

        with partial_path.open("w+b", buffering=1024 * 1024) as target:
            target.write(b"\0" * layout["payload_offset"])
            page_records: list[bytes] = []
            output_count = 0
            page_buffer: list[bytes] = []

            for bucket_index, bucket_points in enumerate(bucket_counts):
                if bucket_points == 0:
                    continue
                print(
                    f"Sorting Morton bucket {bucket_index + 1}/{bucket_count} "
                    f"({bucket_points:,} splats)",
                    file=sys.stderr,
                )
                bucket_path = temp_root / f"bucket-{bucket_index:05d}.spool"
                runs = sort_bucket(
                    bucket_path,
                    temp_root,
                    bucket_index,
                    sort_run_points,
                    merge_fan_in,
                )
                record_iterator = _merge_runs(runs)
                assert record_iterator is not None
                for record in record_iterator:
                    page_buffer.append(record)
                    if len(page_buffer) == page_points:
                        page, manifest_page = _write_page(
                            target, page_buffer, len(pages), output_count, streams
                        )
                        pages.append(page)
                        manifest_pages.append(manifest_page)
                        page_records.append(page.pack())
                        output_count += len(page_buffer)
                        for axis in range(3):
                            global_support_min[axis] = min(global_support_min[axis], page.support_min[axis])
                            global_support_max[axis] = max(global_support_max[axis], page.support_max[axis])
                        page_buffer.clear()
                        _progress("write", output_count, header.vertex_count)
                for run in runs:
                    run.unlink()

            if page_buffer:
                page, manifest_page = _write_page(
                    target, page_buffer, len(pages), output_count, streams
                )
                pages.append(page)
                manifest_pages.append(manifest_page)
                page_records.append(page.pack())
                output_count += len(page_buffer)
                for axis in range(3):
                    global_support_min[axis] = min(global_support_min[axis], page.support_min[axis])
                    global_support_max[axis] = max(global_support_max[axis], page.support_max[axis])
                page_buffer.clear()
                _progress("write", output_count, header.vertex_count)

            if output_count != header.vertex_count:
                raise FormatError(
                    f"output point conservation failed: {output_count} != {header.vertex_count}"
                )
            if len(pages) != layout["page_count"] or len(streams) != layout["stream_count"]:
                raise FormatError("predicted directory sizes do not match generated directories")

            page_directory = b"".join(page_records)
            stream_directory = b"".join(stream.pack() for stream in streams)
            directory_bytes = page_directory + stream_directory
            directory_crc = zlib.crc32(directory_bytes) & 0xFFFFFFFF
            file_bytes = target.tell()
            payload_bytes = file_bytes - layout["payload_offset"]
            packed_header = _pack_header(
                file_bytes=file_bytes,
                page_count=len(pages),
                page_directory_offset=layout["page_directory_offset"],
                stream_count=len(streams),
                stream_directory_offset=layout["stream_directory_offset"],
                payload_offset=layout["payload_offset"],
                payload_bytes=payload_bytes,
                total_splat_count=output_count,
                scene_bounds_min=global_support_min,
                scene_bounds_max=global_support_max,
                directory_crc32=directory_crc,
            )
            target.seek(0)
            target.write(packed_header)
            target.seek(layout["page_directory_offset"])
            target.write(page_directory)
            target.seek(layout["stream_directory_offset"])
            target.write(stream_directory)
            target.flush()
            os.fsync(target.fileno())

        os.replace(partial_path, output_path)
        container_sha256 = _sha256_file(output_path) if compute_file_sha256 else None
        elapsed = time.monotonic() - started
        manifest: dict[str, object] = {
            "schema": "NanoGSPageManifest",
            "version": VERSION,
            "container_file": output_path.name,
            "container_file_bytes": output_path.stat().st_size,
            "container_sha256": container_sha256,
            "source": {
                "file_name": source_path.name,
                "file_bytes": header.file_size,
                "vertex_count": header.vertex_count,
                "vertex_stride": header.vertex_stride,
                "sha256": source_sha256,
                "format": "binary_little_endian 1.0 full SH3 3DGS PLY",
            },
            "format": {
                "magic": MAGIC.decode("ascii"),
                "payload_format": "LosslessGeometry_SH3Half_RGBA8_SoA",
                "header_bytes": HEADER_BYTES,
                "page_record_bytes": PAGE_RECORD_BYTES,
                "stream_record_bytes": STREAM_RECORD_BYTES,
                "payload_alignment_bytes": PAYLOAD_ALIGNMENT,
                "bytes_per_splat": 148,
                "page_points_target": page_points,
                "support_sigma": SUPPORT_SIGMA,
                "bounds_method": "union of rotated Gaussian 2-sigma ellipsoid AABBs",
                "stream_specs": [
                    {
                        "semantic": semantic,
                        "encoding": encoding,
                        "components": components,
                        "stride_bytes": stride,
                        "name": name,
                    }
                    for semantic, encoding, components, stride, name in STREAM_SPECS
                ],
            },
            "coordinate_contract": {
                "payload_space": "NanoGS asset local space",
                "basis": "source PLY XYZ preserved; no axis swap or mirror",
                "position_unit": "centimeters",
                "position_transform": "source position * 100",
                "rotation_transform": "source WXYZ -> normalized XYZW; basis unchanged",
                "scale_transform": "exp(source log-scale) * 100 centimeters",
                "opacity_transform": "sigmoid, then FFloat16 roundtrip and RGBA8 quantization",
                "lcc_x_mirror": "not baked; apply only through actor/runtime metadata when required",
            },
            "directory": {
                "page_count": len(pages),
                "page_directory_offset": layout["page_directory_offset"],
                "stream_count": len(streams),
                "stream_directory_offset": layout["stream_directory_offset"],
                "payload_offset": layout["payload_offset"],
                "payload_bytes": payload_bytes,
                "directory_crc32": f"{directory_crc:08x}",
            },
            "scene_support_bounds_cm": {
                "min": global_support_min,
                "max": global_support_max,
            },
            "pages": manifest_pages,
            "build": {
                "external_sort": "two-pass high-Morton bucket + bounded runs + k-way merge",
                "bucket_count": bucket_count,
                "sort_run_points": sort_run_points,
                "elapsed_seconds": elapsed,
            },
        }
        manifest_partial = Path(str(manifest_path) + ".partial")
        with manifest_partial.open("w", encoding="utf-8") as handle:
            json.dump(manifest, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
        os.replace(manifest_partial, manifest_path)
        print(
            f"Built {output_path} ({output_count:,} splats, {len(pages):,} pages, "
            f"{file_bytes / (1024 ** 3):.3f} GiB)",
            file=sys.stderr,
        )
        return manifest
    except Exception:
        partial_path.unlink(missing_ok=True)
        raise
    finally:
        if keep_temp:
            print(f"Keeping temporary data at {temp_root}", file=sys.stderr)
        else:
            shutil.rmtree(temp_root, ignore_errors=True)


def _page_to_dict(page: PageRecord) -> dict[str, object]:
    return {
        "page_id": page.page_id,
        "first_output_index": page.first_output_index,
        "splat_count": page.splat_count,
        "stream_start": page.stream_start,
        "stream_count": page.stream_count,
        "center_bounds_cm": {"min": page.center_min, "max": page.center_max},
        "support_bounds_cm": {"min": page.support_min, "max": page.support_max},
        "support_sigma": page.support_sigma,
        "max_scale_cm": page.max_scale_cm,
        "flags": page.flags,
    }


def _stream_to_dict(stream: StreamRecord, name: str | None = None) -> dict[str, object]:
    return {
        "page_id": stream.page_id,
        "semantic": stream.semantic,
        "name": name or SEMANTIC_NAMES.get(stream.semantic, "unknown"),
        "encoding": stream.encoding,
        "element_count": stream.element_count,
        "component_count": stream.component_count,
        "element_stride_bytes": stream.element_stride_bytes,
        "file_offset": stream.file_offset,
        "byte_size": stream.byte_size,
        "uncompressed_byte_size": stream.uncompressed_byte_size,
        "payload_crc32": f"{stream.payload_crc32:08x}",
        "flags": stream.flags,
    }


def _unpack_header(data: bytes) -> dict[str, object]:
    if len(data) != HEADER_BYTES:
        raise VerificationError("short NanoGS Page header")
    version, endian, payload_format, flags = struct.unpack_from("<4I", data, 8)
    values = struct.unpack_from("<11Q", data, 24)
    bounds = struct.unpack_from("<6d", data, 112)
    support_sigma, units_to_cm = struct.unpack_from("<2d", data, 160)
    coordinate_system, sh_order, sh_layout, directory_crc, header_crc = struct.unpack_from(
        "<5I", data, 176
    )
    return {
        "magic": data[:8],
        "version": version,
        "endian": endian,
        "payload_format": payload_format,
        "flags": flags,
        "header_bytes": values[0],
        "file_bytes": values[1],
        "page_count": values[2],
        "page_directory_offset": values[3],
        "page_record_bytes": values[4],
        "stream_count": values[5],
        "stream_directory_offset": values[6],
        "stream_record_bytes": values[7],
        "payload_offset": values[8],
        "payload_bytes": values[9],
        "total_splat_count": values[10],
        "scene_bounds_min": bounds[:3],
        "scene_bounds_max": bounds[3:],
        "support_sigma": support_sigma,
        "units_to_cm": units_to_cm,
        "coordinate_system": coordinate_system,
        "sh_order": sh_order,
        "sh_layout": sh_layout,
        "directory_crc32": directory_crc,
        "header_crc32": header_crc,
    }


def _unpack_page(data: bytes) -> PageRecord:
    values = PAGE_STRUCT.unpack(data)
    return PageRecord(
        page_id=values[0],
        first_output_index=values[1],
        splat_count=values[2],
        stream_start=values[3],
        stream_count=values[4],
        center_min=values[5:8],
        center_max=values[8:11],
        support_min=values[11:14],
        support_max=values[14:17],
        support_sigma=values[17],
        max_scale_cm=values[18],
        flags=values[19],
        reserved=values[20],
    )


def _unpack_stream(data: bytes) -> StreamRecord:
    values = STREAM_STRUCT.unpack(data)
    return StreamRecord(*values)


def read_container_directories(
    path: Path | str,
) -> tuple[dict[str, object], list[PageRecord], list[StreamRecord], bytes, bytes]:
    path = Path(path)
    with path.open("rb") as handle:
        raw_header = handle.read(HEADER_BYTES)
        header = _unpack_header(raw_header)
        if header["page_count"] > 100_000_000 or header["stream_count"] > 1_000_000_000:
            raise VerificationError("unreasonable directory count")
        handle.seek(int(header["page_directory_offset"]))
        raw_pages = handle.read(int(header["page_count"]) * PAGE_RECORD_BYTES)
        handle.seek(int(header["stream_directory_offset"]))
        raw_streams = handle.read(int(header["stream_count"]) * STREAM_RECORD_BYTES)
    if len(raw_pages) != int(header["page_count"]) * PAGE_RECORD_BYTES:
        raise VerificationError("short page directory")
    if len(raw_streams) != int(header["stream_count"]) * STREAM_RECORD_BYTES:
        raise VerificationError("short stream directory")
    pages = [
        _unpack_page(raw_pages[offset:offset + PAGE_RECORD_BYTES])
        for offset in range(0, len(raw_pages), PAGE_RECORD_BYTES)
    ]
    streams = [
        _unpack_stream(raw_streams[offset:offset + STREAM_RECORD_BYTES])
        for offset in range(0, len(raw_streams), STREAM_RECORD_BYTES)
    ]
    return header, pages, streams, raw_header, raw_pages + raw_streams


def _crc32_range(handle: BinaryIO, offset: int, size: int) -> int:
    handle.seek(offset)
    crc = 0
    remaining = size
    while remaining:
        block = handle.read(min(8 * 1024 * 1024, remaining))
        if not block:
            raise VerificationError(f"short payload read at offset {offset}")
        crc = zlib.crc32(block, crc)
        remaining -= len(block)
    return crc & 0xFFFFFFFF


def _bounds_close(actual: float, declared: float) -> bool:
    return math.isclose(actual, declared, rel_tol=1.0e-10, abs_tol=1.0e-7)


def verify_container(
    path: Path | str,
    *,
    manifest_path: Path | str | None = None,
    verify_sha256: bool = True,
) -> dict[str, object]:
    path = Path(path)
    file_size = path.stat().st_size
    header, pages, streams, raw_header, raw_directories = read_container_directories(path)
    errors: list[str] = []

    def check(condition: bool, message: str) -> None:
        if not condition:
            errors.append(message)

    check(header["magic"] == MAGIC, "bad magic")
    check(header["version"] == VERSION, "unsupported version")
    check(header["endian"] == ENDIAN_TAG, "bad endian tag")
    check(header["payload_format"] == PAYLOAD_FORMAT, "unsupported payload format")
    check(
        header["flags"]
        == (HEADER_FLAG_STREAM_CRC32 | HEADER_FLAG_DENSE_SOURCE_IDS | HEADER_FLAG_PAYLOAD_ALIGNED_64),
        "unsupported header flags",
    )
    check(header["header_bytes"] == HEADER_BYTES, "bad header size")
    check(header["page_record_bytes"] == PAGE_RECORD_BYTES, "bad page record size")
    check(header["stream_record_bytes"] == STREAM_RECORD_BYTES, "bad stream record size")
    check(header["file_bytes"] == file_size, "header file size mismatch")
    page_directory_end = header["page_directory_offset"] + header["page_count"] * PAGE_RECORD_BYTES
    stream_directory_end = header["stream_directory_offset"] + header["stream_count"] * STREAM_RECORD_BYTES
    check(header["page_directory_offset"] >= HEADER_BYTES, "page directory overlaps header")
    check(
        page_directory_end <= header["stream_directory_offset"],
        "page and stream directories overlap or are reversed",
    )
    check(
        stream_directory_end <= header["payload_offset"],
        "stream directory overlaps payload",
    )
    check(header["payload_offset"] % PAYLOAD_ALIGNMENT == 0, "payload offset is not 64-byte aligned")
    check(
        header["payload_offset"] + header["payload_bytes"] == file_size,
        "payload range does not end at EOF",
    )
    check(header["support_sigma"] == SUPPORT_SIGMA, "unsupported support sigma")
    check(header["units_to_cm"] == 1.0, "payload unit is not centimeters")
    check(header["coordinate_system"] == COORDINATE_SYSTEM, "coordinate system mismatch")
    check(header["sh_order"] == SH_ORDER and header["sh_layout"] == SH_LAYOUT, "SH contract mismatch")
    check(raw_header[196:] == b"\0" * 60, "header reserved bytes are non-zero")
    check(
        all(math.isfinite(value) for value in (*header["scene_bounds_min"], *header["scene_bounds_max"])),
        "scene bounds contain non-finite values",
    )
    for axis in range(3):
        check(
            header["scene_bounds_min"][axis] <= header["scene_bounds_max"][axis],
            f"scene bounds are reversed on axis {axis}",
        )

    header_for_crc = bytearray(raw_header)
    struct.pack_into("<I", header_for_crc, 192, 0)
    check(
        (zlib.crc32(header_for_crc) & 0xFFFFFFFF) == header["header_crc32"],
        "header CRC32 mismatch",
    )
    check(
        (zlib.crc32(raw_directories) & 0xFFFFFFFF) == header["directory_crc32"],
        "directory CRC32 mismatch",
    )
    check(len(pages) == header["page_count"], "page count mismatch")
    check(len(streams) == header["stream_count"], "stream count mismatch")

    # Reject impossible counts before allocating the dense ID bitset.  Every
    # valid v1 splat contributes exactly 148 payload bytes (alignment only adds).
    if int(header["total_splat_count"]) * 148 > int(header["payload_bytes"]):
        raise VerificationError("total splat count cannot fit in declared payload bytes")

    expected_output_index = 0
    expected_stream_index = 0
    total_count = 0
    payload_ranges: list[tuple[int, int, str]] = []
    seen_ids = bytearray((int(header["total_splat_count"]) + 7) // 8)
    unique_ids = 0
    global_support_min = [math.inf, math.inf, math.inf]
    global_support_max = [-math.inf, -math.inf, -math.inf]

    with path.open("rb") as handle:
        for page_index, page in enumerate(pages):
            check(page.page_id == page_index, f"page {page_index}: non-contiguous page ID")
            check(
                page.first_output_index == expected_output_index,
                f"page {page_index}: first output index mismatch",
            )
            check(page.splat_count > 0, f"page {page_index}: empty page")
            check(
                page.stream_start == expected_stream_index,
                f"page {page_index}: stream directory indices are not contiguous",
            )
            check(page.stream_count == len(STREAM_SPECS), f"page {page_index}: stream count is not six")
            check(page.support_sigma == SUPPORT_SIGMA, f"page {page_index}: support sigma mismatch")
            check(
                page.flags == PAGE_FLAG_ROTATED_GAUSSIAN_AABB_2SIGMA,
                f"page {page_index}: unsupported page flags",
            )
            check(page.reserved == 0, f"page {page_index}: reserved field is non-zero")
            check(
                math.isfinite(page.max_scale_cm) and page.max_scale_cm >= 0.0,
                f"page {page_index}: invalid max scale",
            )
            check(
                all(
                    math.isfinite(value)
                    for value in (*page.center_min, *page.center_max, *page.support_min, *page.support_max)
                ),
                f"page {page_index}: non-finite bounds",
            )
            for axis in range(3):
                check(
                    page.center_min[axis] <= page.center_max[axis]
                    and page.support_min[axis] <= page.support_max[axis],
                    f"page {page_index}: reversed bounds on axis {axis}",
                )
            expected_output_index += page.splat_count
            expected_stream_index += page.stream_count
            total_count += page.splat_count

            stream_end = page.stream_start + page.stream_count
            if stream_end > len(streams):
                errors.append(f"page {page_index}: stream directory range out of bounds")
                continue
            page_streams = streams[page.stream_start:stream_end]
            by_semantic: dict[int, StreamRecord] = {}
            for stream in page_streams:
                if stream.semantic in by_semantic:
                    errors.append(f"page {page_index}: duplicate semantic {stream.semantic}")
                by_semantic[stream.semantic] = stream
                check(stream.page_id == page.page_id, f"page {page_index}: stream page ID mismatch")
                check(stream.flags == 0, f"page {page_index}: stream flags are non-zero")
                check(stream.reserved == 0, f"page {page_index}: stream reserved field is non-zero")
                spec = STREAM_SPEC_BY_SEMANTIC.get(stream.semantic)
                if spec is None:
                    errors.append(f"page {page_index}: unknown semantic {stream.semantic}")
                    continue
                _, encoding, components, stride, name = spec
                check(stream.encoding == encoding, f"page {page_index}/{name}: encoding mismatch")
                check(stream.element_count == page.splat_count, f"page {page_index}/{name}: count mismatch")
                check(stream.component_count == components, f"page {page_index}/{name}: component mismatch")
                check(stream.element_stride_bytes == stride, f"page {page_index}/{name}: stride mismatch")
                expected_bytes = page.splat_count * stride
                check(stream.byte_size == expected_bytes, f"page {page_index}/{name}: byte size mismatch")
                check(
                    stream.uncompressed_byte_size == stream.byte_size,
                    f"page {page_index}/{name}: v1 stream unexpectedly compressed",
                )
                check(stream.file_offset % PAYLOAD_ALIGNMENT == 0, f"page {page_index}/{name}: misaligned offset")
                range_end = stream.file_offset + stream.byte_size
                check(
                    stream.file_offset >= header["payload_offset"] and range_end <= file_size,
                    f"page {page_index}/{name}: payload range outside file",
                )
                payload_ranges.append((stream.file_offset, range_end, f"page {page_index}/{name}"))
                if range_end <= file_size:
                    actual_crc = _crc32_range(handle, stream.file_offset, stream.byte_size)
                    check(actual_crc == stream.payload_crc32, f"page {page_index}/{name}: CRC32 mismatch")

            missing = set(STREAM_SPEC_BY_SEMANTIC) - set(by_semantic)
            check(not missing, f"page {page_index}: missing semantics {sorted(missing)}")
            if missing:
                continue

            position_stream = by_semantic[SEMANTIC_POSITION]
            rotation_stream = by_semantic[SEMANTIC_ROTATION]
            scale_stream = by_semantic[SEMANTIC_SCALE]
            id_stream = by_semantic[SEMANTIC_ORIGINAL_ID]
            handle.seek(position_stream.file_offset)
            position_data = handle.read(position_stream.byte_size)
            handle.seek(rotation_stream.file_offset)
            rotation_data = handle.read(rotation_stream.byte_size)
            handle.seek(scale_stream.file_offset)
            scale_data = handle.read(scale_stream.byte_size)
            handle.seek(id_stream.file_offset)
            id_data = handle.read(id_stream.byte_size)
            if any(
                len(data) != expected
                for data, expected in (
                    (position_data, position_stream.byte_size),
                    (rotation_data, rotation_stream.byte_size),
                    (scale_data, scale_stream.byte_size),
                    (id_data, id_stream.byte_size),
                )
            ):
                errors.append(f"page {page_index}: short stream read during bounds verification")
                continue

            actual_center_min = [math.inf, math.inf, math.inf]
            actual_center_max = [-math.inf, -math.inf, -math.inf]
            actual_support_min = [math.inf, math.inf, math.inf]
            actual_support_max = [-math.inf, -math.inf, -math.inf]
            actual_max_scale = 0.0
            for item in range(page.splat_count):
                position = struct.unpack_from("<3f", position_data, item * 12)
                rotation = struct.unpack_from("<4f", rotation_data, item * 16)
                scale = struct.unpack_from("<3f", scale_data, item * 12)
                source_id = struct.unpack_from("<Q", id_data, item * 8)[0]
                if source_id >= header["total_splat_count"]:
                    errors.append(f"page {page_index}: source ID {source_id} outside dense range")
                else:
                    byte_index, bit_index = divmod(source_id, 8)
                    mask = 1 << bit_index
                    if seen_ids[byte_index] & mask:
                        errors.append(f"page {page_index}: duplicate source ID {source_id}")
                    else:
                        seen_ids[byte_index] |= mask
                        unique_ids += 1
                if not all(math.isfinite(value) for value in (*position, *rotation, *scale)):
                    errors.append(f"page {page_index}: non-finite geometry at item {item}")
                    continue
                extent = _rotated_gaussian_extent(rotation, scale)
                actual_max_scale = max(actual_max_scale, *scale)
                for axis in range(3):
                    actual_center_min[axis] = min(actual_center_min[axis], position[axis])
                    actual_center_max[axis] = max(actual_center_max[axis], position[axis])
                    actual_support_min[axis] = min(actual_support_min[axis], position[axis] - extent[axis])
                    actual_support_max[axis] = max(actual_support_max[axis], position[axis] + extent[axis])

            for axis in range(3):
                check(
                    _bounds_close(actual_center_min[axis], page.center_min[axis])
                    and _bounds_close(actual_center_max[axis], page.center_max[axis]),
                    f"page {page_index}: center bounds mismatch on axis {axis}",
                )
                check(
                    _bounds_close(actual_support_min[axis], page.support_min[axis])
                    and _bounds_close(actual_support_max[axis], page.support_max[axis]),
                    f"page {page_index}: support bounds mismatch on axis {axis}",
                )
                global_support_min[axis] = min(global_support_min[axis], actual_support_min[axis])
                global_support_max[axis] = max(global_support_max[axis], actual_support_max[axis])
            check(
                _bounds_close(actual_max_scale, page.max_scale_cm),
                f"page {page_index}: max scale mismatch",
            )

    payload_ranges.sort()
    for previous, current in zip(payload_ranges, payload_ranges[1:]):
        check(previous[1] <= current[0], f"overlapping payload ranges: {previous[2]} and {current[2]}")
    check(total_count == header["total_splat_count"], "point count is not conserved")
    check(expected_output_index == header["total_splat_count"], "output indices are not contiguous")
    check(unique_ids == header["total_splat_count"], "source IDs are not a complete unique dense set")
    for axis in range(3):
        check(
            _bounds_close(global_support_min[axis], header["scene_bounds_min"][axis])
            and _bounds_close(global_support_max[axis], header["scene_bounds_max"][axis]),
            f"scene support bounds mismatch on axis {axis}",
        )

    manifest: dict[str, object] | None = None
    resolved_manifest = Path(manifest_path) if manifest_path else Path(str(path) + ".json")
    if resolved_manifest.exists():
        with resolved_manifest.open("r", encoding="utf-8") as handle:
            manifest = json.load(handle)
        check(manifest.get("container_file_bytes") == file_size, "manifest file size mismatch")
        expected_sha = manifest.get("container_sha256")
        if verify_sha256 and expected_sha:
            check(_sha256_file(path) == expected_sha, "container SHA-256 mismatch")

    if errors:
        preview = "\n  - ".join(errors[:50])
        suffix = "" if len(errors) <= 50 else f"\n  ... {len(errors) - 50} more"
        raise VerificationError(f"verification failed ({len(errors)} errors):\n  - {preview}{suffix}")
    result = {
        "file": str(path),
        "file_bytes": file_size,
        "splat_count": total_count,
        "page_count": len(pages),
        "stream_count": len(streams),
        "unique_source_ids": unique_ids,
        "crc32": "ok",
        "bounds": "ok",
        "sha256": "ok" if verify_sha256 and manifest and manifest.get("container_sha256") else "not checked",
    }
    return result


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            block = handle.read(8 * 1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def _progress(stage: str, completed: int, total: int) -> None:
    interval = max(1, total // 20)
    if completed == total or completed % interval < 32768:
        print(f"  {stage}: {completed:,}/{total:,} ({completed * 100.0 / total:.1f}%)", file=sys.stderr)


def _make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="External-memory NanoGS Page v1 builder and verifier"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    estimate_parser = subparsers.add_parser("estimate", help="inspect PLY and estimate output/disk sizes")
    estimate_parser.add_argument("source", type=Path)
    estimate_parser.add_argument("--page-points", type=int, default=65536)
    estimate_parser.add_argument("--bucket-count", type=int)

    build_parser = subparsers.add_parser("build", help="build a streamable .ngsp container")
    build_parser.add_argument("source", type=Path)
    build_parser.add_argument("output", type=Path)
    build_parser.add_argument("--page-points", type=int, default=65536)
    build_parser.add_argument("--bucket-count", type=int)
    build_parser.add_argument("--chunk-points", type=int, default=32768)
    build_parser.add_argument("--sort-run-points", type=int, default=262144)
    build_parser.add_argument("--merge-fan-in", type=int, default=64)
    build_parser.add_argument("--temp-dir", type=Path)
    build_parser.add_argument("--manifest", type=Path)
    build_parser.add_argument("--keep-temp", action="store_true")
    build_parser.add_argument("--force", action="store_true")
    build_parser.add_argument("--dry-run", action="store_true")
    build_parser.add_argument("--no-file-sha256", action="store_true")

    verify_parser = subparsers.add_parser("verify", help="verify structure, CRC, IDs, and bounds")
    verify_parser.add_argument("container", type=Path)
    verify_parser.add_argument("--manifest", type=Path)
    verify_parser.add_argument("--skip-sha256", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _make_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "estimate":
            result = estimate(args.source, args.page_points, args.bucket_count)
        elif args.command == "build":
            if args.dry_run:
                result = estimate(args.source, args.page_points, args.bucket_count)
            else:
                result = build_container(
                    args.source,
                    args.output,
                    page_points=args.page_points,
                    bucket_count=args.bucket_count,
                    chunk_points=args.chunk_points,
                    sort_run_points=args.sort_run_points,
                    merge_fan_in=args.merge_fan_in,
                    temp_dir=args.temp_dir,
                    keep_temp=args.keep_temp,
                    force=args.force,
                    compute_file_sha256=not args.no_file_sha256,
                    manifest_path=args.manifest,
                )
        else:
            result = verify_container(
                args.container,
                manifest_path=args.manifest,
                verify_sha256=not args.skip_sha256,
            )
        json.dump(result, sys.stdout, ensure_ascii=False, indent=2)
        sys.stdout.write("\n")
        return 0
    except (FormatError, VerificationError, FileExistsError, ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
