#!/usr/bin/env python3
from __future__ import annotations

import array
import unittest

import build_nanogs_tree_v2 as tree


class TreeMetadataTest(unittest.TestCase):
    def test_depth_and_subtree_leaf_counts(self) -> None:
        counts = array.array("H", [2, 2, 0, 0, 0])
        starts = array.array("I", [1, 3, 0, 0, 0])
        depths, leaves = tree.compute_tree_metadata(counts, starts)
        self.assertEqual(list(depths), [0, 1, 1, 2, 2])
        self.assertEqual(list(leaves), [3, 2, 1, 1, 1])

    def test_duplicate_child_is_rejected(self) -> None:
        counts = array.array("H", [2, 1, 0, 0])
        starts = array.array("I", [1, 2, 0, 0])
        with self.assertRaisesRegex(tree.TreeBuildError, "duplicate child"):
            tree.compute_tree_metadata(counts, starts)

    def test_partitioned_page_indices_keep_leaves_contiguous(self) -> None:
        counts = array.array("H", [2, 0, 2, 0, 0])
        self.assertEqual(
            list(tree.partitioned_page_indices(counts, leaf_count=3)),
            [3, 0, 4, 1, 2],
        )


if __name__ == "__main__":
    unittest.main()
