#!/usr/bin/env python3
"""One-shot PLY -> persistent Spark cache -> staged NanoGS Tree v2 package."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Optional, Sequence

import build_nanogs_tree_v2 as tree_builder
import build_nanogs_tree_v3 as tree_v3_builder
import nanogs_ply_crop as ply_crop
import nanogs_spark_cache as spark_cache

REPO = Path(__file__).resolve().parents[2]
DESCRIPTOR_SCHEMA = "nanogs.tree_source.v1"
VERIFICATION_SCHEMA = "nanogs.tree_verified.v1"


class PrepareError(RuntimeError):
    pass


def _is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def write_descriptor(output_dir: Path, project_dir: Path, tree_manifest: dict[str, object]) -> Path:
    content_root = (project_dir / "Content" / "NanoGSData").resolve()
    resolved_output = output_dir.resolve()
    if not _is_relative_to(resolved_output, content_root):
        raise PrepareError("Tree v2 runtime output must be below Content/NanoGSData")
    relative = resolved_output.relative_to(project_dir.resolve()).as_posix()
    descriptor = {
        "schema": DESCRIPTOR_SCHEMA,
        "version": 1,
        "project_relative_directory": relative,
        "tree_key": tree_manifest["tree_key"],
        "source_sha256": tree_manifest["source"]["sha256"],
        "tree_sha256": tree_manifest["tree"]["sha256"],
        "page_sha256": tree_manifest["pages"]["sha256"],
    }
    target = resolved_output / "tree.ngstree"
    partial = Path(str(target) + ".partial")
    partial.write_text(json.dumps(descriptor, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(partial, target)
    return target


def _file_identity(path: Path) -> dict[str, object]:
    stat = path.stat()
    return {"bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns}


def load_verification_stamp(output_dir: Path, manifest: dict[str, object]) -> Optional[dict[str, object]]:
    stamp_path = output_dir / ".nanogs_verified.json"
    try:
        stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
        if stamp.get("schema") != VERIFICATION_SCHEMA or stamp.get("tree_key") != manifest["tree_key"]:
            return None
        for section in ("tree", "pages"):
            path = output_dir / str(manifest[section]["file"])
            if stamp["files"][section] != _file_identity(path):
                return None
        verification = stamp.get("verification")
        return verification if isinstance(verification, dict) else None
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
        return None


def write_verification_stamp(output_dir: Path, manifest: dict[str, object], verification: dict[str, object]) -> None:
    stamp = {
        "schema": VERIFICATION_SCHEMA,
        "tree_key": manifest["tree_key"],
        "files": {
            section: _file_identity(output_dir / str(manifest[section]["file"]))
            for section in ("tree", "pages")
        },
        "verification": verification,
    }
    target = output_dir / ".nanogs_verified.json"
    partial = Path(str(target) + ".partial")
    partial.write_text(json.dumps(stamp, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(partial, target)


def prepare(
    source: Path,
    project_dir: Path,
    builder: Path,
    spark_root: Path,
    tree_root: Path,
    page_points: int,
    force_spark: bool = False,
    force_tree: bool = False,
    crop_config: Optional[Path] = None,
    auto_crop: bool = False,
    crop_root: Optional[Path] = None,
    crop_tail_quantile: float = 0.001,
    crop_z_tail_quantile: float = 0.0,
    crop_protect_half: str = "none",
    force_crop: bool = False,
) -> dict[str, object]:
    original_source = source.resolve()
    source = original_source
    project_dir = project_dir.resolve()
    if not source.is_file():
        raise PrepareError(f"source PLY does not exist: {source}")
    if source.suffix.lower() != ".ply":
        raise PrepareError("source must be a .ply file")
    if page_points <= 0:
        raise PrepareError("page-points must be positive")
    if crop_config is not None and auto_crop:
        raise PrepareError("crop-config and auto-crop are mutually exclusive")
    crop_result: Optional[dict[str, object]] = None
    if crop_config is not None or auto_crop:
        print("[NanoGS 1/5] Preparing cropped PLY...", file=sys.stderr, flush=True)
        config = ply_crop.load_config(crop_config.resolve()) if crop_config else ply_crop.estimate(
            source, tail=crop_tail_quantile, protect=crop_protect_half,
            z_tail=crop_z_tail_quantile
        )
        crop_result = ply_crop.build(
            source,
            crop_root or project_dir / "Saved" / "NanoGSCache" / "Cropped",
            config,
            force=force_crop,
        )
        source = Path(str(crop_result["output"])).resolve()
        print(
            f"[NanoGS 1/5] Crop {'cache hit' if crop_result['cache_hit'] else 'built'}: {source}",
            file=sys.stderr, flush=True,
        )
    else:
        print("[NanoGS 1/5] Crop disabled; preserving all splats.", file=sys.stderr, flush=True)
    options = spark_cache.BuildOptions(
        method="quality", encoding="gsplat", max_sh=3,
        chunked=True, skip_validate=False,
    )
    print("[NanoGS 2/5] Preparing Spark spatial cache...", file=sys.stderr, flush=True)
    spark_result = spark_cache.build(
        source, builder.resolve(), spark_root.resolve(), options, force=force_spark
    )
    print(
        f"[NanoGS 2/5] Spark cache {'hit' if spark_result['cache_hit'] else 'built'}.",
        file=sys.stderr, flush=True,
    )
    print("[NanoGS 3/5] Preparing NanoGS TreeV2 pages...", file=sys.stderr, flush=True)
    tree_result = tree_builder.build_tree(
        source, Path(str(spark_result["cache_dir"])), tree_root.resolve(),
        page_points=page_points, force=force_tree,
    )
    print(
        f"[NanoGS 3/5] TreeV2 {'cache hit' if tree_result['cache_hit'] else 'built'}.",
        file=sys.stderr, flush=True,
    )
    output_dir = Path(str(tree_result["output_dir"])).resolve()
    print("[NanoGS 4/5] Verifying runtime tree...", file=sys.stderr, flush=True)
    verified = load_verification_stamp(output_dir, tree_result["manifest"])
    if verified is None:
        try:
            verified = tree_builder.verify_tree(output_dir)
        except (
            tree_builder.TreeBuildError,
            tree_builder.rad.RadError,
            spark_cache.CacheError,
            tree_builder.ng.FormatError,
            tree_builder.ng.VerificationError,
            OSError,
            ValueError,
            json.JSONDecodeError,
        ):
            if not tree_result["cache_hit"]:
                raise
            shutil.rmtree(output_dir)
            tree_result = tree_builder.build_tree(
                source, Path(str(spark_result["cache_dir"])), tree_root.resolve(),
                page_points=page_points, force=True,
            )
            output_dir = Path(str(tree_result["output_dir"])).resolve()
            verified = tree_builder.verify_tree(output_dir)
        write_verification_stamp(output_dir, tree_result["manifest"], verified)
    print("[NanoGS 4/5] Runtime tree verified.", file=sys.stderr, flush=True)
    print("[NanoGS 5/5] Preparing and verifying TreeV3 blocks...", file=sys.stderr, flush=True)
    tree_v3_result = tree_v3_builder.build_v3(
        output_dir, max_block_leaves=4096, force=force_tree
    )
    try:
        tree_v3_verified = tree_v3_builder.verify_v3(output_dir)
    except (
        tree_v3_builder.TreeV3Error,
        tree_builder.TreeBuildError,
        OSError,
        ValueError,
        KeyError,
        json.JSONDecodeError,
    ):
        if not tree_v3_result["cache_hit"]:
            raise
        tree_v3_result = tree_v3_builder.build_v3(
            output_dir, max_block_leaves=4096, force=True
        )
        tree_v3_verified = tree_v3_builder.verify_v3(output_dir)
    print(
        f"[NanoGS 5/5] TreeV3 {'cache hit' if tree_v3_result['cache_hit'] else 'built'} and verified.",
        file=sys.stderr, flush=True,
    )
    descriptor = write_descriptor(output_dir, project_dir, tree_result["manifest"])
    return {
        "source": str(original_source),
        "effective_source": str(source),
        "crop": crop_result,
        "spark_cache_hit": bool(spark_result["cache_hit"]),
        "tree_cache_hit": bool(tree_result["cache_hit"]),
        "tree_v3_cache_hit": bool(tree_v3_result["cache_hit"]),
        "spark_cache_dir": str(spark_result["cache_dir"]),
        "output_dir": str(output_dir),
        "descriptor": str(descriptor),
        "verification": verified,
        "tree_v3_verification": tree_v3_verified,
    }


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Prepare one PLY as an importable NanoGS Tree v2/v3 package")
    parser.add_argument("source", type=Path)
    parser.add_argument("--project-dir", type=Path, default=REPO)
    parser.add_argument("--builder", type=Path, default=Path(os.environ.get("NANOGS_SPARK_BUILDER", spark_cache.DEFAULT_BUILDER)))
    parser.add_argument("--spark-cache-root", type=Path)
    parser.add_argument("--tree-output-root", type=Path)
    parser.add_argument("--page-points", type=int, default=65536)
    parser.add_argument("--force-spark", action="store_true")
    parser.add_argument("--force-tree", action="store_true")
    crop_group = parser.add_mutually_exclusive_group()
    crop_group.add_argument("--crop-config", type=Path)
    crop_group.add_argument("--auto-crop", action="store_true")
    parser.add_argument("--crop-cache-root", type=Path)
    parser.add_argument("--crop-tail-quantile", type=float, default=0.001)
    parser.add_argument("--crop-z-tail-quantile", type=float, default=0.0)
    parser.add_argument("--crop-protect-half", choices=("none", "negative-q", "positive-q"), default="none")
    parser.add_argument("--force-crop", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    project_dir = args.project_dir.resolve()
    spark_root = args.spark_cache_root or project_dir / "Saved" / "NanoGSCache" / "Spark"
    tree_root = args.tree_output_root or project_dir / "Content" / "NanoGSData" / "TreeV2"
    try:
        result = prepare(
            args.source, project_dir, args.builder, spark_root, tree_root,
            args.page_points, args.force_spark, args.force_tree,
            args.crop_config, args.auto_crop, args.crop_cache_root,
            args.crop_tail_quantile, args.crop_z_tail_quantile,
            args.crop_protect_half, args.force_crop,
        )
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0
    except (PrepareError, ply_crop.CropError, spark_cache.CacheError, tree_builder.TreeBuildError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
