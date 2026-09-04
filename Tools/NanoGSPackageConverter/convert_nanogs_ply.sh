#!/usr/bin/env bash
set -euo pipefail

RUNTIME_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONVERTER_ROOT="${RUNTIME_ROOT}/NanoGSConverter"

usage() {
  cat >&2 <<EOF
Usage: $0 MODEL.ply [OPTIONS]

Convert one Gaussian Splatting PLY into the cache used by this packaged UE app.
The command atomically updates the active scene only after every stage succeeds.

Options:
  --crop-mode none    Default/recommended for full fidelity: keep every splat.
  --crop-mode auto    Optionally remove sparse XY/Z tails.
  --crop-mode config  Use the crop config path embedded in active_scene.json.
  --force-crop        Rebuild the cropped PLY even when its cache is valid.
  --force-spark       Rebuild the Spark-compatible spatial cache.
  --force-tree        Rebuild the NanoGS runtime tree and pages.
  --world-scale N     Multiply the complete UE scene scale by N. This scales
                      positions and Gaussian sizes together without rebuilding
                      spatial caches. Use when the PLY coordinate unit is known;
                      it is deliberately not guessed automatically.
  --spawn-height M    Place PlayerStart/AirSim/HIL origin M metres above the
                      detected local ground. The hidden collision floor stays
                      on the ground. Default is 0.5 m in active_scene.json.
  --camera-settings FILE  Use this FlyWithMe JSON for camera clearance checks;
                      pass the same FILE to run_openfly_hil.sh --settings.
                      Default: OPENFLY_HIL_SETTINGS or bundled OpenFlyHil.json.
  -h, --help          Show this help without changing the active scene.

Generated automatically:
  cropped PLY, Spark cache, NanoGS tree/pages, ground estimate, hidden collision
  floor, PlayerStart, and the shared AirSim/HIL origin. The converter compares
  the configured vertical orientation with a 180-degree X-axis flip and records
  the lower-risk ground orientation; it does not rewrite the source PLY.
  Spawn prefers continuous flat ground and checks body/camera clearance using
  sampled geometry. This is not semantic road detection or a flight safety map.
  No qualifying patch: conversion fails without replacing the active scene.

Example:
  $0 /path/to/expo_west.ply --crop-mode none --spawn-height 0.5

The previous scene remains active if conversion fails. Restart the simulator
after a successful conversion; use ./run_openfly_hil.sh for HIL mode.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
[[ $# -ge 1 ]] || { usage; exit 2; }
[[ -f "$1" && "${1,,}" == *.ply ]] || {
  echo "MODEL must be an existing .ply file: $1" >&2
  exit 2
}
SOURCE="$(realpath -- "$1")"
shift

[[ -f "${CONVERTER_ROOT}/Tools/NanoGSTreeBuilder/prepare_runtime_scene.py" ]] || {
  echo "NanoGS converter bundle is incomplete: ${CONVERTER_ROOT}" >&2
  exit 2
}
python3 -c 'import numpy' >/dev/null 2>&1 || {
  echo "Python 3 with NumPy is required (for example: python3 -m pip install numpy)." >&2
  exit 2
}

python3 "${CONVERTER_ROOT}/Tools/NanoGSTreeBuilder/prepare_runtime_scene.py" \
  "${SOURCE}" \
  --runtime-root "${RUNTIME_ROOT}" \
  --config "${CONVERTER_ROOT}/Config/NanoGS/active_scene.json" \
  --builder "${CONVERTER_ROOT}/Tools/NanoGSTreeBuilder/build/build-lod" \
  "$@"

echo "Conversion complete. Restart this simulator package to load the new scene." >&2
