#!/usr/bin/env bash
set -euo pipefail

RUNTIME_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SETTINGS="${OPENFLY_HIL_SETTINGS:-${RUNTIME_ROOT}/NanoGSConverter/Config/OpenFlyHil.json}"
RUNTIME_UI="${OPENFLY_RUNTIME_UI:-HIL}"

usage() {
  cat <<EOF
Usage: $0 [--ui HIL|All|NanoGS|None] [--settings FILE] [-- UE_OPTIONS...]

Start the packaged OpenFly HIL simulator with deterministic runtime UI.

Options:
  --ui MODE       Floating windows to show (default: HIL).
                  HIL=OpenFly monitor only; All=both; NanoGS=render controls;
                  None=no runtime floating windows.
  --settings FILE Use another AirSim/OpenFly settings JSON.
  -h, --help      Show this help without starting Unreal Engine.
  --              Pass all remaining arguments directly to Unreal Engine.

Environment overrides:
  OPENFLY_RUNTIME_UI, OPENFLY_HIL_SETTINGS
EOF
}

EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --ui)
      [[ $# -ge 2 ]] || { echo "--ui requires a mode" >&2; exit 2; }
      RUNTIME_UI="$2"
      shift 2
      ;;
    --settings)
      [[ $# -ge 2 ]] || { echo "--settings requires a JSON path" >&2; exit 2; }
      SETTINGS="$(realpath -- "$2")"
      shift 2
      ;;
    --)
      shift
      EXTRA_ARGS+=("$@")
      break
      ;;
    *)
      EXTRA_ARGS+=("$1")
      shift
      ;;
  esac
done

case "${RUNTIME_UI,,}" in
  hil) RUNTIME_UI="HIL" ;;
  all) RUNTIME_UI="All" ;;
  nanogs) RUNTIME_UI="NanoGS" ;;
  none) RUNTIME_UI="None" ;;
  *) echo "invalid --ui mode: ${RUNTIME_UI}" >&2; exit 2 ;;
esac
[[ -f "${SETTINGS}" ]] || { echo "settings JSON not found: ${SETTINGS}" >&2; exit 2; }

exec "${RUNTIME_ROOT}/Binaries/Linux/OpenFlySplatUE" OpenFlySplatUE \
  -settings="${SETTINGS}" \
  -RuntimeUI="${RUNTIME_UI}" \
  -windowed -ResX=1920 -ResY=1080 -WinX=320 -WinY=180 \
  -ExecCmds="r.ScreenPercentage 100,r.VSync 1" \
  -log "${EXTRA_ARGS[@]}"
