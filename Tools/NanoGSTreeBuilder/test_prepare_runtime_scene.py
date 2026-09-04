from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import prepare_runtime_scene as runtime


class RuntimeSceneTest(unittest.TestCase):
    def test_writes_atomic_runtime_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "scene.ply"
            source.write_bytes(b"ply")
            config = root / "active.json"
            config.write_text("{}", encoding="utf-8")
            output_dir = root / "Content/NanoGSData/RuntimeActive/Trees/scene/key"
            output_dir.mkdir(parents=True)
            descriptor = output_dir / "tree.ngstree"
            descriptor.write_text(json.dumps({
                "source_sha256": "source-hash", "tree_key": "tree-key"
            }), encoding="utf-8")
            normalized = {
                "page_points": 65536,
                "crop": {"mode": "none", "config": None, "tail_quantile": 0.001,
                         "protect_half": "none"},
                "unreal": {},
            }
            result = {
                "descriptor": str(descriptor), "output_dir": str(output_dir),
                "effective_source": str(source), "crop": None,
                "spark_cache_hit": True, "tree_cache_hit": False,
                "tree_v3_cache_hit": True,
            }
            settings = {
                "transform": {"location": [0, 0, 0], "rotation": [0, 0, 0],
                              "scale": [-1, 1, 1]},
                "component": {"sh_order": 3},
                "player_start": {"location": [1, 2, 3], "rotation": [0, 0, 0]},
                "collision_floor": {"location": [4, 5, 6], "scale": [7, 8, 0.2]},
            }
            with mock.patch.object(runtime.active_scene, "load_config", return_value=normalized), \
                 mock.patch.object(runtime.tree_prepare, "prepare", return_value=result), \
                 mock.patch.object(runtime.active_scene, "resolve_unreal_settings",
                                   return_value=(settings, {"global_ground_z_cm": 0.0})):
                value = runtime.prepare_runtime_scene(source, root, config)
            output = root / "Content/NanoGSData/RuntimeActive/active_scene.runtime.json"
            saved = json.loads(output.read_text())
            self.assertEqual(saved["schema"], runtime.RUNTIME_SCHEMA)
            self.assertEqual(saved["tree_directory"],
                             "Content/NanoGSData/RuntimeActive/Trees/scene/key")
            self.assertEqual(saved["player_start"]["location"], [1, 2, 3])
            self.assertEqual(value["runtime_scene_file"], str(output))
            self.assertFalse(Path(str(output) + ".partial").exists())
            before = output.read_bytes()
            with mock.patch.object(runtime.active_scene, "load_config", return_value=normalized), \
                 mock.patch.object(runtime.tree_prepare, "prepare", return_value=result), \
                 mock.patch.object(runtime.active_scene, "resolve_unreal_settings",
                                   side_effect=runtime.active_scene.ActiveSceneError("no flat, continuous ground")):
                with self.assertRaisesRegex(runtime.active_scene.ActiveSceneError, "no flat"):
                    runtime.prepare_runtime_scene(source, root, config)
            self.assertEqual(output.read_bytes(), before)

    def test_camera_clearance_uses_actual_profile_and_ned_z(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "scene.ply"
            source.write_bytes(b"ply")
            profile = root / "camera.json"
            profile.write_text(json.dumps({"ViewMode": "FlyWithMe",
                "CameraDirector": {"X": -9, "Y": 2, "Z": -6}}))
            normalized = {"unreal": {"ground": {"flat_spawn": {"enabled": True}}},
                          "crop": {"mode": "none", "tail_quantile": .02, "protect_half": "none"},
                          "page_points": 65536}
            with mock.patch.object(runtime.active_scene, "load_config", return_value=normalized), \
                 mock.patch.object(runtime.tree_prepare, "prepare", side_effect=RuntimeError("stop")):
                with self.assertRaisesRegex(RuntimeError, "stop"):
                    runtime.prepare_runtime_scene(source, root, root / "config.json", camera_settings=profile)
            self.assertEqual(normalized["unreal"]["ground"]["flat_spawn"]["camera_offset_cm"], [-900, 200, 600])

    def test_world_scale_multiplies_configured_actor_scale(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "scene.ply"
            source.write_bytes(b"ply")
            config_path = root / "active.json"
            config_path.write_text("{}", encoding="utf-8")
            normalized = {
                "page_points": 65536,
                "crop": {"mode": "none", "config": None, "tail_quantile": 0.001,
                         "protect_half": "none"},
                "unreal": {"transform": {"scale": [-1.0, 1.0, 1.0]}},
            }
            with mock.patch.object(runtime.active_scene, "load_config", return_value=normalized), \
                 mock.patch.object(runtime.tree_prepare, "prepare", side_effect=RuntimeError("stop")):
                with self.assertRaisesRegex(RuntimeError, "stop"):
                    runtime.prepare_runtime_scene(
                        source, root, config_path, world_scale=10.0
                    )
            self.assertEqual(
                normalized["unreal"]["transform"]["scale"], [-10.0, 10.0, 10.0]
            )


if __name__ == "__main__":
    unittest.main()
