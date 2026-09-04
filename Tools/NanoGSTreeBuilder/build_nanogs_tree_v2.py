#!/usr/bin/env python3
"""Convert one stable-ID Spark RAD tree into NanoGS Tree v2 + Page v1."""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import mmap
import os
import shutil
import struct
import sys
import tempfile
import time
import zlib
from pathlib import Path
from typing import Iterator, Sequence

import nanogs_spark_cache as spark_cache
import spark_rad_reader as rad

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools" / "NanoGSPageBuilder"))
import nanogs_page_builder as ng  # noqa: E402


TREE_MAGIC = b"NGSTREE2"
TREE_VERSION = 2
TREE_ENDIAN = 0x01020304
TREE_HEADER_BYTES = 128
TREE_NODE_BYTES = 64
TREE_HEADER = struct.Struct("<8s6I7Q2I32x")
TREE_NODE = struct.Struct("<IHHQII5fIII8x")
TREE_FLAG_EXACT_LEAVES = 1 << 0
TREE_FLAG_INFLATED_PARENTS = 1 << 1
NODE_FLAG_INTERNAL = 1 << 0
NODE_FLAG_EXACT_LEAF = 1 << 1
CONVERTER_VERSION = "1.4.0"
PAGE_RECORD_LAYOUT = "exact_leaves_then_internal"
SH_C0 = ng.SH_C0

assert TREE_HEADER.size == TREE_HEADER_BYTES
assert TREE_NODE.size == TREE_NODE_BYTES


class TreeBuildError(RuntimeError):
    pass


def canonical_json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def converter_key(spark_manifest: dict[str, object], page_points: int) -> str:
    description = {
        "schema": "nanogs.tree_v2",
        "version": TREE_VERSION,
        "converter_version": CONVERTER_VERSION,
        "spark_cache_key": spark_manifest["cache_key"],
        "page_points": page_points,
        "page_format": ng.VERSION,
        "exact_leaf_encoding": ng.PAYLOAD_FORMAT,
    }
    return hashlib.sha256(canonical_json(description)).hexdigest()


def load_tree_arrays(cache_dir: Path) -> tuple[dict[str, object], array.array, array.array, array.array]:
    rad_path = next(iter(cache_dir.glob("*.rad")), None)
    if rad_path is None:
        raise TreeBuildError("Spark cache has no RAD header")
    header = rad.read_rad(rad_path)
    count = rad.require_int(header, "count")
    source_ids, _, _ = rad.read_source_ids(cache_dir, count)
    child_counts = array.array("H")
    child_starts = array.array("I")
    for record in header["chunks"]:
        chunk = rad.read_radc(cache_dir / record["filename"])
        counts, starts = rad.decode_children(chunk)
        child_counts.extend(counts)
        child_starts.extend(starts)
    if len(child_counts) != count or len(child_starts) != count:
        raise TreeBuildError("Spark chunk arrays do not cover the tree")
    return header, source_ids, child_counts, child_starts


def compute_tree_metadata(child_counts: array.array, child_starts: array.array) -> tuple[array.array, array.array]:
    count = len(child_counts)
    depths = array.array("H", [0]) * count
    assigned = bytearray(count)
    assigned[0] = 1
    for parent, child_count in enumerate(child_counts):
        if not assigned[parent]:
            raise TreeBuildError(f"node {parent} has no assigned depth")
        child_depth = depths[parent] + 1
        if child_depth > 0xFFFF:
            raise TreeBuildError("tree depth exceeds uint16")
        start = child_starts[parent]
        for child in range(start, start + child_count):
            if child <= parent or child >= count or assigned[child]:
                raise TreeBuildError(f"invalid or duplicate child {child} at node {parent}")
            depths[child] = child_depth
            assigned[child] = 1
    subtree_leaves = array.array("I", [0]) * count
    for node in range(count - 1, -1, -1):
        child_count = child_counts[node]
        if child_count == 0:
            subtree_leaves[node] = 1
            continue
        start = child_starts[node]
        total = sum(subtree_leaves[start:start + child_count])
        if total <= 0 or total > 0xFFFFFFFF:
            raise TreeBuildError(f"invalid subtree leaf count at node {node}")
        subtree_leaves[node] = total
    return depths, subtree_leaves


def expand_subtree_support_radii(child_counts: array.array, child_starts: array.array,
                                 centers: array.array, support_radii: array.array) -> None:
    for parent in range(len(child_counts) - 1, -1, -1):
        child_count = child_counts[parent]
        if child_count == 0:
            continue
        parent_offset = parent * 3
        parent_x = centers[parent_offset]
        parent_y = centers[parent_offset + 1]
        parent_z = centers[parent_offset + 2]
        support = support_radii[parent]
        start = child_starts[parent]
        for child in range(start, start + child_count):
            child_offset = child * 3
            dx = centers[child_offset] - parent_x
            dy = centers[child_offset + 1] - parent_y
            dz = centers[child_offset + 2] - parent_z
            support = max(support, math.sqrt(dx * dx + dy * dy + dz * dz) + support_radii[child])
        support_radii[parent] = support


def partitioned_page_indices(child_counts: Sequence[int], leaf_count: int) -> Iterator[int]:
    leaf_output = 0
    internal_output = leaf_count
    for child_count in child_counts:
        if child_count:
            yield internal_output
            internal_output += 1
        else:
            yield leaf_output
            leaf_output += 1


def record_geometry(record: bytes, is_parent: bool) -> tuple[tuple[float, float, float], float, float]:
    body = memoryview(record)[ng.SPOOL_PREFIX.size:]
    position = struct.unpack_from("<3f", body, 0)
    scale = struct.unpack_from("<3f", body, 28)
    encoded_opacity = body[43] * ((2.0 if is_parent else 1.0) / 255.0)
    lod_opacity = min(encoded_opacity * 4.0 - 3.0, 5.0) if encoded_opacity > 1.0 else 1.0
    expansion = 1.0 + 0.7 * (lod_opacity - 1.0)
    return (
        position,
        2.0 * expansion * (sum(scale) / 3.0),
        2.0 * expansion * math.sqrt(sum(value * value for value in scale)),
    )


def exact_leaf_record(source_map: mmap.mmap, ply: ng.PlyHeader, source_id: int, node_index: int) -> bytes:
    offset = ply.data_offset + source_id * ply.vertex_stride
    values = ply.vertex_struct.unpack_from(source_map, offset)
    _, record = ng._decode_spool_record(values, ply.property_indices, source_id, (0, 0, 0), (1, 1, 1))
    body = bytearray(record[ng.SPOOL_PREFIX.size:])
    return ng.SPOOL_PREFIX.pack(0, node_index) + body


def parent_record(decoded: rad.DecodedSplats, local: int, node_index: int) -> bytes:
    position_m = decoded.center[local * 3:local * 3 + 3]
    rotation = decoded.quaternion[local * 4:local * 4 + 4]
    scale_m = decoded.scale[local * 3:local * 3 + 3]
    opacity = float(decoded.opacity[local])
    rgb = decoded.rgb[local * 3:local * 3 + 3]
    lod_opacity_quantization_limit = 2.0 + 2.0 / 255.0 + 1.0e-6
    if opacity < 0.0 or opacity > lod_opacity_quantization_limit:
        raise TreeBuildError(
            f"parent node {node_index} opacity {opacity} is outside Spark LoD range"
        )
    sh_dc = [(float(channel) - 0.5) / SH_C0 for channel in rgb]
    sh = [*sh_dc]
    sh.extend(decoded.sh1[local * 9:local * 9 + 9])
    sh.extend(decoded.sh2[local * 15:local * 15 + 15])
    sh.extend(decoded.sh3[local * 21:local * 21 + 21])
    if len(sh) != 48:
        raise TreeBuildError("parent SH3 layout is incomplete")
    position_cm = [float(value) * 100.0 for value in position_m]
    scale_cm = [float(value) * 100.0 for value in scale_m]
    rgba = ng._rgba_lane(sh_dc, min(1.0, 0.5 * opacity))
    body = ng.SPOOL_GEOMETRY_COLOR.pack(*position_cm, *rotation, *scale_cm, *rgba)
    try:
        body += ng.SH_HALF_STRUCT.pack(*sh)
    except OverflowError:
        body += b"".join(ng._half_bits(value) for value in sh)
    return ng.SPOOL_PREFIX.pack(0, node_index) + body


def page_container_header(records_total: int, page_points: int) -> dict[str, int]:
    return ng._layout_estimate(records_total, page_points)


def write_ordered_page_container(output: Path, records: Iterator[bytes], total: int,
                                 page_points: int) -> dict[str, object]:
    layout = page_container_header(total, page_points)
    partial = Path(str(output) + ".partial")
    partial.unlink(missing_ok=True)
    pages: list[ng.PageRecord] = []
    streams: list[ng.StreamRecord] = []
    manifest_pages: list[dict[str, object]] = []
    support_min = [math.inf] * 3
    support_max = [-math.inf] * 3
    buffer: list[bytes] = []
    written = 0
    try:
        with partial.open("w+b") as target:
            target.truncate(layout["file_bytes"])
            target.seek(layout["payload_offset"])
            for record in records:
                buffer.append(record)
                if len(buffer) < page_points:
                    continue
                page, manifest_page = ng._write_page(target, buffer, len(pages), written, streams)
                pages.append(page)
                manifest_pages.append(manifest_page)
                written += len(buffer)
                for axis in range(3):
                    support_min[axis] = min(support_min[axis], page.support_min[axis])
                    support_max[axis] = max(support_max[axis], page.support_max[axis])
                buffer.clear()
            if buffer:
                page, manifest_page = ng._write_page(target, buffer, len(pages), written, streams)
                pages.append(page)
                manifest_pages.append(manifest_page)
                written += len(buffer)
                for axis in range(3):
                    support_min[axis] = min(support_min[axis], page.support_min[axis])
                    support_max[axis] = max(support_max[axis], page.support_max[axis])
            if written != total or len(pages) != layout["page_count"]:
                raise TreeBuildError(f"ordered Page v1 wrote {written}/{total} records")
            page_directory = b"".join(page.pack() for page in pages)
            stream_directory = b"".join(stream.pack() for stream in streams)
            directory_crc = zlib.crc32(page_directory)
            directory_crc = zlib.crc32(stream_directory, directory_crc) & 0xFFFFFFFF
            header = ng._pack_header(
                file_bytes=layout["file_bytes"], page_count=len(pages),
                page_directory_offset=layout["page_directory_offset"], stream_count=len(streams),
                stream_directory_offset=layout["stream_directory_offset"],
                payload_offset=layout["payload_offset"], payload_bytes=layout["payload_bytes"],
                total_splat_count=written, scene_bounds_min=support_min,
                scene_bounds_max=support_max, directory_crc32=directory_crc,
            )
            target.seek(0); target.write(header)
            target.seek(layout["page_directory_offset"]); target.write(page_directory)
            target.seek(layout["stream_directory_offset"]); target.write(stream_directory)
            target.flush(); os.fsync(target.fileno())
        os.replace(partial, output)
        return {
            "file": output.name, "bytes": output.stat().st_size,
            "sha256": spark_cache.sha256_file(output), "pages": len(pages),
            "splats": written, "page_points": page_points,
            "support_bounds_cm": {"min": support_min, "max": support_max},
        }
    except Exception:
        partial.unlink(missing_ok=True)
        raise


def write_tree_file(path: Path, node_records_path: Path, node_count: int, page_points: int,
                    internal_count: int, leaf_count: int, source_input: int) -> dict[str, object]:
    nodes = node_records_path.read_bytes()
    if len(nodes) != node_count * TREE_NODE_BYTES:
        raise TreeBuildError("Tree v2 node record byte count mismatch")
    nodes_crc = zlib.crc32(nodes) & 0xFFFFFFFF
    file_bytes = TREE_HEADER_BYTES + len(nodes)
    header = bytearray(TREE_HEADER_BYTES)
    TREE_HEADER.pack_into(
        header, 0, TREE_MAGIC, TREE_VERSION, TREE_ENDIAN, TREE_HEADER_BYTES,
        TREE_NODE_BYTES, TREE_FLAG_EXACT_LEAVES,
        page_points, node_count, 0, internal_count, leaf_count, source_input,
        TREE_HEADER_BYTES, file_bytes, nodes_crc, 0,
    )
    header_crc = zlib.crc32(header) & 0xFFFFFFFF
    struct.pack_into("<I", header, 92, header_crc)
    partial = Path(str(path) + ".partial")
    with partial.open("wb") as target:
        target.write(header); target.write(nodes); target.flush(); os.fsync(target.fileno())
    os.replace(partial, path)
    return {"file": path.name, "bytes": path.stat().st_size, "sha256": spark_cache.sha256_file(path)}


def build_tree(source_ply: Path, spark_cache_dir: Path, output_root: Path,
               page_points: int = 65536, force: bool = False) -> dict[str, object]:
    started = time.monotonic()
    source_ply = source_ply.resolve()
    spark_cache_dir = spark_cache_dir.resolve()
    spark_manifest = spark_cache.validate_manifest(spark_cache_dir, verify_hashes=True)
    options = spark_manifest.get("options", {})
    if options.get("source_id_sidecar") is not True or options.get("inflate") is not False:
        raise TreeBuildError("Spark cache must enable source_id_sidecar and preserve LoD opacity")
    source_hash = spark_cache.sha256_file(source_ply)
    if source_hash != spark_manifest.get("source_sha256"):
        raise TreeBuildError("source PLY SHA256 does not match Spark cache")
    key = converter_key(spark_manifest, page_points)
    output_root = output_root.resolve()
    preferred_output_dir = output_root / spark_cache.safe_stem(source_ply) / key
    output_dir = preferred_output_dir
    if not force:
        candidates = [preferred_output_dir]
        if output_root.is_dir():
            candidates.extend(sorted(
                candidate for candidate in output_root.glob(f"*/{key}")
                if candidate != preferred_output_dir
            ))
        for candidate in candidates:
            if not candidate.exists():
                continue
            manifest_path = candidate / "manifest.json"
            if not manifest_path.is_file():
                if candidate == preferred_output_dir:
                    raise TreeBuildError(f"invalid existing Tree v2 cache: {candidate}")
                continue
            existing = json.loads(manifest_path.read_text(encoding="utf-8"))
            if existing.get("tree_key") == key:
                return {"cache_hit": True, "output_dir": str(candidate), "manifest": existing}
            if candidate == preferred_output_dir:
                raise TreeBuildError(f"invalid existing Tree v2 cache: {candidate}")
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    rad_summary = rad.validate_tree(spark_cache_dir)
    header, source_ids, child_counts, child_starts = load_tree_arrays(spark_cache_dir)
    node_count = len(source_ids)
    depths, subtree_leaves = compute_tree_metadata(child_counts, child_starts)
    ply = ng.parse_ply_header(source_ply)
    if ply.vertex_count != rad_summary["source_input_splats"]:
        raise TreeBuildError("source PLY vertex count does not match stable-ID sidecar")

    with tempfile.TemporaryDirectory(prefix=f".{key}.stage-", dir=output_dir.parent) as stage_name:
        stage = Path(stage_name)
        node_records_path = stage / "nodes.records"
        page_path = stage / "tree_nodes.ngsp"
        tree_path = stage / "tree.ngst"
        internal_count = sum(1 for count in child_counts if count)
        leaf_count = node_count - internal_count

        centers = array.array("f", [0.0]) * (node_count * 3)
        feature_radii = array.array("f", [0.0]) * node_count
        support_radii = array.array("f", [0.0]) * node_count

        def records() -> Iterator[bytes]:
            source_handle = source_ply.open("rb")
            source_map = mmap.mmap(source_handle.fileno(), 0, access=mmap.ACCESS_READ)
            try:
                for node_index, child_count in enumerate(child_counts):
                    if child_count:
                        continue
                    record = exact_leaf_record(source_map, ply, source_ids[node_index], node_index)
                    position, feature_radius, support_radius = record_geometry(record, False)
                    centers[node_index * 3:node_index * 3 + 3] = array.array("f", position)
                    feature_radii[node_index] = feature_radius
                    support_radii[node_index] = support_radius
                    yield record

                node_index = 0
                for chunk_record in header["chunks"]:
                    chunk = rad.read_radc(spark_cache_dir / chunk_record["filename"])
                    decoded = rad.decode_splats(chunk)
                    for local in range(chunk.count):
                        if child_counts[node_index]:
                            record = parent_record(decoded, local, node_index)
                            position, feature_radius, support_radius = record_geometry(record, True)
                            centers[node_index * 3:node_index * 3 + 3] = array.array("f", position)
                            feature_radii[node_index] = feature_radius
                            support_radii[node_index] = support_radius
                            yield record
                        node_index += 1
                if node_index != node_count:
                    raise TreeBuildError(f"decoded {node_index}/{node_count} tree nodes")
            finally:
                source_map.close(); source_handle.close()

        page_info = write_ordered_page_container(page_path, records(), node_count, page_points)
        expand_subtree_support_radii(child_counts, child_starts, centers, support_radii)
        with node_records_path.open("wb") as node_records:
            for node_index, page_output_index in enumerate(
                    partitioned_page_indices(child_counts, leaf_count)):
                is_internal = child_counts[node_index] != 0
                node_records.write(TREE_NODE.pack(
                    child_starts[node_index], child_counts[node_index], depths[node_index],
                    source_ids[node_index], page_output_index // page_points,
                    page_output_index % page_points,
                    *centers[node_index * 3:node_index * 3 + 3],
                    feature_radii[node_index], support_radii[node_index],
                    NODE_FLAG_INTERNAL if is_internal else NODE_FLAG_EXACT_LEAF,
                    subtree_leaves[node_index], 0,
                ))
        tree_info = write_tree_file(
            tree_path, node_records_path, node_count, page_points, internal_count,
            leaf_count, ply.vertex_count,
        )
        node_records_path.unlink()
        manifest = {
            "schema": "nanogs.tree_v2", "version": TREE_VERSION,
            "converter_version": CONVERTER_VERSION, "tree_key": key,
            "source": {"path": str(source_ply), "bytes": source_ply.stat().st_size,
                       "sha256": source_hash, "vertex_count": ply.vertex_count},
            "spark_cache": {"path": str(spark_cache_dir), "cache_key": spark_manifest["cache_key"],
                            "builder_sha256": spark_manifest["builder_sha256"]},
            "tree": {**tree_info, "nodes": node_count, "root": 0,
                     "internal_nodes": internal_count, "exact_leaves": leaf_count,
                     "max_depth": max(depths), "root_subtree_leaves": subtree_leaves[0]},
            "pages": {**page_info, "record_layout": PAGE_RECORD_LAYOUT},
            "invariants": {
                "exact_leaf_source_ids": True, "node_index_is_page_output_index": False,
                "exact_leaves_are_physically_contiguous": True,
				"spark_lod_opacity_encoded": True, "exact_leaf_alpha_unscaled": True,
				"legacy_paths_modified": False,
            },
            "build_seconds": time.monotonic() - started,
        }
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        if output_dir.exists():
            shutil.rmtree(output_dir)
        os.replace(stage, output_dir)
    return {"cache_hit": False, "output_dir": str(output_dir), "manifest": manifest}


def verify_tree(output_dir: Path) -> dict[str, object]:
    manifest = json.loads((output_dir / "manifest.json").read_text())
    tree_path = output_dir / manifest["tree"]["file"]
    data = tree_path.read_bytes()
    if len(data) < TREE_HEADER_BYTES:
        raise TreeBuildError("Tree v2 file is truncated")
    fields = TREE_HEADER.unpack_from(data)
    magic, version, endian, header_bytes, node_bytes = fields[:5]
    if (magic, version, endian, header_bytes, node_bytes) != (
        TREE_MAGIC, TREE_VERSION, TREE_ENDIAN, TREE_HEADER_BYTES, TREE_NODE_BYTES
    ):
        raise TreeBuildError("Tree v2 header is incompatible")
    node_count, root, internal_count, leaf_count, source_input, nodes_offset, file_bytes = fields[7:14]
    nodes_crc, stored_header_crc = fields[14:16]
    header = bytearray(data[:TREE_HEADER_BYTES]); struct.pack_into("<I", header, 92, 0)
    if zlib.crc32(header) & 0xFFFFFFFF != stored_header_crc:
        raise TreeBuildError("Tree v2 header CRC mismatch")
    nodes = data[nodes_offset:]
    if len(data) != file_bytes or len(nodes) != node_count * TREE_NODE_BYTES:
        raise TreeBuildError("Tree v2 physical size mismatch")
    if zlib.crc32(nodes) & 0xFFFFFFFF != nodes_crc:
        raise TreeBuildError("Tree v2 node CRC mismatch")
    actual_internal = 0
    actual_leaves = 0
    page_layout = manifest["pages"].get("record_layout", "node_index_order")
    expected_page_indices = None
    if page_layout == PAGE_RECORD_LAYOUT:
        child_counts_for_layout = array.array("H")
        for index in range(node_count):
            child_counts_for_layout.append(
                TREE_NODE.unpack_from(nodes, index * TREE_NODE_BYTES)[1]
            )
        expected_page_indices = partitioned_page_indices(child_counts_for_layout, leaf_count)
    elif page_layout != "node_index_order":
        raise TreeBuildError(f"unsupported Tree v2 page record layout: {page_layout}")
    for index in range(node_count):
        record = TREE_NODE.unpack_from(nodes, index * TREE_NODE_BYTES)
        child_start, child_count, _, source_id, page_id, page_local = record[:6]
        flags = record[11]
        expected_page_index = next(expected_page_indices) if expected_page_indices is not None else index
        if (page_id, page_local) != (
                expected_page_index // manifest["pages"]["page_points"],
                expected_page_index % manifest["pages"]["page_points"]):
            raise TreeBuildError(f"Tree v2 page mapping mismatch at node {index}")
        if child_count:
            actual_internal += 1
            if source_id != rad.INTERNAL_SOURCE_ID or not (flags & NODE_FLAG_INTERNAL):
                raise TreeBuildError(f"invalid internal node {index}")
            if child_start <= index or child_start + child_count > node_count:
                raise TreeBuildError(f"invalid children at node {index}")
        else:
            actual_leaves += 1
            if source_id >= source_input or not (flags & NODE_FLAG_EXACT_LEAF):
                raise TreeBuildError(f"invalid exact leaf {index}")
    if root != 0 or actual_internal != internal_count or actual_leaves != leaf_count:
        raise TreeBuildError("Tree v2 summary mismatch")
    page_verify = ng.verify_container(
        output_dir / manifest["pages"]["file"], verify_sha256=False
    )
    return {"tree_valid": True, "nodes": node_count, "internal_nodes": internal_count,
            "exact_leaves": leaf_count, "page_container": page_verify}


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build NanoGS Tree v2 from a stable-ID Spark cache")
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build")
    build.add_argument("source_ply", type=Path)
    build.add_argument("spark_cache", type=Path)
    build.add_argument("--output-root", type=Path, required=True)
    build.add_argument("--page-points", type=int, default=65536)
    build.add_argument("--force", action="store_true")
    verify = sub.add_parser("verify")
    verify.add_argument("output_dir", type=Path)
    args = parser.parse_args(argv)
    try:
        result = build_tree(args.source_ply, args.spark_cache, args.output_root,
                            args.page_points, args.force) if args.command == "build" else verify_tree(args.output_dir)
        json.dump(result, sys.stdout, indent=2, sort_keys=True); sys.stdout.write("\n")
        return 0
    except (TreeBuildError, rad.RadError, spark_cache.CacheError, ng.FormatError,
            ng.VerificationError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
