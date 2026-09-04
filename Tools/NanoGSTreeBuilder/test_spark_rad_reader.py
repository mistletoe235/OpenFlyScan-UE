#!/usr/bin/env python3
from __future__ import annotations

import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

import spark_rad_reader as reader


def prefixed(magic: bytes, meta: dict[str, object], tail: bytes = b"") -> bytes:
    encoded = json.dumps(meta, separators=(",", ":")).encode("utf-8")
    return magic + struct.pack("<I", len(encoded)) + encoded + b"\0" * ((-len(encoded)) & 7) + tail


def raw_deflate(data: bytes) -> bytes:
    encoder = zlib.compressobj(level=6, wbits=-15)
    return encoder.compress(data) + encoder.flush()


class SparkRadReaderTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_tree(self, duplicate_parent: bool = False) -> None:
        counts = struct.pack("<5H", 2, 2, 0, 0, 0)
        starts = struct.pack("<5I", 1, 2 if duplicate_parent else 3, 0, 0, 0)
        encoded_counts = raw_deflate(counts)
        encoded_starts = raw_deflate(starts)
        payload = encoded_counts
        start_offset = (len(payload) + 7) & ~7
        payload += b"\0" * (start_offset - len(payload)) + encoded_starts
        chunk_meta = {
            "version": 1,
            "base": 0,
            "count": 5,
            "payloadBytes": len(payload),
            "lodTree": True,
            "properties": [
                {"offset": 0, "bytes": len(encoded_counts), "property": "child_count", "encoding": "u16", "compression": "gz"},
                {"offset": start_offset, "bytes": len(encoded_starts), "property": "child_start", "encoding": "u32", "compression": "gz"},
            ],
        }
        radc = prefixed(reader.RADC_MAGIC, chunk_meta, struct.pack("<Q", len(payload)) + payload)
        (self.root / "scene-lod-0.radc").write_bytes(radc)
        rad_meta = {
            "version": 1,
            "type": "gsplat",
            "count": 5,
            "lodTree": True,
            "chunks": [{"offset": 0, "bytes": len(radc), "filename": "scene-lod-0.radc"}],
        }
        (self.root / "scene-lod.rad").write_bytes(prefixed(reader.RAD_MAGIC, rad_meta))
        source_ids = [reader.INTERNAL_SOURCE_ID, reader.INTERNAL_SOURCE_ID, 0, 1, 2]
        sidecar = reader.SOURCE_ID_HEADER.pack(
            reader.SOURCE_ID_MAGIC, 1, 0, 5, 3, 3
        ) + struct.pack("<5Q", *source_ids)
        (self.root / "scene-lod.source_ids.bin").write_bytes(sidecar)

    def test_valid_tree(self) -> None:
        self.write_tree()
        result = reader.validate_tree(self.root)
        self.assertTrue(result["tree_valid"])
        self.assertEqual(result["internal_nodes"], 2)
        self.assertEqual(result["leaf_nodes"], 3)
        self.assertEqual(result["source_leaf_ids"], 3)

    def test_duplicate_source_id_is_rejected(self) -> None:
        self.write_tree()
        path = self.root / "scene-lod.source_ids.bin"
        data = bytearray(path.read_bytes())
        struct.pack_into("<Q", data, reader.SOURCE_ID_HEADER.size + 4 * 8, 1)
        path.write_bytes(data)
        with self.assertRaisesRegex(reader.RadError, "duplicate source ID"):
            reader.validate_tree(self.root)

    def test_multiple_parent_is_rejected(self) -> None:
        self.write_tree(duplicate_parent=True)
        with self.assertRaisesRegex(reader.RadError, "multiple parents"):
            reader.validate_tree(self.root)

    def test_truncated_chunk_is_rejected(self) -> None:
        self.write_tree()
        path = self.root / "scene-lod-0.radc"
        path.write_bytes(path.read_bytes()[:-1])
        with self.assertRaisesRegex(reader.RadError, "byte count mismatch"):
            reader.validate_tree(self.root)

    def test_float_encoding_decoders_match_layout(self) -> None:
        planar = struct.pack("<6f", 1.0, 4.0, 2.0, 5.0, 3.0, 6.0)
        self.assertEqual(
            list(reader.decode_planar_floats(planar, "f32", 3, 2)),
            [1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
        )
        delta = bytes((10, 5, 20, 5, 30, 5))
        decoded = reader.decode_planar_floats(delta, "r8_delta", 3, 2, 0.0, 1.0)
        expected = [10/255, 20/255, 30/255, 15/255, 25/255, 35/255]
        for actual, wanted in zip(decoded, expected):
            self.assertAlmostEqual(actual, wanted, places=6)

    def test_oct88_identity_is_finite_and_normalized(self) -> None:
        payload = bytes((128, 128, 0))
        meta = {
            "version": 1, "base": 0, "count": 1, "payloadBytes": 3,
            "properties": [{"offset": 0, "bytes": 3, "property": "orientation", "encoding": "oct88r8"}],
        }
        quat = reader.decode_quaternion(reader.RadChunk(Path("fixture"), meta, payload))
        self.assertAlmostEqual(sum(value * value for value in quat), 1.0, places=6)
        self.assertAlmostEqual(quat[3], 1.0, places=6)


if __name__ == "__main__":
    unittest.main()
