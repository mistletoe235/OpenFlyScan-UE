#!/usr/bin/env python3
"""Prepare the replaceable project PLY and publish its UE import state."""

from __future__ import annotations

import argparse
import copy
import json
import os
import re
import sys
from pathlib import Path
from typing import Any, Optional, Sequence

import nanogs_ground_plane as ground_plane
import prepare_nanogs_tree as tree_prepare

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO / "Config" / "NanoGS" / "active_scene.json"
DEFAULT_STATE = REPO / "Saved" / "NanoGSActive" / "current_scene.json"
CONFIG_SCHEMA = "nanogs.active_scene.v1"
STATE_SCHEMA = "nanogs.active_scene.state.v1"
ASSET_PATH_RE = re.compile(r"^/Game(?:/[A-Za-z0-9_]+)+$")


class ActiveSceneError(RuntimeError):
    pass


def _resolve_path(value: str, project_dir: Path) -> Path:
    path = Path(value).expanduser()
    return path.resolve() if path.is_absolute() else (project_dir / path).resolve()


def _require_object(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ActiveSceneError(f"{name} must be a JSON object")
    return value


def _require_vector(value: Any, name: str) -> list[float]:
    if not isinstance(value, list) or len(value) != 3:
        raise ActiveSceneError(f"{name} must contain exactly three numbers")
    try:
        return [float(component) for component in value]
    except (TypeError, ValueError) as exc:
        raise ActiveSceneError(f"{name} must contain exactly three numbers") from exc


def _require_asset_path(value: Any, name: str) -> str:
    if not isinstance(value, str) or not ASSET_PATH_RE.fullmatch(value):
        raise ActiveSceneError(f"{name} must be a /Game package path")
    return value


def load_config(config_path: Path, project_dir: Path, source_override: Optional[Path] = None) -> dict[str, Any]:
    config_path = config_path.resolve()
    project_dir = project_dir.resolve()
    try:
        raw = json.loads(config_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ActiveSceneError(f"missing config: {config_path}") from exc
    except json.JSONDecodeError as exc:
        raise ActiveSceneError(f"invalid JSON in {config_path}: {exc}") from exc
    config = _require_object(raw, "config")
    if config.get("schema") != CONFIG_SCHEMA or config.get("version") != 1:
        raise ActiveSceneError(f"unsupported active-scene config schema: {config.get('schema')!r}")

    source_value = source_override if source_override is not None else config.get("source_ply")
    if not isinstance(source_value, (str, os.PathLike)):
        raise ActiveSceneError("source_ply must be a path")
    source = _resolve_path(os.fspath(source_value), project_dir)
    if source.suffix.lower() != ".ply":
        raise ActiveSceneError(f"source_ply must name a .ply file: {source}")

    try:
        page_points = int(config.get("page_points", 65536))
    except (TypeError, ValueError) as exc:
        raise ActiveSceneError("page_points must be an integer") from exc
    if page_points <= 0:
        raise ActiveSceneError("page_points must be positive")

    crop = _require_object(config.get("crop", {}), "crop")
    crop_mode = crop.get("mode", "none")
    if crop_mode not in {"none", "auto", "config"}:
        raise ActiveSceneError("crop.mode must be one of: none, auto, config")
    crop_config = crop.get("config")
    if crop_mode == "config" and not isinstance(crop_config, str):
        raise ActiveSceneError("crop.config is required when crop.mode is config")
    if crop_mode != "config" and crop_config is not None:
        raise ActiveSceneError("crop.config is only valid when crop.mode is config")
    protect_half = crop.get("protect_half", "none")
    if protect_half not in {"none", "negative-q", "positive-q"}:
        raise ActiveSceneError("crop.protect_half is invalid")

    unreal_config = _require_object(config.get("unreal"), "unreal")
    source_asset_path = _require_asset_path(unreal_config.get("source_asset_path"), "unreal.source_asset_path")
    target_level_path = _require_asset_path(unreal_config.get("target_level_path"), "unreal.target_level_path")
    actor_label = unreal_config.get("actor_label")
    if not isinstance(actor_label, str) or not actor_label.strip():
        raise ActiveSceneError("unreal.actor_label must be non-empty")

    transform = _require_object(unreal_config.get("transform", {}), "unreal.transform")
    component = _require_object(unreal_config.get("component", {}), "unreal.component")
    ground = _require_object(unreal_config.get("ground", {}), "unreal.ground")
    ground_mode = ground.get("mode", "manual")
    if ground_mode not in {"manual", "auto"}:
        raise ActiveSceneError("unreal.ground.mode must be manual or auto")
    orientation_mode = ground.get("orientation_mode", "configured")
    if orientation_mode not in {"configured", "auto_vertical"}:
        raise ActiveSceneError(
            "unreal.ground.orientation_mode must be configured or auto_vertical"
        )
    player_start = _require_object(unreal_config.get("player_start", {}), "unreal.player_start")
    player_xy_mode = player_start.get("xy_mode", "configured")
    if player_xy_mode not in {"configured", "scene_center", "auto_open"}:
        raise ActiveSceneError(
            "unreal.player_start.xy_mode must be configured, scene_center, or auto_open"
        )
    collision_floor = _require_object(unreal_config.get("collision_floor", {}), "unreal.collision_floor")

    normalized = {
        "schema": CONFIG_SCHEMA,
        "version": 1,
        "config_path": str(config_path),
        "project_dir": str(project_dir),
        "source_ply": str(source),
        "page_points": page_points,
        "crop": {
            "mode": crop_mode,
            "config": str(_resolve_path(crop_config, project_dir)) if crop_config else None,
            "tail_quantile": float(crop.get("tail_quantile", 0.001)),
            "z_tail_quantile": float(crop.get("z_tail_quantile", 0.0)),
            "protect_half": protect_half,
        },
        "unreal": {
            "source_asset_path": source_asset_path,
            "target_level_path": target_level_path,
            "actor_label": actor_label.strip(),
            "transform": {
                "location": _require_vector(transform.get("location", [0, 0, 0]), "unreal.transform.location"),
                "rotation": _require_vector(transform.get("rotation", [0, 0, 0]), "unreal.transform.rotation"),
                "scale": _require_vector(transform.get("scale", [-1, 1, 1]), "unreal.transform.scale"),
            },
            "component": {
                "sh_order": int(component.get("sh_order", 3)),
                "opacity_scale": float(component.get("opacity_scale", 1.0)),
                "splat_scale": float(component.get("splat_scale", 1.0)),
                "tree_lod_splat_budget": int(component.get("tree_lod_splat_budget", 8_000_000)),
                "tree_lod_progressive_splat_budget": int(
                    component.get("tree_lod_progressive_splat_budget", 4_000_000)
                ),
                "tree_lod_detail_scale": float(component.get("tree_lod_detail_scale", 1.0)),
                "force_full_detail_lod0": bool(component.get("force_full_detail_lod0", False)),
            },
            "ground": {
                "flat_spawn": ground_plane.validate_options(ground)["flat_spawn"],
                "mode": ground_mode,
                "orientation_mode": orientation_mode,
                "sample_points": int(ground.get("sample_points", 500_000)),
                "chunk_points": int(ground.get("chunk_points", 262_144)),
                "grid_size": int(ground.get("grid_size", 48)),
                "minimum_cell_points": int(ground.get("minimum_cell_points", 12)),
                "cell_low_quantile": float(ground.get("cell_low_quantile", 0.10)),
                "bounds_tail_quantile": float(ground.get("bounds_tail_quantile", 0.001)),
                "analysis_tail_quantile": float(ground.get("analysis_tail_quantile", 0.02)),
                "mode_bin_cm": float(ground.get("mode_bin_cm", 50.0)),
                "anchor_max_delta_cm": float(ground.get("anchor_max_delta_cm", 300.0)),
                "clearance_height_cm": float(ground.get("clearance_height_cm", 300.0)),
                "edge_margin_fraction": float(ground.get("edge_margin_fraction", 0.15)),
                "center_bias": float(ground.get("center_bias", 0.15)),
                "spawn_support_quantile": float(ground.get("spawn_support_quantile", 0.75)),
                "spawn_max_ground_delta_cm": float(ground.get("spawn_max_ground_delta_cm", 100.0)),
                "spawn_min_ground_alpha": float(ground.get("spawn_min_ground_alpha", 0.5)),
                "clearance_radius_cm": float(ground.get("clearance_radius_cm", 0.0)),
                "observer_offset_xy_cm": _require_vector(
                    [*ground.get("observer_offset_xy_cm", [0.0, 0.0]), 0.0],
                    "unreal.ground.observer_offset_xy_cm",
                )[:2],
            },
            "player_start": {
                "enabled": bool(player_start.get("enabled", True)),
                "label": str(player_start.get("label", "NanoGS_Active_PlayerStart")),
                "location": _require_vector(player_start.get("location", [0, 0, 50]), "unreal.player_start.location"),
                "rotation": _require_vector(player_start.get("rotation", [0, 0, 0]), "unreal.player_start.rotation"),
                "xy_mode": player_xy_mode,
                "height_above_ground_cm": float(player_start.get("height_above_ground_cm", 50.0)),
            },
            "collision_floor": {
                "enabled": bool(collision_floor.get("enabled", True)),
                "label": str(collision_floor.get("label", "NanoGS_Active_CollisionFloor")),
                "location": _require_vector(collision_floor.get("location", [0, 0, -50]), "unreal.collision_floor.location"),
                "scale": _require_vector(collision_floor.get("scale", [2000, 2000, 1]), "unreal.collision_floor.scale"),
                "hidden_in_game": bool(collision_floor.get("hidden_in_game", True)),
                "hidden_in_editor": bool(collision_floor.get("hidden_in_editor", True)),
                "margin": float(collision_floor.get("margin", 1.10)),
                "thickness_cm": float(collision_floor.get("thickness_cm", 20.0)),
                "below_ground_cm": float(collision_floor.get("below_ground_cm", 2.0)),
            },
        },
    }
    if normalized["unreal"]["component"]["sh_order"] not in {0, 1, 2, 3}:
        raise ActiveSceneError("unreal.component.sh_order must be between 0 and 3")
    if not 100_000 <= normalized["unreal"]["component"]["tree_lod_splat_budget"] <= 30_000_000:
        raise ActiveSceneError("unreal.component.tree_lod_splat_budget must be between 100000 and 30000000")
    if not 0 <= normalized["unreal"]["component"]["tree_lod_progressive_splat_budget"] <= 30_000_000:
        raise ActiveSceneError(
            "unreal.component.tree_lod_progressive_splat_budget must be between 0 and 30000000"
        )
    if not 0.1 <= normalized["unreal"]["component"]["tree_lod_detail_scale"] <= 4.0:
        raise ActiveSceneError("unreal.component.tree_lod_detail_scale must be between 0.1 and 4.0")
    if normalized["unreal"]["player_start"]["height_above_ground_cm"] < 0.0:
        raise ActiveSceneError("unreal.player_start.height_above_ground_cm must be non-negative")
    if normalized["unreal"]["collision_floor"]["margin"] < 1.0:
        raise ActiveSceneError("unreal.collision_floor.margin must be at least 1.0")
    if normalized["unreal"]["collision_floor"]["thickness_cm"] <= 0.0:
        raise ActiveSceneError("unreal.collision_floor.thickness_cm must be positive")
    ground_plane.validate_options(normalized["unreal"]["ground"])
    return normalized


def _project_relative(path: Path, project_dir: Path) -> str:
    try:
        return path.resolve().relative_to(project_dir.resolve()).as_posix()
    except ValueError as exc:
        raise ActiveSceneError(f"runtime file must be inside project: {path}") from exc


def resolve_unreal_settings(
    project_dir: Path, config: dict[str, Any], result: dict[str, Any], descriptor_data: dict[str, Any]
) -> tuple[dict[str, Any], Optional[dict[str, Any]]]:
    settings = copy.deepcopy(config["unreal"])
    if settings["ground"]["mode"] != "auto":
        return settings, None
    player = settings["player_start"]
    settings["ground"].setdefault("flat_spawn", {})["spawn_height_cm"] = float(player["height_above_ground_cm"])
    configured_xy = [float(player["location"][0]), float(player["location"][1])]
    source = Path(str(result["effective_source"]))
    source_hash = str(descriptor_data["source_sha256"])
    cache_root = project_dir / "Saved" / "NanoGSCache" / "Ground"
    if settings["ground"]["orientation_mode"] == "auto_vertical":
        configured_rotation = list(settings["transform"]["rotation"])
        if any(abs(float(value)) > 1e-6 for value in configured_rotation):
            raise ActiveSceneError(
                "auto_vertical orientation requires unreal.transform.rotation=[0,0,0]"
            )
        candidates = []
        rejected_candidates = {}
        for name, rotation in (("configured", [0.0, 0.0, 0.0]), ("roll_180", [0.0, 0.0, 180.0])):
            candidate_transform = copy.deepcopy(settings["transform"])
            candidate_transform["rotation"] = rotation
            try:
                candidate_estimate = ground_plane.estimate_cached(
                    source, source_hash, candidate_transform, settings["ground"],
                    configured_xy, cache_root,
                )
            except ground_plane.GroundPlaneError as exc:
                rejected_candidates[name] = str(exc)
                continue
            candidates.append((
                float(candidate_estimate.get("orientation_score", candidate_estimate["recommended_player_score"])),
                name,
                candidate_transform,
                candidate_estimate,
            ))
        if not candidates:
            raise ActiveSceneError(f"no orientation has a supported ground/spawn candidate: {rejected_candidates}")
        score, orientation, chosen_transform, estimate = min(candidates, key=lambda item: item[0])
        settings["transform"] = chosen_transform
        estimate["orientation_selection"] = {
            "mode": "auto_vertical",
            "selected": orientation,
            "selected_score": score,
            "candidates": {name: candidate_score for candidate_score, name, _, _ in candidates},
            "rejected_candidates": rejected_candidates,
        }
    else:
        estimate = ground_plane.estimate_cached(
            source, source_hash, settings["transform"], settings["ground"],
            configured_xy, cache_root,
        )
    bounds_min = estimate["bounds_min_cm"]
    bounds_max = estimate["bounds_max_cm"]
    if player["xy_mode"] == "auto_open":
        geometry = estimate.get("spawn_geometry")
        if geometry and not geometry["valid"]:
            raise ActiveSceneError(
                "no flat, continuous ground patch with body/camera clearance in the chosen orientation; "
                f"check physical scale or explicitly choose a manual spawn. Diagnostic: {geometry}"
            )
        player_xy = list(estimate["recommended_player_xy_cm"])
        ground_z = float(estimate["recommended_player_ground_z_cm"])
    elif player["xy_mode"] == "scene_center":
        player_xy = [
            0.5 * (bounds_min[0] + bounds_max[0]),
            0.5 * (bounds_min[1] + bounds_max[1]),
        ]
        if player_xy != configured_xy:
            estimate = ground_plane.estimate_cached(
                Path(str(result["effective_source"])),
                str(descriptor_data["source_sha256"]),
                settings["transform"],
                settings["ground"],
                player_xy,
                project_dir / "Saved" / "NanoGSCache" / "Ground",
            )
        ground_z = float(estimate["anchor_ground_z_cm"])
    else:
        player_xy = configured_xy
        ground_z = float(estimate["anchor_ground_z_cm"])
    if player["enabled"]:
        player["location"] = [
            player_xy[0],
            player_xy[1],
            ground_z + float(player["height_above_ground_cm"]),
        ]
    floor = settings["collision_floor"]
    if floor["enabled"]:
        thickness = float(floor["thickness_cm"])
        margin = float(floor["margin"] )
        width = max(100.0, (bounds_max[0] - bounds_min[0]) * margin)
        depth = max(100.0, (bounds_max[1] - bounds_min[1]) * margin)
        floor_top = ground_z - float(floor["below_ground_cm"])
        floor["location"] = [
            0.5 * (bounds_min[0] + bounds_max[0]),
            0.5 * (bounds_min[1] + bounds_max[1]),
            floor_top - 0.5 * thickness,
        ]
        floor["scale"] = [width / 100.0, depth / 100.0, thickness / 100.0]
    estimate["resolved_player_location_cm"] = list(player["location"])
    estimate["resolved_floor_location_cm"] = list(floor["location"])
    estimate["resolved_floor_scale"] = list(floor["scale"])
    return settings, estimate


def write_state(state_path: Path, project_dir: Path, config: dict[str, Any], result: dict[str, Any]) -> dict[str, Any]:
    descriptor = Path(str(result["descriptor"])).resolve()
    output_dir = Path(str(result["output_dir"])).resolve()
    descriptor_data = json.loads(descriptor.read_text(encoding="utf-8"))
    unreal_settings, ground_estimate = resolve_unreal_settings(project_dir, config, result, descriptor_data)
    state = {
        "schema": STATE_SCHEMA,
        "version": 1,
        "source_ply": config["source_ply"],
        "effective_source": result["effective_source"],
        "crop": result.get("crop"),
        "descriptor": _project_relative(descriptor, project_dir),
        "tree_directory": _project_relative(output_dir, project_dir),
        "tree_key": descriptor_data["tree_key"],
        "source_sha256": descriptor_data["source_sha256"],
        "spark_cache_hit": bool(result["spark_cache_hit"]),
        "tree_cache_hit": bool(result["tree_cache_hit"]),
        "tree_v3_cache_hit": bool(result["tree_v3_cache_hit"]),
        "verification": result["verification"],
        "tree_v3_verification": result["tree_v3_verification"],
        "ground_estimate": ground_estimate,
        "unreal": unreal_settings,
    }
    state_path.parent.mkdir(parents=True, exist_ok=True)
    partial = Path(str(state_path) + ".partial")
    partial.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(partial, state_path)
    return state


def prepare_active_scene(
    config_path: Path,
    project_dir: Path,
    state_path: Path,
    source_override: Optional[Path] = None,
    force_spark: bool = False,
    force_tree: bool = False,
    force_crop: bool = False,
) -> dict[str, Any]:
    config = load_config(config_path, project_dir, source_override)
    source = Path(config["source_ply"])
    if not source.is_file():
        raise ActiveSceneError(
            f"active PLY is missing: {source}\n"
            "Replace Data/NanoGS/Active/scene.ply or pass --source; the previous UE asset was not changed."
        )
    crop = config["crop"]
    result = tree_prepare.prepare(
        source=source,
        project_dir=project_dir,
        builder=Path(os.environ.get("NANOGS_SPARK_BUILDER", tree_prepare.spark_cache.DEFAULT_BUILDER)),
        spark_root=project_dir / "Saved" / "NanoGSCache" / "Spark",
        tree_root=project_dir / "Content" / "NanoGSData" / "TreeV2" / "Active",
        page_points=config["page_points"],
        force_spark=force_spark,
        force_tree=force_tree,
        crop_config=Path(crop["config"]) if crop["mode"] == "config" else None,
        auto_crop=crop["mode"] == "auto",
        crop_root=project_dir / "Saved" / "NanoGSCache" / "Cropped",
        crop_tail_quantile=crop["tail_quantile"],
        crop_protect_half=crop["protect_half"],
        force_crop=force_crop,
    )
    return write_state(state_path, project_dir, config, result)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Prepare the replaceable NanoGS active-scene PLY")
    parser.add_argument("--project-dir", type=Path, default=REPO)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--state", type=Path, default=DEFAULT_STATE)
    parser.add_argument("--source", type=Path, help="one-run PLY override; does not rewrite the config")
    parser.add_argument("--force-spark", action="store_true")
    parser.add_argument("--force-tree", action="store_true")
    parser.add_argument("--force-crop", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    project_dir = args.project_dir.resolve()
    config_path = args.config if args.config.is_absolute() else project_dir / args.config
    state_path = args.state if args.state.is_absolute() else project_dir / args.state
    try:
        state = prepare_active_scene(
            config_path, project_dir, state_path, args.source,
            args.force_spark, args.force_tree, args.force_crop,
        )
        json.dump(state, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0
    except (
        ActiveSceneError,
        ground_plane.GroundPlaneError,
        tree_prepare.PrepareError,
        tree_prepare.ply_crop.CropError,
        tree_prepare.spark_cache.CacheError,
        tree_prepare.tree_builder.TreeBuildError,
        tree_prepare.tree_v3_builder.TreeV3Error,
        OSError,
        ValueError,
    ) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
