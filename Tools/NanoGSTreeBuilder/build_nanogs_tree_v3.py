#!/usr/bin/env python3
"""Build a lossless, block-streamable Tree v3 sidecar from NanoGS Tree v2."""
from __future__ import annotations
import argparse, hashlib, json, mmap, os, struct, sys, zlib
from collections import deque
from pathlib import Path
from typing import Iterable, Sequence
import build_nanogs_tree_v2 as v2

SCHEMA = "nanogs.tree_v3"
VERSION = 3
CONVERTER_VERSION = "0.1.0"
GPU_NODE = struct.Struct("<4f4I")
BLOCK_RECORD = struct.Struct("<8I")
GPU_NODE_FLAG_INTERNAL = 1 << 16
GPU_NODE_FLAG_EXACT_LEAF = 1 << 17
GPU_NODE_FLAG_BLOCK_PROXY = 1 << 18
INVALID_BLOCK = 0xFFFFFFFF

class TreeV3Error(RuntimeError):
    pass

def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def read_node(data: mmap.mmap, nodes_offset: int, index: int) -> tuple:
    return v2.TREE_NODE.unpack_from(data, nodes_offset + index * v2.TREE_NODE_BYTES)

def pack_node(record: tuple, first_child: int, child_count: int, extra_flags: int = 0) -> bytes:
    _, _, _, _, page_id, page_local, cx, cy, cz, feature, support, flags, _, _ = record
    if page_id > 0xFFFF or page_local > 0xFFFF:
        raise TreeV3Error("Tree v2 page address does not fit Tree v3")
    packed_flags = child_count & 0xFFFF
    if child_count:
        packed_flags |= GPU_NODE_FLAG_INTERNAL
    if flags & v2.NODE_FLAG_EXACT_LEAF:
        packed_flags |= GPU_NODE_FLAG_EXACT_LEAF
    packed_flags |= extra_flags
    feature_bits = struct.unpack("<I", struct.pack("<f", feature))[0]
    return GPU_NODE.pack(cx, cy, cz, support, first_child, packed_flags,
                         feature_bits, (page_id << 16) | page_local)

def read_header(data: mmap.mmap) -> dict[str, int]:
    if len(data) < v2.TREE_HEADER_BYTES:
        raise TreeV3Error("Tree v2 file is truncated")
    fields = v2.TREE_HEADER.unpack_from(data)
    if fields[:5] != (v2.TREE_MAGIC, v2.TREE_VERSION, v2.TREE_ENDIAN,
                      v2.TREE_HEADER_BYTES, v2.TREE_NODE_BYTES):
        raise TreeV3Error("Tree v2 header is incompatible")
    return {"page_points": fields[6], "node_count": fields[7], "root": fields[8],
            "internal_count": fields[9], "leaf_count": fields[10],
            "source_input": fields[11], "nodes_offset": fields[12],
            "file_bytes": fields[13], "nodes_crc": fields[14]}

def write_bytes_atomic(path: Path, chunks: Iterable[bytes]) -> tuple[int, int]:
    partial = Path(str(path) + ".partial")
    partial.unlink(missing_ok=True)
    crc = size = 0
    try:
        with partial.open("wb") as output:
            for chunk in chunks:
                output.write(chunk)
                crc = zlib.crc32(chunk, crc)
                size += len(chunk)
            output.flush(); os.fsync(output.fileno())
        os.replace(partial, path)
        return size, crc & 0xFFFFFFFF
    except Exception:
        partial.unlink(missing_ok=True)
        raise

def build_v3(v2_dir: Path, max_block_leaves: int, force: bool = False) -> dict[str, object]:
    if max_block_leaves <= 0:
        raise TreeV3Error("max_block_leaves must be positive")
    v2_dir = v2_dir.resolve()
    source_manifest = json.loads((v2_dir / "manifest.json").read_text(encoding="utf-8"))
    if source_manifest.get("schema") != "nanogs.tree_v2":
        raise TreeV3Error("input manifest is not NanoGS Tree v2")
    tree_path = v2_dir / source_manifest["tree"]["file"]
    source_sha = sha256_file(tree_path)
    key_data = json.dumps({"schema": SCHEMA, "version": VERSION,
                           "converter": CONVERTER_VERSION, "source": source_sha,
                           "max_block_leaves": max_block_leaves},
                          sort_keys=True, separators=(",", ":")).encode()
    tree_v3_key = hashlib.sha256(key_data).hexdigest()
    manifest_path = v2_dir / "tree_v3_manifest.json"
    if manifest_path.is_file() and not force:
        existing = json.loads(manifest_path.read_text(encoding="utf-8"))
        if existing.get("tree_v3_key") == tree_v3_key:
            return {"cache_hit": True, "output_dir": str(v2_dir), "manifest": existing}

    with tree_path.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
        header = read_header(data)
        node_count, nodes_offset = header["node_count"], header["nodes_offset"]
        if header["file_bytes"] != len(data):
            raise TreeV3Error("Tree v2 file size mismatch")
        def subtree_leaves(index: int) -> int:
            return read_node(data, nodes_offset, index)[12]

        skeleton_entries = [(header["root"], INVALID_BLOCK)]
        skeleton_children = [(0, 0)]
        queue = deque([0])
        block_roots: list[int] = []
        while queue:
            skeleton_index = queue.popleft()
            source_index, _ = skeleton_entries[skeleton_index]
            record = read_node(data, nodes_offset, source_index)
            child_start, child_count = record[0], record[1]
            if subtree_leaves(source_index) <= max_block_leaves:
                block_id = len(block_roots)
                block_roots.append(source_index)
                skeleton_entries[skeleton_index] = (source_index, block_id)
                continue
            first_output_child = len(skeleton_entries)
            for child in range(child_start, child_start + child_count):
                skeleton_entries.append((child, INVALID_BLOCK))
                queue.append(len(skeleton_entries) - 1)
            skeleton_children[skeleton_index] = (first_output_child, child_count)
            skeleton_children.extend([(0, 0)] * child_count)

        skeleton_path = v2_dir / "tree_v3_skeleton.bin"
        skeleton_bytes, skeleton_crc = write_bytes_atomic(
            skeleton_path,
            (pack_node(read_node(data, nodes_offset, source_index),
                       block_id if block_id != INVALID_BLOCK else skeleton_children[index][0],
                       0 if block_id != INVALID_BLOCK else skeleton_children[index][1],
                       GPU_NODE_FLAG_BLOCK_PROXY if block_id != INVALID_BLOCK else 0)
             for index, (source_index, block_id) in enumerate(skeleton_entries)))

        blocks_path = v2_dir / "tree_v3_blocks.bin"
        blocks_partial = Path(str(blocks_path) + ".partial")
        directory_path = v2_dir / "tree_v3_block_directory.bin"
        block_records: list[bytes] = []
        block_page_ids: list[int] = []
        block_node_offset = max_block_nodes = 0
        block_bytes = block_crc = 0
        blocks_partial.unlink(missing_ok=True)
        try:
            with blocks_partial.open("wb") as blocks_output:
                for source_root in block_roots:
                    local_sources = [source_root]
                    local_children = [(0, 0)]
                    local_queue = deque([0])
                    max_depth = 0
                    while local_queue:
                        local_index = local_queue.popleft()
                        source_index = local_sources[local_index]
                        record = read_node(data, nodes_offset, source_index)
                        child_start, child_count, depth = record[0], record[1], record[2]
                        max_depth = max(max_depth, depth)
                        first_local_child = len(local_sources)
                        for child in range(child_start, child_start + child_count):
                            local_sources.append(child)
                            local_queue.append(len(local_sources) - 1)
                        local_children[local_index] = (first_local_child, child_count)
                        local_children.extend([(0, 0)] * child_count)
                    pages_for_block: set[int] = set()
                    for local_index, source_index in enumerate(local_sources):
                        first_child, child_count = local_children[local_index]
                        source_record = read_node(data, nodes_offset, source_index)
                        pages_for_block.add(source_record[4])
                        packed = pack_node(source_record, first_child, child_count)
                        blocks_output.write(packed)
                        block_crc = zlib.crc32(packed, block_crc)
                        block_bytes += len(packed)
                    root_record = read_node(data, nodes_offset, source_root)
                    first_page = len(block_page_ids)
                    sorted_pages = sorted(pages_for_block)
                    block_page_ids.extend(sorted_pages)
                    block_records.append(BLOCK_RECORD.pack(
                        source_root, block_node_offset, len(local_sources), root_record[12],
                        max_depth, root_record[4], first_page, len(sorted_pages)))
                    block_node_offset += len(local_sources)
                    max_block_nodes = max(max_block_nodes, len(local_sources))
                blocks_output.flush(); os.fsync(blocks_output.fileno())
            os.replace(blocks_partial, blocks_path)
        except Exception:
            blocks_partial.unlink(missing_ok=True)
            raise
        block_crc &= 0xFFFFFFFF
        directory_bytes, directory_crc = write_bytes_atomic(directory_path, block_records)
        block_pages_path = v2_dir / "tree_v3_block_pages.bin"
        block_pages_bytes, block_pages_crc = write_bytes_atomic(
            block_pages_path, (struct.pack("<I", page_id) for page_id in block_page_ids))
        nonproxy_skeleton_nodes = sum(1 for _, block_id in skeleton_entries
                                      if block_id == INVALID_BLOCK)
        if nonproxy_skeleton_nodes + block_node_offset != node_count:
            raise TreeV3Error("Tree v3 partition does not cover every Tree v2 node exactly once")

    manifest = {
        "schema": SCHEMA, "version": VERSION, "converter_version": CONVERTER_VERSION,
        "tree_v3_key": tree_v3_key,
        "source_tree_v2": {"manifest": "manifest.json", "tree_sha256": source_sha,
                           "nodes": node_count, "exact_leaves": header["leaf_count"]},
        "partition": {"max_block_leaves": max_block_leaves,
                      "skeleton_nodes": len(skeleton_entries),
                      "nonproxy_skeleton_nodes": nonproxy_skeleton_nodes,
                      "blocks": len(block_roots), "block_nodes": block_node_offset,
                      "max_block_nodes": max_block_nodes},
        "skeleton": {"file": skeleton_path.name, "bytes": skeleton_bytes,
                     "records": len(skeleton_entries), "record_bytes": GPU_NODE.size,
                     "crc32": skeleton_crc, "sha256": sha256_file(skeleton_path)},
        "block_directory": {"file": directory_path.name, "bytes": directory_bytes,
                            "records": len(block_records), "record_bytes": BLOCK_RECORD.size,
                            "crc32": directory_crc, "sha256": sha256_file(directory_path)},
        "blocks": {"file": blocks_path.name, "bytes": block_bytes,
                   "records": block_node_offset, "record_bytes": GPU_NODE.size,
                   "crc32": block_crc, "sha256": sha256_file(blocks_path)},
        "block_pages": {"file": block_pages_path.name, "bytes": block_pages_bytes,
                        "records": len(block_page_ids), "record_bytes": 4,
                        "crc32": block_pages_crc, "sha256": sha256_file(block_pages_path)},
        "invariants": {"lossless_exact_leaves": True, "page_addresses_unchanged": True,
                       "tree_v2_compatible": True, "cpu_fallback_preserved": True},
    }
    temporary = Path(str(manifest_path) + ".partial")
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, manifest_path)
    return {"cache_hit": False, "output_dir": str(v2_dir), "manifest": manifest}

def verify_v3(v2_dir: Path) -> dict[str, object]:
    v2_dir = v2_dir.resolve()
    manifest = json.loads((v2_dir / "tree_v3_manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA or manifest.get("version") != VERSION:
        raise TreeV3Error("Tree v3 manifest is incompatible")
    for section in ("skeleton", "block_directory", "blocks", "block_pages"):
        info = manifest[section]
        path = v2_dir / info["file"]
        if path.stat().st_size != info["bytes"] or sha256_file(path) != info["sha256"]:
            raise TreeV3Error(f"Tree v3 {section} size/hash mismatch")
        crc = 0
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(8 * 1024 * 1024), b""):
                crc = zlib.crc32(chunk, crc)
        if crc & 0xFFFFFFFF != info["crc32"]:
            raise TreeV3Error(f"Tree v3 {section} CRC mismatch")
    partition = manifest["partition"]
    if manifest["skeleton"]["records"] != partition["skeleton_nodes"]:
        raise TreeV3Error("Tree v3 skeleton count mismatch")
    if manifest["block_directory"]["records"] != partition["blocks"]:
        raise TreeV3Error("Tree v3 block directory count mismatch")
    if manifest["blocks"]["records"] != partition["block_nodes"]:
        raise TreeV3Error("Tree v3 block node count mismatch")
    return {"tree_v3_valid": True, **partition}

def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build lossless block-streamable NanoGS Tree v3 sidecars")
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build")
    build.add_argument("tree_v2_dir", type=Path)
    build.add_argument("--max-block-leaves", type=int, default=4096)
    build.add_argument("--force", action="store_true")
    verify = sub.add_parser("verify")
    verify.add_argument("tree_v2_dir", type=Path)
    args = parser.parse_args(argv)
    try:
        result = build_v3(args.tree_v2_dir, args.max_block_leaves, args.force) if args.command == "build" else verify_v3(args.tree_v2_dir)
        json.dump(result, sys.stdout, indent=2, sort_keys=True); sys.stdout.write("\n")
        return 0
    except (TreeV3Error, v2.TreeBuildError, OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

if __name__ == "__main__":
    raise SystemExit(main())
