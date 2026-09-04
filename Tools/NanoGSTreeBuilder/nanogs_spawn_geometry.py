"""NumPy-only, sampled geometry checks. No road/water semantic classification."""
from collections import Counter

import numpy as np


def select_flat_spawn(samples, candidates, lo, hi, global_z, options):
    """Return a diagnostic on failure; never silently choose an unsafe fallback."""
    cfg = options["flat_spawn"]
    radius = cfg["patch_radius_cm"]
    alpha = samples[:, 3] if samples.shape[1] > 3 else np.ones(len(samples))
    points = samples[alpha >= options["spawn_min_ground_alpha"], :3]
    points = points[np.argsort(points[:, 0])]

    def box(low, high):
        start, end = np.searchsorted(points[:, 0], [low[0], high[0]])
        p = points[start:end]
        return p[((p >= low) & (p <= high)).all(axis=1)]

    span = hi - lo
    center = np.average(candidates[:, :2], axis=0, weights=candidates[:, 3])
    edges = np.min(np.minimum((candidates[:, :2] - lo) / span,
                              (hi - candidates[:, :2]) / span), axis=1)
    eligible = ((edges >= options["edge_margin_fraction"])
                & (abs(candidates[:, 2] - global_z) <= options["spawn_max_ground_delta_cm"]))
    rejected = Counter()
    accepted = []
    offset = np.asarray(cfg["camera_offset_cm"])
    for i in np.flatnonzero(eligible):
        xy = candidates[i, :2]
        nearby = box([*(xy-radius), global_z-200], [*(xy+radius), global_z+1000])
        patch = nearby[(np.linalg.norm(nearby[:, :2]-xy, axis=1) <= radius)
                       & (abs(nearby[:, 2]-candidates[i, 2]) <= 60)]
        if len(patch) < 24:
            rejected["insufficient_ground_samples"] += 1
            continue
        design = np.column_stack(((patch[:, :2]-xy)/radius, np.ones(len(patch))))
        keep = np.ones(len(patch), dtype=bool)
        for _ in range(4):
            fit = np.linalg.lstsq(design[keep], patch[keep, 2], rcond=None)[0]
            residual = abs(design @ fit - patch[:, 2])
            keep = residual <= max(4, float(np.quantile(residual, .8)))
        slope = float(np.linalg.norm(fit[:2])/radius)
        roughness = float(np.quantile(residual, .8))
        if slope > cfg["max_slope"] or roughness > cfg["max_roughness_cm"]:
            rejected["slope_or_roughness"] += 1
            continue
        # Coverage around the center rejects narrow wall-foot lines and gaps.
        tiles = np.floor((patch[keep, :2]-xy)/radius*1.5 + 1.5).astype(int)
        tiles = np.clip(tiles, 0, 2)
        counts = np.bincount(tiles[:, 0]+3*tiles[:, 1], minlength=9)
        if np.count_nonzero(counts >= 2) < 7 or counts[4] < 3:
            rejected["ground_not_continuous"] += 1
            continue
        ground_z = float(fit[2])
        relative_z = nearby[:, 2] - (np.column_stack(((nearby[:, :2]-xy)/radius,
                                                     np.ones(len(nearby)))) @ fit)
        body = nearby[(np.linalg.norm(nearby[:, :2]-xy, axis=1) < cfg["body_radius_cm"])
                      & (relative_z > 25) & (relative_z < cfg["spawn_height_cm"]+250)]
        if len(np.unique(np.floor(body/25), axis=0)) >= 3:
            rejected["body_obstacle"] += 1
            continue
        target = np.array([*xy, ground_z + cfg["spawn_height_cm"]])
        camera = target + offset
        corridor = cfg["view_clearance_cm"]
        p = box(np.minimum(camera, target)-corridor, np.maximum(camera, target)+corridor)
        direction = target-camera
        distance2 = float(direction @ direction)
        t = np.clip((p-camera) @ direction / max(distance2, 1), 0, 1)
        d = np.linalg.norm(p-(camera+t[:, None]*direction), axis=1)
        occluders = p[(d < corridor) & (p[:, 2] > ground_z+30)]
        if len(np.unique(np.floor(occluders/50), axis=0)) >= 3:
            rejected["camera_or_sight_obstacle"] += 1
            continue
        center_distance = float(np.linalg.norm((xy-center)/span))
        # All accepted patches pass the same hard geometry gates. Then prefer
        # smoother/flatter, contiguous ground near the reconstructed center.
        score = roughness / cfg["max_roughness_cm"] + slope/cfg["max_slope"] + center_distance
        accepted.append(dict(xy_cm=xy.tolist(), ground_z_cm=ground_z,
                             slope=slope, roughness_p80_cm=roughness,
                             covered_tiles=int(np.count_nonzero(counts >= 2)),
                             ground_samples=len(patch), score=score,
                             edge_fraction=float(edges[i]), camera_cm=camera.tolist()))
    accepted.sort(key=lambda item: item["score"])
    return dict(valid=bool(accepted), eligible_count=int(eligible.sum()),
                accepted_count=len(accepted), rejected=dict(rejected),
                selected=accepted[0] if accepted else None, shortlist=accepted[:10],
                semantic_road_detection=False, sampled_geometry_only=True)
