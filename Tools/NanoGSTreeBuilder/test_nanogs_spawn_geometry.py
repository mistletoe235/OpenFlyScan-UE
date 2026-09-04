import unittest
import numpy as np

import nanogs_ground_plane as ground
from nanogs_spawn_geometry import select_flat_spawn


class FlatSpawnTest(unittest.TestCase):
    def setUp(self):
        self.options = ground.validate_options({"flat_spawn": {"enabled": True}})
        xy = np.stack(np.meshgrid(np.arange(-200, 201, 20), np.arange(-200, 201, 20)), axis=-1).reshape(-1, 2)
        self.points = np.column_stack((xy, np.zeros(len(xy)), np.ones(len(xy))))
        self.candidates = np.array([[0., 0., 0., len(xy), 0, 0, len(xy), 1]])

    def select(self, points):
        return select_flat_spawn(points, self.candidates, np.array([-1000., -1000.]),
                                 np.array([1000., 1000.]), 0, self.options)

    def test_continuous_flat_patch(self):
        result = self.select(self.points)
        self.assertTrue(result["valid"])
        self.assertAlmostEqual(result["selected"]["ground_z_cm"], 0)
        self.assertEqual(result["selected"]["covered_tiles"], 9)

    def test_stair_or_rough_ground_rejected(self):
        points = self.points.copy()
        points[:, 2] = np.where(points[:, 0] > 0, 40, -40)
        self.assertFalse(self.select(points)["valid"])

    def test_body_obstacles_and_camera_wall_rejected(self):
        for obstacle in [np.array([[x, 0, 90, 1] for x in [-70, 0, 70]]),
                         np.array([[-600, 150+y, 450, 1] for y in [-70, 0, 70]])]:
            with self.subTest(obstacle=obstacle.tolist()):
                result = self.select(np.vstack((self.points, obstacle)))
                self.assertFalse(result["valid"])
                self.assertIsNone(result["selected"])

    def test_narrow_strip_not_a_continuous_patch(self):
        points = self.points[abs(self.points[:, 1]) <= 20]
        self.assertFalse(self.select(points)["valid"])


if __name__ == "__main__":
    unittest.main()
