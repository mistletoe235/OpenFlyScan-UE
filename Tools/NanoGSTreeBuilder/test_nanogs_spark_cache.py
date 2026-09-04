#!/usr/bin/env python3
from __future__ import annotations

import json
import stat
import tempfile
import unittest
from pathlib import Path

import nanogs_spark_cache as cache


FAKE_BUILDER = r'''#!/usr/bin/env python3
import pathlib
import sys

source = pathlib.Path(sys.argv[-1])
base = source.stem + "-lod"
pathlib.Path(base + ".rad").write_bytes(b"RAD" + source.read_bytes())
pathlib.Path(base + "-0.radc").write_bytes(b"RADC0")
pathlib.Path(base + "-1.radc").write_bytes(b"RADC1")
pathlib.Path(base + ".source_ids.bin").write_bytes(b"SOURCE_IDS")
print("Read: num_splats: 8 with sh_degree: 3")
print("Output set: 12 / 15")
print("LoD growth factor: 1.5")
print("Root #children: 4")
'''

INVALID_BUILDER = r'''#!/usr/bin/env python3
print("Found 3 invalid splats")
print("Stopping processing due to invalid splats!")
'''


class SparkCacheTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "scene.ply"
        self.source.write_bytes(b"ply fixture one")
        self.builder = self.root / "build-lod"
        self.builder.write_text(FAKE_BUILDER, encoding="utf-8")
        self.builder.chmod(self.builder.stat().st_mode | stat.S_IXUSR)
        self.cache_root = self.root / "cache"
        self.options = cache.BuildOptions()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_build_then_hit_same_immutable_cache(self) -> None:
        first = cache.build(self.source, self.builder, self.cache_root, self.options)
        self.assertFalse(first["cache_hit"])
        cache_dir = Path(first["cache_dir"])
        self.assertEqual(cache.validate_manifest(cache_dir, verify_hashes=True)["rad_chunks"], 2)
        second = cache.build(self.source, self.builder, self.cache_root, self.options)
        self.assertTrue(second["cache_hit"])
        self.assertEqual(second["cache_dir"], first["cache_dir"])

    def test_renamed_identical_source_reuses_content_cache(self) -> None:
        first = cache.build(self.source, self.builder, self.cache_root, self.options)
        renamed = self.root / "renamed_scene.ply"
        renamed.write_bytes(self.source.read_bytes())
        second = cache.build(renamed, self.builder, self.cache_root, self.options)
        self.assertTrue(second["cache_hit"])
        self.assertEqual(second["cache_dir"], first["cache_dir"])

    def test_source_builder_and_options_invalidate_key(self) -> None:
        base = cache.plan(self.source, self.builder, self.cache_root, self.options)
        self.source.write_bytes(b"ply fixture two")
        changed_source = cache.plan(self.source, self.builder, self.cache_root, self.options)
        self.assertNotEqual(base["cache_key"], changed_source["cache_key"])
        self.builder.write_text(FAKE_BUILDER + "\n# changed\n", encoding="utf-8")
        changed_builder = cache.plan(self.source, self.builder, self.cache_root, self.options)
        self.assertNotEqual(changed_source["cache_key"], changed_builder["cache_key"])
        changed_options = cache.plan(
            self.source,
            self.builder,
            self.cache_root,
            cache.BuildOptions(method="quick", max_sh=0),
        )
        self.assertNotEqual(changed_builder["cache_key"], changed_options["cache_key"])

    def test_compact_encoding_rejects_exact_leaf_sidecar(self) -> None:
        with self.assertRaisesRegex(cache.CacheError, "requires gsplat"):
            cache.build(
                self.source,
                self.builder,
                self.cache_root,
                cache.BuildOptions(encoding="csplat"),
            )

    def test_corrupt_file_is_rebuilt_without_force(self) -> None:
        result = cache.build(self.source, self.builder, self.cache_root, self.options)
        cache_dir = Path(result["cache_dir"])
        manifest = json.loads((cache_dir / "manifest.json").read_text(encoding="utf-8"))
        (cache_dir / manifest["rad_header"]).write_bytes(b"")
        rebuilt_plan = cache.plan(self.source, self.builder, self.cache_root, self.options)
        self.assertFalse(rebuilt_plan["cache_hit"])
        rebuilt = cache.build(self.source, self.builder, self.cache_root, self.options)
        self.assertFalse(rebuilt["cache_hit"])
        cache.validate_manifest(cache_dir, verify_hashes=True)

    def test_zero_exit_without_outputs_reports_invalid_splats(self) -> None:
        self.builder.write_text(INVALID_BUILDER, encoding="utf-8")
        self.builder.chmod(self.builder.stat().st_mode | stat.S_IXUSR)
        with self.assertRaisesRegex(
            cache.CacheError,
            "(?s)rejected non-finite splats.*Stopping processing due to invalid splats",
        ):
            cache.build(self.source, self.builder, self.cache_root, self.options)


if __name__ == "__main__":
    unittest.main()
