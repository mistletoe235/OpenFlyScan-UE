import json
import struct
import tempfile
import unittest
from pathlib import Path

import build_nanogs_tree_v2 as v2
import build_nanogs_tree_v3 as v3


class TreeV3BuilderTests(unittest.TestCase):
    def test_partitions_losslessly_and_preserves_page_addresses(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            records_path = directory / "records.bin"
            nodes = [
                (1, 2, 0, v2.rad.INTERNAL_SOURCE_ID, 0, 4, 0.0, 0.0, 0.0, 4.0, 8.0, v2.NODE_FLAG_INTERNAL, 4, 0),
                (3, 2, 1, v2.rad.INTERNAL_SOURCE_ID, 0, 5, -1.0, 0.0, 0.0, 2.0, 4.0, v2.NODE_FLAG_INTERNAL, 2, 0),
                (5, 2, 1, v2.rad.INTERNAL_SOURCE_ID, 0, 6, 1.0, 0.0, 0.0, 2.0, 4.0, v2.NODE_FLAG_INTERNAL, 2, 0),
                (0, 0, 2, 0, 0, 0, -1.5, 0.0, 0.0, 1.0, 2.0, v2.NODE_FLAG_EXACT_LEAF, 1, 0),
                (0, 0, 2, 1, 0, 1, -0.5, 0.0, 0.0, 1.0, 2.0, v2.NODE_FLAG_EXACT_LEAF, 1, 0),
                (0, 0, 2, 2, 0, 2, 0.5, 0.0, 0.0, 1.0, 2.0, v2.NODE_FLAG_EXACT_LEAF, 1, 0),
                (0, 0, 2, 3, 0, 3, 1.5, 0.0, 0.0, 1.0, 2.0, v2.NODE_FLAG_EXACT_LEAF, 1, 0),
            ]
            records_path.write_bytes(b"".join(v2.TREE_NODE.pack(*node) for node in nodes))
            tree_info = v2.write_tree_file(directory / "tree.ngst", records_path, 7, 65536, 3, 4, 4)
            (directory / "manifest.json").write_text(json.dumps({
                "schema": "nanogs.tree_v2", "version": 2,
                "tree": {**tree_info, "file": "tree.ngst", "nodes": 7,
                         "exact_leaves": 4, "internal_nodes": 3},
            }), encoding="utf-8")

            result = v3.build_v3(directory, max_block_leaves=2)
            manifest = result["manifest"]
            self.assertEqual(manifest["partition"]["skeleton_nodes"], 3)
            self.assertEqual(manifest["partition"]["nonproxy_skeleton_nodes"], 1)
            self.assertEqual(manifest["partition"]["blocks"], 2)
            self.assertEqual(manifest["partition"]["block_nodes"], 6)
            self.assertEqual(manifest["partition"]["max_block_nodes"], 3)
            self.assertTrue(manifest["invariants"]["lossless_exact_leaves"])
            self.assertTrue(v3.verify_v3(directory)["tree_v3_valid"])

            skeleton = (directory / "tree_v3_skeleton.bin").read_bytes()
            left_proxy = v3.GPU_NODE.unpack_from(skeleton, v3.GPU_NODE.size)
            self.assertTrue(left_proxy[5] & v3.GPU_NODE_FLAG_BLOCK_PROXY)
            self.assertEqual(left_proxy[4], 0)
            block_directory = (directory / "tree_v3_block_directory.bin").read_bytes()
            first_block = v3.BLOCK_RECORD.unpack_from(block_directory)
            self.assertEqual(first_block[2:4], (3, 2))
            page_ids = struct.unpack("<2I", (directory / "tree_v3_block_pages.bin").read_bytes())
            self.assertEqual(page_ids, (0, 0))

            self.assertTrue(v3.build_v3(directory, max_block_leaves=2)["cache_hit"])


if __name__ == "__main__":
    unittest.main()
