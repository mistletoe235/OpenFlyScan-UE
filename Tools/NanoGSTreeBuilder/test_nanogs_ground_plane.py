from __future__ import annotations

import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

import numpy as np

import nanogs_ground_plane as ground


def write_ply(path: Path, points: np.ndarray) -> None:
    header = (
        "ply\n"
        "format binary_little_endian 1.0\n"
        f"element vertex {len(points)}\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "end_header\n"
    ).encode("ascii")
    with path.open("wb") as stream:
        stream.write(header)
        for point in points:
            stream.write(struct.pack("<3f", *point))


class GroundPlaneTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        rng = np.random.default_rng(7)
        xy = np.stack(np.meshgrid(np.linspace(-20, 20, 50), np.linspace(-10, 10, 30)), axis=-1).reshape(-1, 2)
        ground_points = np.column_stack((xy, 1.0 + rng.normal(0.0, 0.01, len(xy))))
        buildings = np.column_stack((
            rng.uniform(-18, 18, 900),
            rng.uniform(-8, 8, 900),
            rng.uniform(4, 25, 900),
        ))
        below_outliers = np.column_stack((
            rng.uniform(-20, 20, 20),
            rng.uniform(-10, 10, 20),
            rng.uniform(-30, -10, 20),
        ))
        self.source = self.root / "scene.ply"
        write_ply(self.source, np.vstack((ground_points, buildings, below_outliers)))
        self.transform = {
            "location": [100.0, 200.0, 300.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [-1.0, 1.0, 1.0],
        }
        self.options = {
            "sample_points": 10_000,
            "chunk_points": 1_000,
            "grid_size": 12,
            "minimum_cell_points": 3,
            "cell_low_quantile": 0.10,
            "bounds_tail_quantile": 0.0,
            "mode_bin_cm": 25.0,
            "anchor_max_delta_cm": 100.0,
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_estimates_ground_despite_buildings_and_low_outliers(self) -> None:
        result = ground.estimate(self.source, self.transform, self.options, [100.0, 200.0])
        self.assertAlmostEqual(result["global_ground_z_cm"], 400.0, delta=5.0)
        self.assertAlmostEqual(result["anchor_ground_z_cm"], 400.0, delta=5.0)
        self.assertLess(result["bounds_min_cm"][0], -1800.0)
        self.assertGreater(result["bounds_max_cm"][0], 2000.0)
        self.assertGreaterEqual(result["candidate_cell_count"], 16)
        self.assertGreaterEqual(result["recommended_player_edge_fraction"], 0.15)
        self.assertAlmostEqual(result["recommended_player_ground_z_cm"], 400.0, delta=5.0)

    def test_cached_result_is_reused(self) -> None:
        cache = self.root / "cache"
        first = ground.estimate_cached(
            self.source, "source-hash", self.transform, self.options, [100.0, 200.0], cache
        )
        second = ground.estimate_cached(
            self.source, "source-hash", self.transform, self.options, [100.0, 200.0], cache
        )
        self.assertFalse(first["cache_hit"])
        self.assertTrue(second["cache_hit"])
        self.assertEqual(first["cache_key"], second["cache_key"])
        cached_files = list(cache.glob("*/ground.json"))
        self.assertEqual(len(cached_files), 1)
        self.assertEqual(json.loads(cached_files[0].read_text())["schema"], ground.SCHEMA)

    def test_collision_bounds_ignore_sparse_xy_outlier(self) -> None:
        options = dict(self.options)
        options.update({"bounds_tail_quantile": 0.01, "grid_size": 8, "minimum_cell_points": 3})
        samples, raw_min, raw_max = ground._sample_world_positions(
            self.source, self.transform, ground.validate_options(options)
        )
        samples = np.vstack((samples, [1_000_000.0, 1_000_000.0, 400.0]))
        with mock.patch.object(
            ground,
            "_sample_world_positions",
            return_value=(
                samples,
                raw_min,
                np.asarray([1_000_000.0, 1_000_000.0, raw_max[2]]),
            ),
        ):
            result = ground.estimate(self.source, self.transform, options, [100.0, 200.0])
        self.assertGreater(result["raw_bounds_max_cm"][0], 900_000.0)
        self.assertLess(result["bounds_max_cm"][0], 100_000.0)

    def test_open_anchor_never_prefers_clear_edge_cell(self) -> None:
        candidates = np.asarray([
            [1.0, 50.0, 0.0, 100.0, 0.0, 0.0],
            [50.0, 50.0, 0.0, 100.0, 0.02, 20.0],
            [75.0, 50.0, 0.0, 100.0, 0.03, 30.0],
        ])
        options = ground.validate_options({
            "sample_points": 1000,
            "chunk_points": 1000,
            "grid_size": 8,
            "minimum_cell_points": 3,
            "edge_margin_fraction": 0.15,
        })
        selected_xy, _, _, edge_fraction = ground._choose_open_anchor(
            candidates,
            np.asarray([0.0, 0.0]),
            np.asarray([100.0, 100.0]),
            0.0,
            options,
        )
        self.assertTrue(np.array_equal(selected_xy, np.asarray([50.0, 50.0])))
        self.assertGreaterEqual(edge_fraction, 0.15)

    def test_open_anchor_fails_when_only_edge_cells_exist(self) -> None:
        candidates = np.asarray([
            [1.0, 50.0, 0.0, 100.0, 0.0, 0.0],
            [99.0, 50.0, 0.0, 100.0, 0.0, 0.0],
        ])
        options = ground.validate_options({
            "sample_points": 1000,
            "chunk_points": 1000,
            "grid_size": 8,
            "minimum_cell_points": 3,
            "edge_margin_fraction": 0.15,
        })
        with self.assertRaisesRegex(ground.GroundPlaneError, "edge_margin_fraction"):
            ground._choose_open_anchor(
                candidates,
                np.asarray([0.0, 0.0]),
                np.asarray([100.0, 100.0]),
                0.0,
                options,
            )

    def test_open_anchor_uses_density_center_not_bbox_midpoint(self) -> None:
        candidates = np.asarray([
            [20.0, 50.0, 0.0, 1000.0, 0.0, 0.0],
            [50.0, 50.0, 0.0, 10.0, 0.0, 0.0],
            *[[20.0, float(y), 0.0, 1000.0, 0.0, 0.0] for y in range(20, 81, 4)],
        ])
        options = ground.validate_options({
            "sample_points": 1000,
            "chunk_points": 1000,
            "grid_size": 8,
            "minimum_cell_points": 3,
            "edge_margin_fraction": 0.1,
            "center_bias": 1.0,
        })
        selected_xy, _, _, _ = ground._choose_open_anchor(
            candidates,
            np.asarray([0.0, 0.0]),
            np.asarray([100.0, 100.0]),
            0.0,
            options,
        )
        self.assertEqual(selected_xy.tolist(), [20.0, 50.0])

    def test_roll_180_transform_is_applied_before_ground_estimation(self) -> None:
        transform = dict(self.transform)
        transform["rotation"] = [0.0, 0.0, 180.0]
        _, bounds_min, bounds_max = ground._sample_world_positions(
            self.source, transform, ground.validate_options(self.options)
        )
        self.assertAlmostEqual(bounds_min[2], 300.0 - 2500.0, delta=10.0)
        self.assertGreater(bounds_max[2], 3000.0)

    def test_sparse_background_shell_does_not_define_spawn_interior(self) -> None:
        rng = np.random.default_rng(42)
        main = np.column_stack((
            rng.uniform(-400, 400, 100_000),
            rng.uniform(-650, 650, 100_000),
            rng.normal(0, 1, 100_000),
        ))
        angle = rng.uniform(0, 2 * np.pi, 2000)
        shell = np.column_stack((10000 * np.cos(angle), 10000 * np.sin(angle), np.zeros(2000)))
        samples = np.vstack((main, shell))
        with mock.patch.object(ground, "_sample_world_positions", return_value=(
            samples, samples.min(axis=0), samples.max(axis=0)
        )):
            result = ground.estimate(self.source, self.transform, {
                "sample_points": 150000, "minimum_cell_points": 12,
            }, [0, 0])
        # Coverage is intentionally unchanged, but spawn margins are evaluated
        # against the dense reconstruction, not the remote background shell.
        self.assertGreater(result["bounds_max_cm"][0], 9000)
        self.assertLess(result["analysis_bounds_xy_max_cm"][0], 400)
        self.assertGreater(result["candidate_cell_count"], 1000)
        x, y = result["recommended_player_xy_cm"]
        self.assertLess(abs(x), 280)
        self.assertLess(abs(y), 455)

    def test_impossible_observer_clearance_does_not_return_first_cell(self) -> None:
        candidates = np.asarray([
            [40, 50, 0, 100, 0, 0], [50, 50, 0, 100, 0, 0],
        ], dtype=float)
        options = ground.validate_options({
            "clearance_radius_cm": 5,
            "observer_offset_xy_cm": [1000, 0],
        })
        with self.assertRaisesRegex(ground.GroundPlaneError, "no finite spawn"):
            ground._choose_open_anchor(
                candidates, np.array([0, 0]), np.array([100, 100]), 0, options
            )

    def test_dense_translucent_flat_surface_is_not_preferred_to_supported_ground(self) -> None:
        candidates = np.asarray([
            [50, 50, 0, 1000, 0, 1, 200, .2],
            [40, 50, 0, 200, .01, 10, 160, .8],
        ], dtype=float)
        xy, _, _, _ = ground._choose_open_anchor(
            candidates, np.array([0, 0]), np.array([100, 100]), 0,
            ground.validate_options({}),
        )
        self.assertEqual(xy.tolist(), [40, 50])

    def test_dense_low_roof_is_not_accepted_as_main_ground(self) -> None:
        candidates = np.asarray([
            [50, 50, 250, 1000, 0, 1, 1000, 1],
            [40, 50, 0, 200, .01, 10, 160, .8],
        ], dtype=float)
        xy, height, _, _ = ground._choose_open_anchor(
            candidates, np.array([0, 0]), np.array([100, 100]), 0,
            ground.validate_options({}),
        )
        self.assertEqual(xy.tolist(), [40, 50])
        self.assertEqual(height, 0)

    def test_source_logit_opacity_is_sampled_without_changing_xyz(self) -> None:
        source = self.root / "with_alpha.ply"
        source.write_bytes(
            b"ply\nformat binary_little_endian 1.0\nelement vertex 3\n"
            b"property float x\nproperty float y\nproperty float z\n"
            b"property float opacity\nend_header\n"
            + b"".join(struct.pack("<4f", 1, 2, 3, a) for a in [0, -100, 100])
        )
        samples, _, _ = ground._sample_world_positions(
            source, self.transform, ground.validate_options(self.options)
        )
        np.testing.assert_allclose(samples[:, :3], [[0, 400, 600]] * 3)
        np.testing.assert_allclose(samples[:, 3], [.5, 0, 1], atol=1e-10)

    def test_rejects_tilted_rotation(self) -> None:
        transform = dict(self.transform)
        transform["rotation"] = [5.0, 0.0, 0.0]
        with self.assertRaisesRegex(ground.GroundPlaneError, "horizontal XY plane"):
            ground.estimate(self.source, transform, self.options, [0.0, 0.0])


if __name__ == "__main__":
    unittest.main()
