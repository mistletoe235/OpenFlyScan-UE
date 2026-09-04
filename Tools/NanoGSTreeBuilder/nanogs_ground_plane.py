#!/usr/bin/env python3
"""Deterministic cached horizontal-ground estimation for replaceable NanoGS PLY scenes."""

from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
from typing import Any

import numpy as np

import nanogs_ply_crop as ply_crop

SCHEMA = "nanogs.ground_plane.v1"
VERSION = "1.9.0"


class GroundPlaneError(RuntimeError):
    pass


def _finite_number(value: Any, name: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise GroundPlaneError(f"{name} must be a finite number") from exc
    if not math.isfinite(result):
        raise GroundPlaneError(f"{name} must be a finite number")
    return result


def validate_options(options: dict[str, Any]) -> dict[str, Any]:
    try:
        sample_points = int(options.get("sample_points", 500_000))
        chunk_points = int(options.get("chunk_points", 262_144))
        grid_size = int(options.get("grid_size", 48))
        minimum_cell_points = int(options.get("minimum_cell_points", 12))
    except (TypeError, ValueError) as exc:
        raise GroundPlaneError("ground integer options are invalid") from exc
    low_quantile = _finite_number(options.get("cell_low_quantile", 0.10), "cell_low_quantile")
    bounds_tail = _finite_number(options.get("bounds_tail_quantile", 0.001), "bounds_tail_quantile")
    analysis_tail = _finite_number(
        options.get("analysis_tail_quantile", 0.02), "analysis_tail_quantile"
    )
    mode_bin_cm = _finite_number(options.get("mode_bin_cm", 50.0), "mode_bin_cm")
    anchor_max_delta_cm = _finite_number(
        options.get("anchor_max_delta_cm", 300.0), "anchor_max_delta_cm"
    )
    clearance_height_cm = _finite_number(
        options.get("clearance_height_cm", 300.0), "clearance_height_cm"
    )
    edge_margin_fraction = _finite_number(
        options.get("edge_margin_fraction", 0.15), "edge_margin_fraction"
    )
    center_bias = _finite_number(options.get("center_bias", 0.15), "center_bias")
    support_quantile = _finite_number(
        options.get("spawn_support_quantile", 0.75), "spawn_support_quantile"
    )
    spawn_ground_delta = _finite_number(
        options.get("spawn_max_ground_delta_cm", 100.0), "spawn_max_ground_delta_cm"
    )
    minimum_ground_alpha = _finite_number(
        options.get("spawn_min_ground_alpha", 0.5), "spawn_min_ground_alpha"
    )
    clearance_radius_cm = _finite_number(
        options.get("clearance_radius_cm", 0.0), "clearance_radius_cm"
    )
    try:
        observer_offset_xy_cm = np.asarray(
            options.get("observer_offset_xy_cm", [0.0, 0.0]), dtype=np.float64
        )
    except (TypeError, ValueError) as exc:
        raise GroundPlaneError("observer_offset_xy_cm must contain two finite numbers") from exc
    if sample_points < 1_000 or chunk_points < 1_000:
        raise GroundPlaneError("ground sample_points and chunk_points must be at least 1000")
    if not 8 <= grid_size <= 256 or minimum_cell_points < 3:
        raise GroundPlaneError("ground grid_size or minimum_cell_points is invalid")
    if not 0.0 < low_quantile < 0.5:
        raise GroundPlaneError("cell_low_quantile must be between 0 and 0.5")
    if not 0.0 <= bounds_tail < 0.1:
        raise GroundPlaneError("bounds_tail_quantile must be between 0 and 0.1")
    if not 0.0 <= analysis_tail < 0.1:
        raise GroundPlaneError("analysis_tail_quantile must be between 0 and 0.1")
    if mode_bin_cm <= 0.0 or anchor_max_delta_cm < 0.0 or clearance_height_cm <= 0.0:
        raise GroundPlaneError("ground height thresholds are invalid")
    if not 0.0 <= edge_margin_fraction < 0.5 or center_bias < 0.0:
        raise GroundPlaneError("ground edge_margin_fraction or center_bias is invalid")
    if clearance_radius_cm < 0.0:
        raise GroundPlaneError("clearance_radius_cm must be non-negative")
    if not 0.0 <= support_quantile < 1.0 or spawn_ground_delta < 0.0:
        raise GroundPlaneError("spawn support quantile or ground delta is invalid")
    if not 0.0 <= minimum_ground_alpha <= 1.0:
        raise GroundPlaneError("spawn_min_ground_alpha must be between 0 and 1")
    if observer_offset_xy_cm.shape != (2,) or not np.isfinite(observer_offset_xy_cm).all():
        raise GroundPlaneError("observer_offset_xy_cm must contain two finite numbers")
    flat = dict(options.get("flat_spawn", {}))
    flat_defaults = dict(enabled=False, patch_radius_cm=200.0, max_slope=0.10,
        max_roughness_cm=12.0, body_radius_cm=150.0, view_clearance_cm=120.0,
        spawn_height_cm=50.0, camera_offset_cm=[-1200.0, 300.0, 800.0])
    if not isinstance(flat.get("enabled", False), bool):
        raise GroundPlaneError("flat_spawn.enabled must be boolean")
    for key, default in flat_defaults.items():
        if key in {"enabled", "camera_offset_cm"}:
            continue
        flat[key] = _finite_number(flat.get(key, default), "flat_spawn." + key)
        if flat[key] < 0 or (key != "spawn_height_cm" and flat[key] == 0):
            raise GroundPlaneError("flat_spawn geometry limits must be positive")
    camera = np.asarray(flat.get("camera_offset_cm", flat_defaults["camera_offset_cm"]), dtype=float)
    if camera.shape != (3,) or not np.isfinite(camera).all() or np.linalg.norm(camera) < 1:
        raise GroundPlaneError("flat_spawn.camera_offset_cm must be a finite nonzero XYZ vector")
    flat["camera_offset_cm"] = camera.tolist()
    flat.setdefault("enabled", False)
    return {
        "flat_spawn": flat,
        "sample_points": sample_points,
        "chunk_points": chunk_points,
        "grid_size": grid_size,
        "minimum_cell_points": minimum_cell_points,
        "cell_low_quantile": low_quantile,
        "bounds_tail_quantile": bounds_tail,
        "analysis_tail_quantile": analysis_tail,
        "mode_bin_cm": mode_bin_cm,
        "anchor_max_delta_cm": anchor_max_delta_cm,
        "clearance_height_cm": clearance_height_cm,
        "edge_margin_fraction": edge_margin_fraction,
        "center_bias": center_bias,
        "spawn_support_quantile": support_quantile,
        "spawn_max_ground_delta_cm": spawn_ground_delta,
        "spawn_min_ground_alpha": minimum_ground_alpha,
        "clearance_radius_cm": clearance_radius_cm,
        "observer_offset_xy_cm": observer_offset_xy_cm.tolist(),
    }


def _ue_rotation_matrix(rotation: np.ndarray) -> np.ndarray:
    """Return Unreal's Pitch/Yaw/Roll matrix for row-vector point batches."""
    pitch, yaw, roll = np.radians(rotation)
    sp, cp = math.sin(pitch), math.cos(pitch)
    sy, cy = math.sin(yaw), math.cos(yaw)
    sr, cr = math.sin(roll), math.cos(roll)
    return np.asarray([
        [cp * cy, cp * sy, sp],
        [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp],
        [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp],
    ], dtype=np.float64)


def _validate_transform(transform: dict[str, Any]) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    try:
        location = np.asarray(transform["location"], dtype=np.float64)
        rotation = np.asarray(transform["rotation"], dtype=np.float64)
        scale = np.asarray(transform["scale"], dtype=np.float64)
    except (KeyError, TypeError, ValueError) as exc:
        raise GroundPlaneError("ground estimation requires location/rotation/scale vectors") from exc
    if location.shape != (3,) or rotation.shape != (3,) or scale.shape != (3,):
        raise GroundPlaneError("ground transform vectors must contain three values")
    if not np.isfinite(location).all() or not np.isfinite(rotation).all() or not np.isfinite(scale).all():
        raise GroundPlaneError("ground transform contains a non-finite value")
    rotation_matrix = _ue_rotation_matrix(rotation)
    # The estimator models a horizontal world-space floor. Yaw and a 180-degree
    # vertical flip preserve that contract; a tilted source does not.
    transformed_z = rotation_matrix[2]
    if np.linalg.norm(transformed_z[:2]) > 1e-6 or abs(abs(transformed_z[2]) - 1.0) > 1e-6:
        raise GroundPlaneError(
            "automatic ground estimation requires actor rotation that preserves a horizontal XY plane; "
            "bake tilted orientation into the PLY or use unreal.ground.mode=manual"
        )
    if abs(scale[0]) < 1e-9 or abs(scale[1]) < 1e-9 or scale[2] <= 0.0:
        raise GroundPlaneError("automatic ground estimation requires non-zero XY scale and positive Z scale")
    return location, scale, rotation_matrix


def _sample_world_positions(
    source: Path, transform: dict[str, Any], options: dict[str, Any]
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    layout = ply_crop.parse_ply(source)
    location, scale, rotation_matrix = _validate_transform(transform)
    stride = max(1, math.ceil(layout["count"] / options["sample_points"]))
    samples: list[np.ndarray] = []
    bounds_min = np.full(3, np.inf, dtype=np.float64)
    bounds_max = np.full(3, -np.inf, dtype=np.float64)
    with source.open("rb") as stream:
        for first, raw in ply_crop.chunks(stream, layout, options["chunk_points"]):
            positions = np.column_stack(
                [ply_crop.values(raw, layout, axis) for axis in "xyz"]
            ).astype(np.float64, copy=False)
            valid = np.isfinite(positions).all(axis=1)
            if not valid.any():
                continue
            world = (positions[valid] * (100.0 * scale)) @ rotation_matrix + location
            bounds_min = np.minimum(bounds_min, np.min(world, axis=0))
            bounds_max = np.maximum(bounds_max, np.max(world, axis=0))
            valid_indices = np.flatnonzero(valid)
            selected = valid_indices[((first + valid_indices) % stride) == 0]
            if selected.size:
                sample = (positions[selected] * (100.0 * scale)) @ rotation_matrix + location
                if "opacity" in layout["offsets"]:
                    logits = ply_crop.values(raw, layout, "opacity")[selected].astype(np.float64)
                    alpha = 1.0 / (1.0 + np.exp(-np.clip(logits, -80, 80)))
                    alpha[~np.isfinite(logits)] = 0.0
                    # Optional fourth lane: source alpha for ground support,
                    # independent of the user's runtime Cloud Opacity slider.
                    sample = np.column_stack((sample, alpha))
                samples.append(sample)
    if not samples or not np.isfinite(bounds_min).all() or not np.isfinite(bounds_max).all():
        raise GroundPlaneError("PLY has no finite XYZ samples")
    return np.concatenate(samples), bounds_min, bounds_max


def _cell_ground_candidates(
    samples: np.ndarray, options: dict[str, Any]
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    # Spawn analysis must not inherit the collision/render footprint: a sparse
    # reconstruction shell can dwarf the actual scene and make a fringe cell
    # appear central. This filters analysis samples only, never the source PLY.
    tail = options["analysis_tail_quantile"]
    robust_min = np.quantile(samples[:, :2], tail, axis=0)
    robust_max = np.quantile(samples[:, :2], 1.0 - tail, axis=0)
    span = robust_max - robust_min
    if np.any(span <= 1e-6):
        raise GroundPlaneError("PLY XY extent is too small for automatic ground estimation")
    inside = np.logical_and(samples[:, :2] >= robust_min, samples[:, :2] <= robust_max).all(axis=1)
    points = samples[inside]
    grid_size = options["grid_size"]
    cells = np.floor((points[:, :2] - robust_min) / span * grid_size).astype(np.int64)
    cells = np.clip(cells, 0, grid_size - 1)
    cell_ids = cells[:, 0] + grid_size * cells[:, 1]
    order = np.argsort(cell_ids, kind="stable")
    cell_ids = cell_ids[order]
    points = points[order]
    boundaries = np.concatenate(([0], np.flatnonzero(np.diff(cell_ids)) + 1, [len(cell_ids)]))
    candidates: list[list[float]] = []
    for start, end in zip(boundaries[:-1], boundaries[1:]):
        if end - start < options["minimum_cell_points"]:
            continue
        cell_points = points[start:end]
        ground_z = float(np.quantile(cell_points[:, 2], options["cell_low_quantile"]))
        ground_band = np.abs(cell_points[:, 2] - ground_z) <= options["mode_bin_cm"]
        ground_alpha = cell_points[ground_band, 3] if cell_points.shape[1] > 3 else np.ones(np.count_nonzero(ground_band))
        candidates.append([
            float(np.median(cell_points[:, 0])),
            float(np.median(cell_points[:, 1])),
            ground_z,
            float(end - start),
            float(np.mean(cell_points[:, 2] > ground_z + options["clearance_height_cm"])),
            float(np.quantile(cell_points[:, 2], 0.90) - ground_z),
            float(np.sum(ground_alpha)),
            float(np.mean(ground_alpha)) if len(ground_alpha) else 0.0,
        ])
    if len(candidates) < 16:
        raise GroundPlaneError(
            f"only {len(candidates)} populated XY cells; automatic ground estimation needs at least 16"
        )
    return np.asarray(candidates, dtype=np.float64), robust_min, robust_max


def _modal_ground_height(candidates: np.ndarray, options: dict[str, Any]) -> tuple[float, np.ndarray]:
    values = candidates[:, 2]
    lower = float(np.min(values))
    upper = float(np.max(values))
    if upper - lower <= options["mode_bin_cm"]:
        selected = np.ones(len(values), dtype=bool)
        return float(np.median(values)), selected
    bin_width = options["mode_bin_cm"]
    bin_count = max(1, min(512, int(math.ceil((upper - lower) / bin_width))))
    histogram, edges = np.histogram(values, bins=bin_count, range=(lower, upper))
    peak = int(np.argmax(histogram))
    peak_center = 0.5 * (edges[peak] + edges[peak + 1])
    selected = np.abs(values - peak_center) <= bin_width
    if np.count_nonzero(selected) < 8:
        selected = np.logical_and(values >= edges[peak], values <= edges[peak + 1])
    return float(np.median(values[selected])), selected


def _choose_open_anchor(
    candidates: np.ndarray,
    robust_min: np.ndarray,
    robust_max: np.ndarray,
    global_ground_z: float,
    options: dict[str, Any],
) -> tuple[np.ndarray, float, float, float]:
    span = robust_max - robust_min
    normalized_xy = (candidates[:, :2] - robust_min) / span
    edge_distance = np.min(
        np.column_stack((
            normalized_xy[:, 0], 1.0 - normalized_xy[:, 0],
            normalized_xy[:, 1], 1.0 - normalized_xy[:, 1],
        )),
        axis=1,
    )
    ground_match = np.abs(candidates[:, 2] - global_ground_z) <= options["anchor_max_delta_cm"]
    eligible = ground_match & (edge_distance >= options["edge_margin_fraction"])
    if not eligible.any():
        raise GroundPlaneError(
            "no main-ground cell satisfies edge_margin_fraction; "
            "reduce the margin deliberately or use unreal.player_start.xy_mode=manual"
        )
    # Dense foliage/roofs must not qualify just by total splat count. Compare
    # support in a narrow ground-height band, and avoid raised platforms far
    # above the main plane. This is a geometric confidence filter, not a water
    # classifier; no source points or obstacle neighbors are removed.
    eligible &= np.abs(candidates[:, 2] - global_ground_z) <= options["spawn_max_ground_delta_cm"]
    if candidates.shape[1] > 7:
        eligible &= candidates[:, 7] >= options["spawn_min_ground_alpha"]
    support = candidates[:, 6] if candidates.shape[1] > 6 else candidates[:, 3]
    if not eligible.any():
        raise GroundPlaneError(
            "no interior spawn candidate satisfies main-ground height and source-alpha confidence; "
            "review spawn_max_ground_delta_cm/spawn_min_ground_alpha or use a manual spawn"
        )
    minimum_support = np.quantile(support[eligible], options["spawn_support_quantile"])
    eligible &= support >= minimum_support
    # A bounding-box midpoint is easily pulled into an empty part of an
    # asymmetric scene by a long but valid tail. Use the sampled population
    # represented by each cell instead, so "central" means central to the
    # reconstruction while the edge guard still uses robust bounds.
    center = np.average(candidates[:, :2], axis=0, weights=candidates[:, 3])
    center_distance = np.linalg.norm((candidates[:, :2] - center) / span, axis=1)
    obstacle_height = np.maximum(0.0, candidates[:, 5]) / options["clearance_height_cm"]
    local_risk = candidates[:, 4] * 8.0 + np.minimum(obstacle_height, 10.0) * 0.25
    clearance_radius = options["clearance_radius_cm"]
    if clearance_radius > 0.0:
        observer_offset = np.asarray(options["observer_offset_xy_cm"], dtype=np.float64)
        neighborhood_risk = np.full(len(candidates), np.inf, dtype=np.float64)
        for index in np.flatnonzero(eligible):
            player_distance = np.linalg.norm(
                candidates[:, :2] - candidates[index, :2], axis=1
            )
            observer_xy = candidates[index, :2] + observer_offset
            observer_distance = np.linalg.norm(candidates[:, :2] - observer_xy, axis=1)
            player_cells = ground_match & (player_distance <= clearance_radius)
            observer_cells = ground_match & (observer_distance <= clearance_radius)
            if player_cells.any() and observer_cells.any():
                neighborhood_risk[index] = (
                    float(np.quantile(local_risk[player_cells], 0.90))
                    + float(np.quantile(local_risk[observer_cells], 0.90))
                )
        score = neighborhood_risk + center_distance * options["center_bias"]
    else:
        score = local_risk + center_distance * options["center_bias"]
    score[~eligible] = np.inf
    if not np.isfinite(score).any():
        raise GroundPlaneError(
            "no finite spawn candidate supports the player/observer clearance; "
            "check scene scale and observer_offset_xy_cm or use a manual spawn"
        )
    selected = int(np.argmin(score))
    return (
        candidates[selected, :2].copy(),
        float(candidates[selected, 2]),
        float(score[selected]),
        float(edge_distance[selected]),
    )


def estimate(source: Path, transform: dict[str, Any], options: dict[str, Any], anchor_xy: list[float]) -> dict[str, Any]:
    source = source.resolve()
    normalized = validate_options(options)
    samples, bounds_min, bounds_max = _sample_world_positions(source, transform, normalized)
    candidates, robust_min, robust_max = _cell_ground_candidates(samples, normalized)
    global_ground_z, modal_mask = _modal_ground_height(candidates, normalized)
    # Orientation is independent of the new spawn confidence policy. Changing
    # water/ground confidence must not flip a previously upright rendering.
    orientation_options = dict(normalized, spawn_support_quantile=0.0,
        spawn_min_ground_alpha=0.0,
        spawn_max_ground_delta_cm=normalized["anchor_max_delta_cm"])
    _, _, orientation_score, _ = _choose_open_anchor(
        candidates, robust_min, robust_max, global_ground_z, orientation_options
    )
    # Preserve orientation evidence even if the selected orientation has no safe
    # spawn: callers must not flip the scene to make a clearance check pass.
    spawn_geometry = None
    if normalized["flat_spawn"]["enabled"]:
        from nanogs_spawn_geometry import select_flat_spawn
        spawn_geometry = select_flat_spawn(samples, candidates, robust_min, robust_max,
                                          global_ground_z, normalized)
        selected = spawn_geometry["selected"]
        if selected:
            open_xy = np.asarray(selected["xy_cm"])
            open_ground_z, open_score = selected["ground_z_cm"], selected["score"]
            open_edge_fraction = selected["edge_fraction"]
        else:
            open_xy, open_ground_z, open_score, open_edge_fraction = _choose_open_anchor(
                candidates, robust_min, robust_max, global_ground_z, orientation_options)
    else:
        open_xy, open_ground_z, open_score, open_edge_fraction = _choose_open_anchor(
            candidates, robust_min, robust_max, global_ground_z, normalized
        )
    selected_index = int(np.argmin(np.sum((candidates[:, :2] - open_xy) ** 2, axis=1)))
    anchor = np.asarray(anchor_xy, dtype=np.float64)
    if anchor.shape != (2,) or not np.isfinite(anchor).all():
        raise GroundPlaneError("ground anchor_xy must contain two finite values")
    distances = np.sum((candidates[:, :2] - anchor) ** 2, axis=1)
    nearest_index = int(np.argmin(distances))
    nearest_z = float(candidates[nearest_index, 2])
    anchor_ground_z = nearest_z
    if abs(nearest_z - global_ground_z) > normalized["anchor_max_delta_cm"]:
        anchor_ground_z = global_ground_z
    modal_values = candidates[modal_mask, 2]
    coverage_tail = normalized["bounds_tail_quantile"]
    coverage_min = np.quantile(samples[:, :2], coverage_tail, axis=0)
    coverage_max = np.quantile(samples[:, :2], 1.0 - coverage_tail, axis=0)
    return {
        "schema": SCHEMA,
        "version": 1,
        "tool_version": VERSION,
        "source": str(source),
        "sample_count": int(len(samples)),
        "candidate_cell_count": int(len(candidates)),
        "modal_cell_count": int(np.count_nonzero(modal_mask)),
        # Collision coverage uses robust XY bounds, so a few remote splats do
        # not create an enormous physics body. Preserve exact extrema for audit.
        "bounds_min_cm": [float(coverage_min[0]), float(coverage_min[1]), float(bounds_min[2])],
        "bounds_max_cm": [float(coverage_max[0]), float(coverage_max[1]), float(bounds_max[2])],
        "analysis_bounds_xy_min_cm": robust_min.tolist(),
        "analysis_bounds_xy_max_cm": robust_max.tolist(),
        "raw_bounds_min_cm": bounds_min.tolist(),
        "raw_bounds_max_cm": bounds_max.tolist(),
        "global_ground_z_cm": global_ground_z,
        "anchor_xy_cm": anchor.tolist(),
        "anchor_ground_z_cm": anchor_ground_z,
        "nearest_candidate_distance_cm": float(math.sqrt(distances[nearest_index])),
        "nearest_candidate_z_cm": nearest_z,
        "recommended_player_xy_cm": open_xy.tolist(),
        "recommended_player_ground_z_cm": open_ground_z,
        "recommended_player_score": open_score,
        "orientation_score": orientation_score,
        "spawn_geometry": spawn_geometry,
        "recommended_player_support": {
            "cell_sample_count": int(candidates[selected_index, 3]),
            "ground_alpha_weighted_count": float(candidates[selected_index, 6]),
            "ground_mean_source_alpha": float(candidates[selected_index, 7]),
            "main_ground_delta_cm": float(abs(open_ground_z - global_ground_z)),
            "semantic_water_detection": False,
        },
        "recommended_player_edge_fraction": open_edge_fraction,
        "modal_mad_cm": float(np.median(np.abs(modal_values - np.median(modal_values)))),
        "plane_normal": [0.0, 0.0, 1.0],
        "options": normalized,
        "cache_hit": False,
    }


def estimate_cached(
    source: Path,
    source_sha256: str,
    transform: dict[str, Any],
    options: dict[str, Any],
    anchor_xy: list[float],
    cache_root: Path,
) -> dict[str, Any]:
    normalized = validate_options(options)
    key_data = {
        "schema": SCHEMA,
        "tool_version": VERSION,
        "source_sha256": source_sha256,
        "transform": transform,
        "options": normalized,
        "anchor_xy": anchor_xy,
    }
    key = hashlib.sha256(
        json.dumps(key_data, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    cache_path = cache_root.resolve() / key / "ground.json"
    if cache_path.is_file():
        cached = json.loads(cache_path.read_text(encoding="utf-8"))
        if cached.get("schema") == SCHEMA and cached.get("cache_key") == key:
            cached["cache_hit"] = True
            return cached
    result = estimate(source, transform, normalized, anchor_xy)
    result["source_sha256"] = source_sha256
    result["cache_key"] = key
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    partial = Path(str(cache_path) + ".partial")
    partial.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(partial, cache_path)
    return result
