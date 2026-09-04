from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import prepare_nanogs_tree as prepare


class DescriptorTest(unittest.TestCase):
    def test_crop_arguments_are_mutually_exclusive(self) -> None:
        parser = prepare.make_parser()
        with self.assertRaises(SystemExit):
            parser.parse_args(["scene.ply", "--auto-crop", "--crop-config", "crop.json"])

    def test_descriptor_is_atomic_and_project_relative(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            output = project / "Content" / "NanoGSData" / "TreeV2" / "scene" / "key"
            output.mkdir(parents=True)
            manifest = {
                "tree_key": "key",
                "source": {"sha256": "source"},
                "tree": {"sha256": "tree"},
                "pages": {"sha256": "pages"},
            }
            descriptor = prepare.write_descriptor(output, project, manifest)
            text = descriptor.read_text(encoding="utf-8")
            self.assertIn('"project_relative_directory": "Content/NanoGSData/TreeV2/scene/key"', text)
            self.assertFalse(Path(str(descriptor) + ".partial").exists())

    def test_descriptor_rejects_non_runtime_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            output = project / "Saved" / "TreeV2"
            output.mkdir(parents=True)
            with self.assertRaises(prepare.PrepareError):
                prepare.write_descriptor(output, project, {})

    def test_verification_stamp_requires_unchanged_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / "tree.ngst").write_bytes(b"tree")
            (output / "tree_nodes.ngsp").write_bytes(b"pages")
            manifest = {
                "tree_key": "key",
                "tree": {"file": "tree.ngst"},
                "pages": {"file": "tree_nodes.ngsp"},
            }
            verification = {"tree_valid": True}
            prepare.write_verification_stamp(output, manifest, verification)
            self.assertEqual(prepare.load_verification_stamp(output, manifest), verification)
            (output / "tree.ngst").write_bytes(b"changed")
            self.assertIsNone(prepare.load_verification_stamp(output, manifest))

    def test_prepare_builds_and_verifies_tree_v3(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            source = project / "scene.ply"
            source.write_bytes(b"ply")
            output = project / "Content/NanoGSData/TreeV2/scene/key"
            output.mkdir(parents=True)
            manifest = {
                "tree_key": "key",
                "source": {"sha256": "source"},
                "tree": {"file": "tree.ngst", "sha256": "tree"},
                "pages": {"file": "tree_nodes.ngsp", "sha256": "pages"},
            }
            (output / "tree.ngst").write_bytes(b"tree")
            (output / "tree_nodes.ngsp").write_bytes(b"pages")
            spark_result = {"cache_hit": True, "cache_dir": str(project / "spark")}
            tree_result = {"cache_hit": True, "output_dir": str(output), "manifest": manifest}
            with mock.patch.object(prepare.spark_cache, "build", return_value=spark_result), \
                 mock.patch.object(prepare.tree_builder, "build_tree", return_value=tree_result), \
                 mock.patch.object(prepare.tree_builder, "verify_tree", return_value={"tree_valid": True}), \
                 mock.patch.object(prepare.tree_v3_builder, "build_v3", return_value={"cache_hit": False}) as build_v3, \
                 mock.patch.object(prepare.tree_v3_builder, "verify_v3", return_value={"tree_v3_valid": True}):
                result = prepare.prepare(
                    source, project, project / "builder", project / "spark",
                    project / "Content/NanoGSData/TreeV2/scene", 65536,
                )
            self.assertFalse(result["tree_v3_cache_hit"])
            self.assertTrue(result["tree_v3_verification"]["tree_v3_valid"])
            build_v3.assert_called_once_with(
                output.resolve(), max_block_leaves=4096, force=False
            )


if __name__ == "__main__":
    unittest.main()
