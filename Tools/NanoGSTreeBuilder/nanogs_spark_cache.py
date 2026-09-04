#!/usr/bin/env python3
"""Build and reuse Spark Bhatt-LoD caches from one Gaussian PLY.

This tool deliberately keeps Spark preprocessing separate from the NanoGS
runtime format. It provides the reproducible, persistent first stage:

    PLY -> Spark build-lod -> RAD header + RADC chunks + cache manifest

The cache key includes the complete source PLY, the exact builder executable,
and every output-affecting option. A later Tree v2 converter can therefore
consume immutable RAD/RADC directories without guessing how they were built.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Sequence


CACHE_SCHEMA = "nanogs.spark_lod_cache"
CACHE_VERSION = 2
TOOL_VERSION = "1.1.0"
DEFAULT_BUILDER = Path(__file__).resolve().parent / "build" / "build-lod"
BLOCK_BYTES = 8 * 1024 * 1024


class CacheError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class BuildOptions:
    method: str = "quality"
    encoding: str = "gsplat"
    max_sh: int = 3
    chunked: bool = True
    skip_validate: bool = False
    source_id_sidecar: bool = True
    inflate: bool = False

    def validate(self) -> None:
        if self.method not in ("quality", "quick"):
            raise CacheError(f"unsupported method: {self.method}")
        if self.encoding not in ("gsplat", "csplat"):
            raise CacheError(f"unsupported encoding: {self.encoding}")
        if self.max_sh not in (0, 1, 2, 3):
            raise CacheError("max_sh must be between 0 and 3")

    def key_dict(self) -> dict[str, object]:
        return dataclasses.asdict(self)

    def command_args(self) -> list[str]:
        args = [f"--{self.encoding}", f"--{self.method}", f"--max-sh={self.max_sh}"]
        args.append("--rad-chunked" if self.chunked else "--rad")
        if self.skip_validate:
            args.append("--skip-validate")
        if self.source_id_sidecar:
            if self.encoding != "gsplat":
                raise CacheError("source ID sidecar requires gsplat encoding")
            args.append("--source-id-sidecar")
        if self.inflate:
            args.append("--inflate")
        return args


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            block = handle.read(BLOCK_BYTES)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def canonical_json(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("utf-8")


def cache_key(source_sha256: str, builder_sha256: str, options: BuildOptions) -> str:
    description = {
        "schema": CACHE_SCHEMA,
        "version": CACHE_VERSION,
        "tool_version": TOOL_VERSION,
        "source_sha256": source_sha256,
        "builder_sha256": builder_sha256,
        "options": options.key_dict(),
    }
    return hashlib.sha256(canonical_json(description)).hexdigest()


def safe_stem(path: Path) -> str:
    stem = re.sub(r"[^A-Za-z0-9._-]+", "_", path.stem).strip("._-")
    return stem or "splat"


def output_names(source_name: str) -> tuple[str, str, str]:
    base = Path(source_name).stem + "-lod"
    return base + ".rad", base + "-*.radc", base + ".source_ids.bin"


def parse_builder_log(text: str) -> dict[str, object]:
    patterns = {
        "input_splats": r"Read:\s*num_splats:\s*(\d+)",
        "output_splats": r"Output set:\s*(\d+)\s*/",
        "lod_growth_factor": r"LoD growth factor:\s*([0-9.eE+-]+)",
        "root_children": r"Root #children:\s*(\d+)",
    }
    result: dict[str, object] = {}
    for name, pattern in patterns.items():
        match = re.search(pattern, text)
        if match:
            result[name] = float(match.group(1)) if name == "lod_growth_factor" else int(match.group(1))
    return result


def validate_manifest(cache_dir: Path, expected_key: str | None = None, verify_hashes: bool = False) -> dict[str, object]:
    manifest_path = cache_dir / "manifest.json"
    if not manifest_path.is_file():
        raise CacheError(f"missing cache manifest: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != CACHE_SCHEMA or manifest.get("version") != CACHE_VERSION:
        raise CacheError("unsupported cache manifest schema/version")
    if expected_key is not None and manifest.get("cache_key") != expected_key:
        raise CacheError("cache key does not match manifest")
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise CacheError("cache manifest has no files")
    for record in files:
        if not isinstance(record, dict) or not isinstance(record.get("name"), str):
            raise CacheError("invalid cache file record")
        path = cache_dir / record["name"]
        if not path.is_file() or path.stat().st_size != record.get("bytes"):
            raise CacheError(f"missing or size-mismatched cache file: {path}")
        if verify_hashes and sha256_file(path) != record.get("sha256"):
            raise CacheError(f"SHA256 mismatch: {path}")
    return manifest


def plan(source: Path, builder: Path, cache_root: Path, options: BuildOptions) -> dict[str, object]:
    source = source.resolve()
    builder = builder.resolve()
    options.validate()
    if not source.is_file():
        raise CacheError(f"source PLY does not exist: {source}")
    if source.suffix.lower() != ".ply":
        raise CacheError("source must be a .ply file")
    if not builder.is_file() or not os.access(builder, os.X_OK):
        raise CacheError(f"Spark build-lod is not executable: {builder}")
    source_hash = sha256_file(source)
    builder_hash = sha256_file(builder)
    key = cache_key(source_hash, builder_hash, options)
    cache_root = cache_root.resolve()
    preferred_cache_dir = cache_root / safe_stem(source) / key
    cache_dir = preferred_cache_dir
    hit = False
    candidates = [preferred_cache_dir]
    if cache_root.is_dir():
        candidates.extend(sorted(
            candidate for candidate in cache_root.glob(f"*/{key}")
            if candidate != preferred_cache_dir
        ))
    for candidate in candidates:
        if not candidate.exists():
            continue
        try:
            validate_manifest(candidate, key)
            cache_dir = candidate
            hit = True
            break
        except (CacheError, OSError, ValueError, json.JSONDecodeError):
            continue
    return {
        "schema": CACHE_SCHEMA,
        "version": CACHE_VERSION,
        "tool_version": TOOL_VERSION,
        "source": str(source),
        "source_bytes": source.stat().st_size,
        "source_sha256": source_hash,
        "builder": str(builder),
        "builder_sha256": builder_hash,
        "options": options.key_dict(),
        "cache_key": key,
        "cache_dir": str(cache_dir),
        "cache_hit": hit,
    }


def _file_record(path: Path) -> dict[str, object]:
    return {"name": path.name, "bytes": path.stat().st_size, "sha256": sha256_file(path)}


def build(source: Path, builder: Path, cache_root: Path, options: BuildOptions, force: bool = False) -> dict[str, object]:
    build_plan = plan(source, builder, cache_root, options)
    cache_dir = Path(str(build_plan["cache_dir"]))
    if build_plan["cache_hit"] and not force:
        manifest = validate_manifest(cache_dir, str(build_plan["cache_key"]))
        return {"cache_hit": True, "cache_dir": str(cache_dir), "manifest": manifest}

    cache_dir.parent.mkdir(parents=True, exist_ok=True)
    lock_path = cache_dir.parent / f".{cache_dir.name}.lock"
    lock_fd: int | None = None
    try:
        try:
            lock_fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            os.write(lock_fd, f"pid={os.getpid()} time={time.time()}\n".encode("ascii"))
        except FileExistsError as exc:
            raise CacheError(f"cache build is already locked: {lock_path}") from exc

        with tempfile.TemporaryDirectory(prefix=f".{cache_dir.name}.stage-", dir=cache_dir.parent) as temp_name:
            stage = Path(temp_name)
            source_link = stage / Path(str(build_plan["source"])).name
            source_link.symlink_to(Path(str(build_plan["source"])))
            command = [str(Path(str(build_plan["builder"]))), *options.command_args(), source_link.name]
            started = time.time()
            completed = subprocess.run(
                command,
                cwd=stage,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            duration = time.time() - started
            (stage / "build.log").write_text(completed.stdout, encoding="utf-8")
            if completed.returncode != 0:
                raise CacheError(f"Spark build-lod failed with exit code {completed.returncode}")

            rad_name, radc_pattern, source_ids_name = output_names(source_link.name)
            rad_path = stage / rad_name
            radc_paths = sorted(stage.glob(radc_pattern))
            source_ids_path = stage / source_ids_name
            if not rad_path.is_file():
                tail = "\n".join(completed.stdout.strip().splitlines()[-12:])
                if "invalid splats" in completed.stdout.lower():
                    raise CacheError(
                        "Spark builder rejected non-finite splats; repair NaN/Inf rotation, scale, opacity, or SH values "
                        f"before import. Builder output tail:\n{tail}"
                    )
                raise CacheError(f"Spark builder did not produce {rad_name}. Builder output tail:\n{tail}")
            if options.chunked and not radc_paths:
                raise CacheError("Spark builder produced no RADC chunks")
            if options.source_id_sidecar and not source_ids_path.is_file():
                raise CacheError("Spark builder produced no source ID sidecar")

            source_link.unlink()
            output_paths = [rad_path, *radc_paths]
            if options.source_id_sidecar:
                output_paths.append(source_ids_path)
            output_paths.append(stage / "build.log")
            manifest = {
                **build_plan,
                "cache_hit": False,
                "created_unix_seconds": time.time(),
                "build_duration_seconds": duration,
                "command": [Path(command[0]).name, *command[1:]],
                "builder_summary": parse_builder_log(completed.stdout),
                "rad_header": rad_path.name,
                "rad_chunks": len(radc_paths),
                "source_id_sidecar": source_ids_path.name if options.source_id_sidecar else None,
                "files": [_file_record(path) for path in output_paths],
            }
            (stage / "manifest.json").write_text(
                json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )

            if cache_dir.exists():
                if not force:
                    try:
                        existing = validate_manifest(cache_dir, str(build_plan["cache_key"]))
                        return {"cache_hit": True, "cache_dir": str(cache_dir), "manifest": existing}
                    except (CacheError, OSError, ValueError, json.JSONDecodeError):
                        pass
                shutil.rmtree(cache_dir)
            os.replace(stage, cache_dir)
        verified = validate_manifest(cache_dir, str(build_plan["cache_key"]), verify_hashes=True)
        return {"cache_hit": False, "cache_dir": str(cache_dir), "manifest": verified}
    finally:
        if lock_fd is not None:
            os.close(lock_fd)
        try:
            lock_path.unlink()
        except FileNotFoundError:
            pass


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Persistent Spark Bhatt-LoD cache builder for NanoGS")
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("plan", "build"):
        item = subparsers.add_parser(name)
        item.add_argument("source", type=Path)
        item.add_argument("--cache-root", type=Path, required=True)
        item.add_argument(
            "--builder",
            type=Path,
            default=Path(os.environ.get("NANOGS_SPARK_BUILDER", DEFAULT_BUILDER)),
        )
        item.add_argument("--method", choices=("quality", "quick"), default="quality")
        item.add_argument("--encoding", choices=("gsplat", "csplat"), default="gsplat")
        item.add_argument("--max-sh", type=int, choices=range(4), default=3)
        item.add_argument("--single-rad", action="store_true")
        item.add_argument("--skip-validate", action="store_true")
        if name == "build":
            item.add_argument("--force", action="store_true")
    verify = subparsers.add_parser("verify")
    verify.add_argument("cache_dir", type=Path)
    verify.add_argument("--hashes", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    try:
        if args.command == "verify":
            result = validate_manifest(args.cache_dir.resolve(), verify_hashes=args.hashes)
        else:
            options = BuildOptions(
                method=args.method,
                encoding=args.encoding,
                max_sh=args.max_sh,
                chunked=not args.single_rad,
                skip_validate=args.skip_validate,
            )
            if args.command == "plan":
                result = plan(args.source, args.builder, args.cache_root, options)
            else:
                result = build(args.source, args.builder, args.cache_root, options, force=args.force)
        json.dump(result, sys.stdout, ensure_ascii=False, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0
    except (CacheError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
