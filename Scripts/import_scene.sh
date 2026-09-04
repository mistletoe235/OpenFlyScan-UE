#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
UE_ROOT="${OPENFLYSPLAT_UE_ROOT:-}"
BUILDER="${PROJECT_ROOT}/Tools/NanoGSTreeBuilder/build/build-lod"

usage() {
  cat <<EOF
Usage: $(basename "$0") MODEL.ply [--prepare-only] [--force-spark] [--force-tree] [--force-crop]

Build the scalable NanoGS Tree cache and bind it to the shipped runtime map.
For direct, in-memory import of a smaller PLY, use Content Browser > Import in UE.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then usage; exit 0; fi
[[ $# -ge 1 ]] || { usage >&2; exit 2; }
SOURCE="$(realpath -- "$1")"
shift
[[ -f "${SOURCE}" && "${SOURCE,,}" == *.ply ]] || { echo "Not a PLY file: ${SOURCE}" >&2; exit 2; }
[[ -x "${BUILDER}" ]] || {
  echo "Spark builder is missing. Run Scripts/bootstrap_spark_builder.sh first." >&2
  exit 2
}

PREPARE_ONLY=0
PREPARE_ARGS=()
while (($#)); do
  case "$1" in
    --prepare-only) PREPARE_ONLY=1 ;;
    --force-spark|--force-tree|--force-crop) PREPARE_ARGS+=("$1") ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

python3 -c 'import numpy' >/dev/null 2>&1 || {
  echo "Python 3 with NumPy is required." >&2
  exit 2
}
python3 "${PROJECT_ROOT}/Tools/NanoGSTreeBuilder/prepare_active_scene.py" \
  --project-dir "${PROJECT_ROOT}" --source "${SOURCE}" "${PREPARE_ARGS[@]}"

((PREPARE_ONLY)) && exit 0
[[ -n "${UE_ROOT}" ]] || {
  echo "Set OPENFLYSPLAT_UE_ROOT to finish the editor import." >&2
  exit 2
}
EDITOR_CMD="${UE_ROOT}/Engine/Binaries/Linux/UnrealEditor-Cmd"
[[ -x "${EDITOR_CMD}" ]] || { echo "UnrealEditor-Cmd not found: ${EDITOR_CMD}" >&2; exit 2; }
if pgrep -f "UnrealEditor.*OpenFlySplatUE.uproject" >/dev/null; then
  echo "Close the OpenFlySplatUE editor before binding the generated Tree." >&2
  exit 3
fi

exec "${EDITOR_CMD}" "${PROJECT_ROOT}/OpenFlySplatUE.uproject" \
  -run=pythonscript -script="${PROJECT_ROOT}/Scripts/configure_nanogs_active_scene.py" \
  -unattended -nop4 -nosplash -NullRHI
