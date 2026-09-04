from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import prepare_active_scene as active


def config(source: str = "Data/NanoGS/Active/scene.ply") -> dict[str, object]:
    return {
        "schema": active.CONFIG_SCHEMA,
        "version": 1,
        "source_ply": source,
        "page_points": 65536,
        "crop": {"mode": "none", "config": None},
        "unreal": {
            "source_asset_path": "/Game/NanoGS/Template/ActiveScene_TreeSource",
            "target_level_path": "/Game/NanoGS/Template/NanoGS_Runtime",
            "actor_label": "NanoGS_ActiveScene",
        },
    }


class ActiveSceneTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.project = Path(self.temporary.name)
        self.config_path = self.project / "Config" / "NanoGS" / "active_scene.json"
        self.config_path.parent.mkdir(parents=True)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_config(self, value: dict[str, object]) -> None:
        self.config_path.write_text(json.dumps(value), encoding="utf-8")

    def test_default_source_and_stable_asset_paths(self) -> None:
        self.write_config(config())
        loaded = active.load_config(self.config_path, self.project)
        self.assertEqual(loaded["source_ply"], str(self.project / "Data/NanoGS/Active/scene.ply"))
        self.assertEqual(loaded["unreal"]["source_asset_path"], "/Game/NanoGS/Template/ActiveScene_TreeSource")
        self.assertEqual(loaded["unreal"]["target_level_path"], "/Game/NanoGS/Template/NanoGS_Runtime")
        self.assertEqual(loaded["unreal"]["transform"]["scale"], [-1.0, 1.0, 1.0])
        self.assertEqual(loaded["unreal"]["component"]["tree_lod_splat_budget"], 8_000_000)
        self.assertEqual(
            loaded["unreal"]["component"]["tree_lod_progressive_splat_budget"], 4_000_000
        )
        self.assertEqual(loaded["unreal"]["component"]["tree_lod_detail_scale"], 1.0)

    def test_spark_comparison_settings_are_preserved(self) -> None:
        value = config()
        value["unreal"]["component"] = {
            "tree_lod_splat_budget": 10_000_000,
            "tree_lod_progressive_splat_budget": 4_000_000,
            "tree_lod_detail_scale": 3.0,
        }
        self.write_config(value)
        loaded = active.load_config(self.config_path, self.project)
        self.assertEqual(loaded["unreal"]["component"]["tree_lod_splat_budget"], 10_000_000)
        self.assertEqual(
            loaded["unreal"]["component"]["tree_lod_progressive_splat_budget"], 4_000_000
        )
        self.assertEqual(loaded["unreal"]["component"]["tree_lod_detail_scale"], 3.0)

    def test_rejects_invalid_spark_comparison_settings(self) -> None:
        invalid_values = (
            ("tree_lod_splat_budget", 99_999, "tree_lod_splat_budget"),
            ("tree_lod_splat_budget", 30_000_001, "tree_lod_splat_budget"),
            ("tree_lod_progressive_splat_budget", -1, "tree_lod_progressive_splat_budget"),
            ("tree_lod_progressive_splat_budget", 30_000_001, "tree_lod_progressive_splat_budget"),
            ("tree_lod_detail_scale", 0.09, "tree_lod_detail_scale"),
            ("tree_lod_detail_scale", 4.01, "tree_lod_detail_scale"),
        )
        for property_name, property_value, error_pattern in invalid_values:
            with self.subTest(property_name=property_name, property_value=property_value):
                value = config()
                value["unreal"]["component"] = {property_name: property_value}
                self.write_config(value)
                with self.assertRaisesRegex(active.ActiveSceneError, error_pattern):
                    active.load_config(self.config_path, self.project)

    def test_auto_ground_resolves_player_and_hidden_floor(self) -> None:
        value = config()
        value["unreal"]["ground"] = {"mode": "auto"}
        value["unreal"]["player_start"] = {
            "enabled": True, "location": [10, 20, 150], "xy_mode": "configured",
            "height_above_ground_cm": 120,
        }
        value["unreal"]["collision_floor"] = {
            "enabled": True, "margin": 1.1, "thickness_cm": 20,
            "below_ground_cm": 2, "hidden_in_game": True, "hidden_in_editor": True,
        }
        self.write_config(value)
        loaded = active.load_config(self.config_path, self.project)
        source = self.project / "scene.ply"
        descriptor = self.project / "tree.ngstree"
        descriptor.write_text(json.dumps({"source_sha256": "hash"}), encoding="utf-8")
        result = {"effective_source": str(source)}
        estimate = {
            "bounds_min_cm": [-1000.0, -500.0, -100.0],
            "bounds_max_cm": [3000.0, 1500.0, 2000.0],
            "anchor_ground_z_cm": 40.0,
        }
        with mock.patch.object(active.ground_plane, "estimate_cached", return_value=estimate):
            settings, resolved = active.resolve_unreal_settings(
                self.project, loaded, result, {"source_sha256": "hash"}
            )
        self.assertIs(resolved, estimate)
        self.assertEqual(settings["player_start"]["location"], [10.0, 20.0, 160.0])
        self.assertEqual(settings["collision_floor"]["location"], [1000.0, 500.0, 28.0])
        self.assertEqual(settings["collision_floor"]["scale"], [44.0, 22.0, 0.2])
        self.assertTrue(settings["collision_floor"]["hidden_in_editor"])

    def test_auto_open_uses_recommended_interior_anchor(self) -> None:
        value = config()
        value["unreal"]["ground"] = {"mode": "auto"}
        value["unreal"]["player_start"] = {
            "enabled": True, "location": [0, 0, 150], "xy_mode": "auto_open",
            "height_above_ground_cm": 125,
        }
        value["unreal"]["collision_floor"] = {"enabled": False}
        self.write_config(value)
        loaded = active.load_config(self.config_path, self.project)
        estimate = {
            "bounds_min_cm": [-1000.0, -500.0, -100.0],
            "bounds_max_cm": [3000.0, 1500.0, 2000.0],
            "anchor_ground_z_cm": 40.0,
            "recommended_player_xy_cm": [500.0, 250.0],
            "recommended_player_ground_z_cm": 30.0,
        }
        with mock.patch.object(active.ground_plane, "estimate_cached", return_value=estimate):
            settings, _ = active.resolve_unreal_settings(
                self.project, loaded, {"effective_source": str(self.project / "scene.ply")},
                {"source_sha256": "hash"},
            )
        self.assertEqual(settings["player_start"]["location"], [500.0, 250.0, 155.0])

    def test_auto_vertical_selects_lower_risk_orientation(self) -> None:
        value = config()
        value["unreal"]["ground"] = {
            "mode": "auto", "orientation_mode": "auto_vertical"
        }
        value["unreal"]["player_start"] = {
            "enabled": True, "location": [0, 0, 150], "xy_mode": "auto_open",
            "height_above_ground_cm": 125,
        }
        value["unreal"]["collision_floor"] = {"enabled": False}
        self.write_config(value)
        loaded = active.load_config(self.config_path, self.project)
        common = {
            "bounds_min_cm": [-1000.0, -500.0, -100.0],
            "bounds_max_cm": [3000.0, 1500.0, 2000.0],
            "anchor_ground_z_cm": 40.0,
            "recommended_player_xy_cm": [500.0, 250.0],
            "recommended_player_ground_z_cm": 30.0,
        }
        estimates = [
            {**common, "recommended_player_score": 0.1, "orientation_score": 2.0},
            {**common, "recommended_player_score": 3.0, "orientation_score": 0.5},
        ]
        with mock.patch.object(active.ground_plane, "estimate_cached", side_effect=estimates):
            settings, estimate = active.resolve_unreal_settings(
                self.project, loaded, {"effective_source": str(self.project / "scene.ply")},
                {"source_sha256": "hash"},
            )
        self.assertEqual(settings["transform"]["rotation"], [0.0, 0.0, 180.0])
        self.assertEqual(estimate["orientation_selection"]["selected"], "roll_180")

        # An upside-down candidate without confident ground must not abort the
        # valid other orientation. This is not a relaxed fallback spawn.
        with mock.patch.object(active.ground_plane, "estimate_cached", side_effect=[
            active.ground_plane.GroundPlaneError("no confident ground"), estimates[1]
        ]):
            settings, estimate = active.resolve_unreal_settings(
                self.project, loaded, {"effective_source": str(self.project / "scene.ply")},
                {"source_sha256": "hash"},
            )
        self.assertEqual(settings["transform"]["rotation"], [0.0, 0.0, 180.0])
        self.assertIn("configured", estimate["orientation_selection"]["rejected_candidates"])

        # A blocked upright spawn is an error, not permission to invert the
        # entire point cloud merely because the inverted camera is unobstructed.
        blocked = {**common, "orientation_score": 0.1, "recommended_player_score": 0.1,
                   "spawn_geometry": {"valid": False, "selected": None}}
        with mock.patch.object(active.ground_plane, "estimate_cached", side_effect=[blocked, estimates[1]]):
            with self.assertRaisesRegex(active.ActiveSceneError, "no flat, continuous"):
                active.resolve_unreal_settings(
                    self.project, loaded, {"effective_source": str(self.project / "scene.ply")},
                    {"source_sha256": "hash"})

    def test_source_override_does_not_rewrite_config(self) -> None:
        self.write_config(config())
        override = self.project / "incoming" / "other.ply"
        loaded = active.load_config(self.config_path, self.project, override)
        self.assertEqual(loaded["source_ply"], str(override))
        self.assertEqual(json.loads(self.config_path.read_text())["source_ply"], "Data/NanoGS/Active/scene.ply")

    def test_rejects_invalid_target_path(self) -> None:
        value = config()
        value["unreal"]["target_level_path"] = "NanoGS_Runtime"
        self.write_config(value)
        with self.assertRaisesRegex(active.ActiveSceneError, "must be a /Game package path"):
            active.load_config(self.config_path, self.project)

    def test_missing_ply_fails_before_builder_and_preserves_state(self) -> None:
        self.write_config(config())
        state_path = self.project / "Saved/NanoGSActive/current_scene.json"
        state_path.parent.mkdir(parents=True)
        state_path.write_text('{"old": true}\n', encoding="utf-8")
        with mock.patch.object(active.tree_prepare, "prepare") as prepare:
            with self.assertRaisesRegex(active.ActiveSceneError, "previous UE asset was not changed"):
                active.prepare_active_scene(self.config_path, self.project, state_path)
        prepare.assert_not_called()
        self.assertEqual(json.loads(state_path.read_text()), {"old": True})

    def test_writes_portable_state_after_success(self) -> None:
        self.write_config(config())
        source = self.project / "Data/NanoGS/Active/scene.ply"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"ply")
        output = self.project / "Content/NanoGSData/TreeV2/Active/scene/key"
        output.mkdir(parents=True)
        descriptor = output / "tree.ngstree"
        descriptor.write_text(json.dumps({
            "tree_key": "key",
            "source_sha256": "source-hash",
        }), encoding="utf-8")
        result = {
            "descriptor": str(descriptor),
            "output_dir": str(output),
            "effective_source": str(source),
            "spark_cache_hit": True,
            "tree_cache_hit": True,
            "tree_v3_cache_hit": False,
            "verification": {"tree_valid": True},
            "tree_v3_verification": {"tree_v3_valid": True},
        }
        state_path = self.project / "Saved/NanoGSActive/current_scene.json"
        with mock.patch.object(active.tree_prepare, "prepare", return_value=result):
            state = active.prepare_active_scene(self.config_path, self.project, state_path)
        self.assertEqual(state["descriptor"], "Content/NanoGSData/TreeV2/Active/scene/key/tree.ngstree")
        self.assertEqual(state["tree_key"], "key")
        self.assertIsNone(state["crop"])
        self.assertFalse(state["tree_v3_cache_hit"])
        self.assertTrue(state["tree_v3_verification"]["tree_v3_valid"])
        self.assertEqual(json.loads(state_path.read_text())["source_sha256"], "source-hash")
        self.assertFalse(Path(str(state_path) + ".partial").exists())


if __name__ == "__main__":
    unittest.main()
