#!/usr/bin/env python3
"""Strict reader for the Spark RAD0/RADC LoD tree container subset."""

from __future__ import annotations

import argparse
import array
import dataclasses
import json
import math
import struct
import sys
import zlib
from pathlib import Path
from typing import Sequence


RAD_MAGIC = b"RAD0"
RADC_MAGIC = b"RADC"
PREFIX = struct.Struct("<4sI")
U64 = struct.Struct("<Q")
SOURCE_ID_HEADER = struct.Struct("<8sIIQQQ")
SOURCE_ID_MAGIC = b"NGSSID1\0"
INTERNAL_SOURCE_ID = 0xFFFFFFFFFFFFFFFF


class RadError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class RadChunk:
    path: Path
    meta: dict[str, object]
    payload: bytes

    @property
    def base(self) -> int:
        return require_int(self.meta, "base")

    @property
    def count(self) -> int:
        return require_int(self.meta, "count")


@dataclasses.dataclass(frozen=True)
class DecodedSplats:
    center: array.array
    opacity: array.array
    rgb: array.array
    scale: array.array
    quaternion: array.array
    sh1: array.array
    sh2: array.array
    sh3: array.array
    child_count: array.array
    child_start: array.array


def align8(value: int) -> int:
    return (value + 7) & ~7


def require_int(mapping: dict[str, object], name: str) -> int:
    value = mapping.get(name)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise RadError(f"{name} must be a non-negative integer")
    return value


def read_json_prefix(path: Path, expected_magic: bytes) -> tuple[dict[str, object], bytes, int]:
    data = path.read_bytes()
    if len(data) < PREFIX.size:
        raise RadError(f"file is too small: {path}")
    magic, json_bytes = PREFIX.unpack_from(data)
    if magic != expected_magic:
        raise RadError(f"invalid magic in {path}: {magic!r}")
    json_end = PREFIX.size + json_bytes
    padded_end = PREFIX.size + align8(json_bytes)
    if json_end > len(data) or padded_end > len(data):
        raise RadError(f"truncated JSON metadata in {path}")
    try:
        meta = json.loads(data[PREFIX.size:json_end])
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RadError(f"invalid JSON metadata in {path}: {exc}") from exc
    if not isinstance(meta, dict):
        raise RadError(f"metadata root must be an object: {path}")
    if any(data[json_end:padded_end]):
        raise RadError(f"nonzero JSON alignment padding in {path}")
    return meta, data, padded_end


def read_rad(path: Path) -> dict[str, object]:
    meta, data, payload_offset = read_json_prefix(path, RAD_MAGIC)
    if payload_offset != len(data):
        raise RadError("chunked RAD header must not contain inline payload")
    if require_int(meta, "version") != 1:
        raise RadError("unsupported RAD version")
    if meta.get("type") != "gsplat" or meta.get("lodTree") is not True:
        raise RadError("RAD must be a gsplat LoD tree")
    if require_int(meta, "count") <= 0:
        raise RadError("RAD tree must contain at least one splat")
    chunks = meta.get("chunks")
    if not isinstance(chunks, list) or not chunks:
        raise RadError("RAD header has no chunks")
    return meta


def read_radc(path: Path) -> RadChunk:
    meta, data, payload_size_offset = read_json_prefix(path, RADC_MAGIC)
    if require_int(meta, "version") != 1:
        raise RadError("unsupported RADC version")
    if payload_size_offset + U64.size > len(data):
        raise RadError(f"missing RADC payload size: {path}")
    payload_bytes = U64.unpack_from(data, payload_size_offset)[0]
    payload_offset = payload_size_offset + U64.size
    if payload_bytes != require_int(meta, "payloadBytes"):
        raise RadError(f"RADC payload size field mismatch: {path}")
    if payload_offset + payload_bytes != len(data):
        raise RadError(f"RADC physical payload size mismatch: {path}")
    properties = meta.get("properties")
    if not isinstance(properties, list) or not properties:
        raise RadError(f"RADC has no properties: {path}")
    names: set[str] = set()
    for prop in properties:
        if not isinstance(prop, dict) or not isinstance(prop.get("property"), str):
            raise RadError(f"invalid RADC property record: {path}")
        name = prop["property"]
        if name in names:
            raise RadError(f"duplicate RADC property {name}: {path}")
        names.add(name)
        offset = require_int(prop, "offset")
        size = require_int(prop, "bytes")
        if offset + size > payload_bytes:
            raise RadError(f"RADC property {name} exceeds payload: {path}")
    return RadChunk(path=path, meta=meta, payload=data[payload_offset:])


def property_bytes(chunk: RadChunk, name: str, expected_bytes: int | None = None) -> bytes:
    properties = chunk.meta["properties"]
    prop = next((item for item in properties if item.get("property") == name), None)
    if prop is None:
        raise RadError(f"missing RADC property {name}: {chunk.path}")
    offset = require_int(prop, "offset")
    size = require_int(prop, "bytes")
    raw = chunk.payload[offset:offset + size]
    compression = prop.get("compression")
    if compression is None:
        decoded = raw
    elif compression == "gz":
        try:
            decoded = zlib.decompress(raw, wbits=-15)
        except zlib.error as exc:
            raise RadError(f"raw DEFLATE failed for {name} in {chunk.path}: {exc}") from exc
    else:
        raise RadError(f"unsupported compression {compression!r} for {name}")
    if expected_bytes is not None and len(decoded) != expected_bytes:
        raise RadError(
            f"decoded {name} size mismatch in {chunk.path}: "
            f"expected {expected_bytes}, got {len(decoded)}"
        )
    return decoded


def property_record(chunk: RadChunk, name: str) -> dict[str, object]:
    prop = next((item for item in chunk.meta["properties"] if item.get("property") == name), None)
    if prop is None:
        raise RadError(f"missing RADC property {name}: {chunk.path}")
    return prop


def decode_planar_floats(raw: bytes, encoding: str, dims: int, count: int,
                         minimum: float | None = None, maximum: float | None = None) -> array.array:
    result = array.array("f")
    if encoding in ("f32", "f16"):
        width, code = (4, "f") if encoding == "f32" else (2, "e")
        if len(raw) != dims * count * width:
            raise RadError(f"{encoding} property size mismatch")
        for index in range(count):
            for dim in range(dims):
                result.append(struct.unpack_from("<" + code, raw, (dim * count + index) * width)[0])
        return result
    if encoding in ("f32_lebytes", "f16_lebytes"):
        width, code = (4, "f") if encoding == "f32_lebytes" else (2, "e")
        stride = dims * count
        if len(raw) != stride * width:
            raise RadError(f"{encoding} property size mismatch")
        for index in range(count):
            for dim in range(dims):
                base = dim * count + index
                encoded = bytes(raw[base + byte * stride] for byte in range(width))
                result.append(struct.unpack("<" + code, encoded)[0])
        return result
    if encoding in ("r8", "r8_delta"):
        if minimum is None or maximum is None or len(raw) != dims * count:
            raise RadError(f"{encoding} property metadata/size mismatch")
        previous = [0] * dims
        for index in range(count):
            for dim in range(dims):
                value = raw[dim * count + index]
                if encoding == "r8_delta":
                    value = (previous[dim] + value) & 0xFF
                    previous[dim] = value
                result.append(minimum + value / 255.0 * (maximum - minimum))
        return result
    if encoding in ("s8", "s8_delta"):
        if maximum is None or len(raw) != dims * count:
            raise RadError(f"{encoding} property metadata/size mismatch")
        previous = [0] * dims
        for index in range(count):
            for dim in range(dims):
                value = raw[dim * count + index]
                if encoding == "s8_delta":
                    value = (previous[dim] + value) & 0xFF
                    previous[dim] = value
                signed = value if value < 128 else value - 256
                result.append(signed / 127.0 * maximum)
        return result
    if encoding == "ln_0r8":
        if minimum is None or maximum is None or len(raw) != dims * count:
            raise RadError("ln_0r8 property metadata/size mismatch")
        step = (maximum - minimum) / 254.0
        for index in range(count):
            for dim in range(dims):
                value = raw[dim * count + index]
                result.append(0.0 if value == 0 else math.exp(minimum + (value - 1) * step))
        return result
    if encoding == "ln_f16":
        logs = decode_planar_floats(raw, "f16", dims, count)
        return array.array("f", (math.exp(value) for value in logs))
    raise RadError(f"unsupported floating property encoding: {encoding}")


def decode_float_property(chunk: RadChunk, name: str, dims: int) -> array.array:
    prop = property_record(chunk, name)
    encoding = prop.get("encoding")
    if not isinstance(encoding, str):
        raise RadError(f"property {name} has no encoding")
    minimum = prop.get("min")
    maximum = prop.get("max")
    if minimum is not None and not isinstance(minimum, (int, float)):
        raise RadError(f"property {name} min is invalid")
    if maximum is not None and not isinstance(maximum, (int, float)):
        raise RadError(f"property {name} max is invalid")
    return decode_planar_floats(
        property_bytes(chunk, name), encoding, dims, chunk.count,
        None if minimum is None else float(minimum),
        None if maximum is None else float(maximum),
    )


def decode_quaternion(chunk: RadChunk) -> array.array:
    prop = property_record(chunk, "orientation")
    encoding = prop.get("encoding")
    if encoding in ("f32", "f16"):
        xyz = decode_float_property(chunk, "orientation", 3)
        result = array.array("f")
        for index in range(chunk.count):
            x, y, z = xyz[index * 3:index * 3 + 3]
            result.extend((x, y, z, math.sqrt(max(0.0, 1.0 - x*x - y*y - z*z))))
        return result
    if encoding != "oct88r8":
        raise RadError(f"unsupported orientation encoding: {encoding}")
    raw = property_bytes(chunk, "orientation", chunk.count * 3)
    result = array.array("f")
    for index in range(chunk.count):
        u, v, radius = raw[index * 3:index * 3 + 3]
        x, y = u / 255.0 * 2.0 - 1.0, v / 255.0 * 2.0 - 1.0
        z = 1.0 - abs(x) - abs(y)
        correction = max(-z, 0.0)
        x = x - correction if x >= 0.0 else x + correction
        y = y - correction if y >= 0.0 else y + correction
        length = math.sqrt(x*x + y*y + z*z)
        if length <= 0.0:
            raise RadError("invalid zero-length octahedral orientation")
        x, y, z = x / length, y / length, z / length
        half_theta = radius / 255.0 * 0.5 * math.pi
        sine, cosine = math.sin(half_theta), math.cos(half_theta)
        result.extend((x * sine, y * sine, z * sine, cosine))
    return result


def decode_optional_sh(chunk: RadChunk, name: str, dims: int) -> array.array:
    if not any(item.get("property") == name for item in chunk.meta["properties"]):
        return array.array("f", [0.0]) * (chunk.count * dims)
    return decode_float_property(chunk, name, dims)


def decode_splats(chunk: RadChunk) -> DecodedSplats:
    decoded = DecodedSplats(
        center=decode_float_property(chunk, "center", 3),
        opacity=decode_float_property(chunk, "alpha", 1),
        rgb=decode_float_property(chunk, "rgb", 3),
        scale=decode_float_property(chunk, "scales", 3),
        quaternion=decode_quaternion(chunk),
        sh1=decode_optional_sh(chunk, "sh1", 9),
        sh2=decode_optional_sh(chunk, "sh2", 15),
        sh3=decode_optional_sh(chunk, "sh3", 21),
        child_count=decode_children(chunk)[0],
        child_start=decode_children(chunk)[1],
    )
    arrays = (
        ("center", decoded.center, 3), ("opacity", decoded.opacity, 1),
        ("rgb", decoded.rgb, 3), ("scale", decoded.scale, 3),
        ("quaternion", decoded.quaternion, 4), ("sh1", decoded.sh1, 9),
        ("sh2", decoded.sh2, 15), ("sh3", decoded.sh3, 21),
    )
    for name, values, dims in arrays:
        if len(values) != chunk.count * dims or not all(math.isfinite(value) for value in values):
            raise RadError(f"decoded {name} values are invalid")
    return decoded


def decode_unsigned(raw: bytes, typecode: str, width: int) -> array.array:
    values = array.array(typecode)
    values.frombytes(raw)
    if values.itemsize != width:
        raise RadError(f"host array width mismatch for {typecode}")
    if sys.byteorder != "little":
        values.byteswap()
    return values


def decode_children(chunk: RadChunk) -> tuple[array.array, array.array]:
    count = chunk.count
    counts = decode_unsigned(property_bytes(chunk, "child_count", count * 2), "H", 2)
    starts = decode_unsigned(property_bytes(chunk, "child_start", count * 4), "I", 4)
    return counts, starts


def read_source_ids(cache_dir: Path, expected_nodes: int) -> tuple[array.array, int, int]:
    paths = list(cache_dir.glob("*.source_ids.bin"))
    if len(paths) != 1:
        raise RadError(f"expected exactly one source ID sidecar in {cache_dir}")
    data = paths[0].read_bytes()
    if len(data) < SOURCE_ID_HEADER.size:
        raise RadError("source ID sidecar is truncated")
    magic, version, reserved, nodes, leaves, input_splats = SOURCE_ID_HEADER.unpack_from(data)
    if magic != SOURCE_ID_MAGIC or version != 1 or reserved != 0:
        raise RadError("invalid source ID sidecar header")
    if nodes != expected_nodes:
        raise RadError(f"source ID node count is {nodes}, expected {expected_nodes}")
    expected_bytes = SOURCE_ID_HEADER.size + nodes * 8
    if len(data) != expected_bytes:
        raise RadError("source ID sidecar physical size mismatch")
    ids = decode_unsigned(data[SOURCE_ID_HEADER.size:], "Q", 8)
    return ids, leaves, input_splats


def validate_tree(cache_dir: Path, verify_file_sizes: bool = True) -> dict[str, object]:
    manifests = list(cache_dir.glob("*.rad"))
    if len(manifests) != 1:
        raise RadError(f"expected exactly one RAD header in {cache_dir}")
    rad_path = manifests[0]
    rad = read_rad(rad_path)
    total_count = require_int(rad, "count")
    chunk_records = rad["chunks"]
    expected_base = 0
    parent_by_node = array.array("q", [-1]) * total_count
    child_counts = array.array("H", [0]) * total_count
    internal_nodes = 0
    leaf_nodes = 0
    max_children = 0
    radc_files: list[str] = []

    for record in chunk_records:
        if not isinstance(record, dict):
            raise RadError("invalid RAD chunk record")
        filename = record.get("filename")
        if not isinstance(filename, str) or Path(filename).name != filename:
            raise RadError("chunk filename must be a plain basename")
        if filename in radc_files:
            raise RadError(f"duplicate RAD chunk filename: {filename}")
        radc_files.append(filename)
        chunk_path = cache_dir / filename
        if not chunk_path.is_file():
            raise RadError(f"missing RAD chunk: {chunk_path}")
        if verify_file_sizes and chunk_path.stat().st_size != require_int(record, "bytes"):
            raise RadError(f"RAD chunk byte count mismatch: {chunk_path}")
        chunk = read_radc(chunk_path)
        if chunk.base != expected_base:
            raise RadError(f"non-contiguous chunk base in {chunk_path}")
        if chunk.meta.get("lodTree") is not True:
            raise RadError(f"RADC is not marked as an LoD tree: {chunk_path}")
        counts, starts = decode_children(chunk)
        child_counts[chunk.base:chunk.base + chunk.count] = counts
        for local_index, child_count in enumerate(counts):
            node_index = chunk.base + local_index
            child_start = starts[local_index]
            if child_count == 0:
                leaf_nodes += 1
                continue
            internal_nodes += 1
            max_children = max(max_children, child_count)
            if child_start <= node_index or child_start + child_count > total_count:
                raise RadError(f"invalid child range at node {node_index}")
            for child in range(child_start, child_start + child_count):
                if parent_by_node[child] != -1:
                    raise RadError(f"node {child} has multiple parents")
                parent_by_node[child] = node_index
        expected_base += chunk.count

    if expected_base != total_count:
        raise RadError(f"chunk counts cover {expected_base}, expected {total_count}")
    if parent_by_node[0] != -1:
        raise RadError("root node 0 must not have a parent")
    missing_parent = next((index for index in range(1, total_count) if parent_by_node[index] == -1), None)
    if missing_parent is not None:
        raise RadError(f"non-root node {missing_parent} has no parent")
    source_ids, declared_leaves, input_splats = read_source_ids(cache_dir, total_count)
    seen_source_ids: set[int] = set()
    actual_leaves = 0
    for node_index, (source_id, child_count) in enumerate(zip(source_ids, child_counts)):
        if child_count:
            if source_id != INTERNAL_SOURCE_ID:
                raise RadError(f"internal node {node_index} has a source ID")
            continue
        actual_leaves += 1
        if source_id == INTERNAL_SOURCE_ID:
            raise RadError(f"leaf node {node_index} has no source ID")
        if source_id >= input_splats:
            raise RadError(f"leaf node {node_index} source ID is outside input range")
        if source_id in seen_source_ids:
            raise RadError(f"duplicate source ID {source_id}")
        seen_source_ids.add(source_id)
    if declared_leaves != actual_leaves:
        raise RadError(f"source ID leaf count is {declared_leaves}, expected {actual_leaves}")
    return {
        "schema": "nanogs.spark_rad_tree_summary",
        "version": 1,
        "rad": rad_path.name,
        "chunks": len(chunk_records),
        "nodes": total_count,
        "internal_nodes": internal_nodes,
        "leaf_nodes": leaf_nodes,
        "max_children": max_children,
        "root": 0,
        "source_input_splats": input_splats,
        "source_leaf_ids": len(seen_source_ids),
        "source_ids_valid": True,
        "tree_valid": True,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate a Spark RAD/RADC LoD tree")
    parser.add_argument("cache_dir", type=Path)
    args = parser.parse_args(argv)
    try:
        result = validate_tree(args.cache_dir.resolve())
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0
    except (RadError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
