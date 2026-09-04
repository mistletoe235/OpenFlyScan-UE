#!/usr/bin/env python3
"""Convert one PLY into a loose scene consumed by an already packaged NanoGS game."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Optional, Sequence

import prepare_active_scene as active_scene
import prepare_nanogs_tree as tree_prepare

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO / "Config" / "NanoGS" / "active_scene.json"
RUNTIME_SCHEMA = "nanogs.runtime_scene.v1"


class RuntimeSceneError(RuntimeError):
    pass


def _relative(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError as exc:
        raise RuntimeSceneError(f"runtime output escaped package root: {path}") from exc


def prepare_runtime_scene(
    source: Path,
    runtime_root: Path,
    config_path: Path = DEFAULT_CONFIG,
    crop_mode: Optional[str] = None,
    force_spark: bool = False,
    force_tree: bool = False,
    force_crop: bool = False,
    builder: Optional[Path] = None,
    world_scale: Optional[float] = None,
    spawn_height_m: Optional[float] = None,
    camera_settings: Optional[Path] = None,
) -> dict[str, Any]:
    source = source.resolve()
    runtime_root = runtime_root.resolve()
    if not source.is_file() or source.suffix.lower() != ".ply":
        raise RuntimeSceneError(f"source must be an existing .ply file: {source}")
    content_root = runtime_root / "Content" / "NanoGSData" / "RuntimeActive"
    content_root.mkdir(parents=True, exist_ok=True)

    config = active_scene.load_config(config_path.resolve(), REPO, source)
    flat = config["unreal"].get("ground", {}).get("flat_spawn", {})
    if flat.get("enabled"):
        profile = camera_settings or Path(os.environ.get("OPENFLY_HIL_SETTINGS",
            str(runtime_root / "NanoGSConverter/Config/OpenFlyHil.json")))
        if profile.is_file():
            data = json.loads(profile.read_text(encoding="utf-8"))
            director = data.get("CameraDirector", {})
            if data.get("ViewMode") != "FlyWithMe" or not all(k in director for k in ("X", "Y", "Z")):
                raise RuntimeSceneError("spawn camera check requires a FlyWithMe profile with CameraDirector X/Y/Z")
            flat["camera_offset_cm"] = [float(director["X"])*100,
                float(director["Y"])*100, -float(director["Z"])*100]
            flat["camera_profile"] = str(profile.resolve())
        elif camera_settings is not None or "OPENFLY_HIL_SETTINGS" in os.environ:
            raise RuntimeSceneError(f"camera settings file not found: {profile}")
    if world_scale is not None:
        if not 0.001 <= world_scale <= 1000.0:
            raise RuntimeSceneError("world scale must be between 0.001 and 1000")
        base_scale = config["unreal"]["transform"]["scale"]
        config["unreal"]["transform"]["scale"] = [
            float(value) * world_scale for value in base_scale
        ]
    if spawn_height_m is not None:
        if not 0.0 <= spawn_height_m <= 1000.0:
            raise RuntimeSceneError("spawn height must be between 0 and 1000 metres")
        config["unreal"]["player_start"]["height_above_ground_cm"] = (
            spawn_height_m * 100.0
        )
    crop = dict(config["crop"])
    if crop_mode is not None:
        if crop_mode not in {"none", "auto", "config"}:
            raise RuntimeSceneError("crop mode must be none, auto, or config")
        crop["mode"] = crop_mode
        if crop_mode != "config":
            crop["config"] = None
        elif not crop.get("config"):
            raise RuntimeSceneError("crop mode config requires crop.config in the JSON config")

    cache_root = runtime_root / "NanoGSConverterCache"
    result = tree_prepare.prepare(
        source=source,
        project_dir=runtime_root,
        builder=(builder or Path(os.environ.get(
            "NANOGS_SPARK_BUILDER", tree_prepare.spark_cache.DEFAULT_BUILDER
        ))).resolve(),
        spark_root=cache_root / "Spark",
        tree_root=content_root / "Trees",
        page_points=config["page_points"],
        force_spark=force_spark,
        force_tree=force_tree,
        crop_config=Path(crop["config"]) if crop["mode"] == "config" else None,
        auto_crop=crop["mode"] == "auto",
        crop_root=cache_root / "Cropped",
        crop_tail_quantile=crop["tail_quantile"],
        crop_z_tail_quantile=crop.get("z_tail_quantile", 0.0),
        crop_protect_half=crop["protect_half"],
        force_crop=force_crop,
    )

    descriptor = Path(str(result["descriptor"])).resolve()
    descriptor_data = json.loads(descriptor.read_text(encoding="utf-8"))
    unreal_settings, ground_estimate = active_scene.resolve_unreal_settings(
        runtime_root, config, result, descriptor_data
    )
    if ground_estimate is None:
        raise RuntimeSceneError("packaged runtime scenes require unreal.ground.mode=auto")

    print("[NanoGS] Ground, PlayerStart, and collision floor resolved.", file=sys.stderr, flush=True)
    runtime_scene = {
        "schema": RUNTIME_SCHEMA,
        "version": 1,
        "source_ply": str(source),
        "source_sha256": descriptor_data["source_sha256"],
        "tree_key": descriptor_data["tree_key"],
        "tree_directory": _relative(Path(str(result["output_dir"])), runtime_root),
        "crop": result.get("crop"),
        "ground_estimate": ground_estimate,
        "transform": unreal_settings["transform"],
        "component": unreal_settings["component"],
        "player_start": {
            "location": unreal_settings["player_start"]["location"],
            "rotation": unreal_settings["player_start"]["rotation"],
        },
        "collision_floor": {
            "location": unreal_settings["collision_floor"]["location"],
            "scale": unreal_settings["collision_floor"]["scale"],
        },
        "cache": {
            "spark_hit": bool(result["spark_cache_hit"]),
            "tree_hit": bool(result["tree_cache_hit"]),
            "tree_v3_hit": bool(result["tree_v3_cache_hit"]),
        },
    }
    output = content_root / "active_scene.runtime.json"
    partial = Path(str(output) + ".partial")
    partial.write_text(
        json.dumps(runtime_scene, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    os.replace(partial, output)
    print(f"[NanoGS] Active scene published atomically: {output}", file=sys.stderr, flush=True)
    runtime_scene["runtime_scene_file"] = str(output)
    return runtime_scene


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert a PLY for an already packaged NanoGS runtime"
    )
    parser.add_argument("source", type=Path)
    parser.add_argument("--runtime-root", type=Path, required=True,
                        help="packaged OpenFlySplatUE directory containing Content/")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--builder", type=Path)
    parser.add_argument("--camera-settings", type=Path,
                        help="FlyWithMe AirSim JSON used for spawn clearance; defaults to the bundled HIL profile")
    parser.add_argument("--crop-mode", choices=("none", "auto", "config"))
    parser.add_argument("--force-spark", action="store_true")
    parser.add_argument("--force-tree", action="store_true")
    parser.add_argument("--force-crop", action="store_true")
    parser.add_argument(
        "--world-scale", type=float,
        help="multiply the configured UE actor scale without rebuilding spatial caches",
    )
    parser.add_argument(
        "--spawn-height", type=float, metavar="METRES",
        help="place PlayerStart this many metres above the automatically detected ground",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    try:
        result = prepare_runtime_scene(
            args.source, args.runtime_root, args.config, args.crop_mode,
            args.force_spark, args.force_tree, args.force_crop, args.builder,
            args.world_scale, args.spawn_height, args.camera_settings,
        )
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0
    except (
        RuntimeSceneError,
        active_scene.ActiveSceneError,
        active_scene.ground_plane.GroundPlaneError,
        tree_prepare.PrepareError,
        tree_prepare.ply_crop.CropError,
        tree_prepare.spark_cache.CacheError,
        tree_prepare.tree_builder.TreeBuildError,
        tree_prepare.tree_v3_builder.TreeV3Error,
        OSError,
        ValueError,
        KeyError,
        json.JSONDecodeError,
    ) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
