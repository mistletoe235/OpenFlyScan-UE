#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
UE_ROOT="${OPENFLYSPLAT_UE_ROOT:-}"
DEFAULT_OUTPUT="${PROJECT_ROOT}/Artifacts/OpenFlySplatUE-Linux"

usage() {
  cat <<EOF
Usage: $(basename "$0") [OUTPUT_DIRECTORY]

Build a data-free Linux package and install the end-user PLY converter beside
the executable. The output directory must be empty. No PLY or generated Tree is
copied into the package.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then usage; exit 0; fi
[[ $# -le 1 ]] || { usage >&2; exit 2; }
[[ -n "${UE_ROOT}" ]] || { echo "Set OPENFLYSPLAT_UE_ROOT." >&2; exit 2; }
RUN_UAT="${UE_ROOT}/Engine/Build/BatchFiles/RunUAT.sh"
[[ -x "${RUN_UAT}" ]] || { echo "RunUAT.sh not found: ${RUN_UAT}" >&2; exit 2; }
[[ -x "${PROJECT_ROOT}/Tools/NanoGSTreeBuilder/build/build-lod" ]] || {
  echo "Run Scripts/bootstrap_spark_builder.sh before packaging." >&2
  exit 2
}

OUTPUT="$(realpath -m -- "${1:-${DEFAULT_OUTPUT}}")"
if [[ -d "${OUTPUT}" ]] && find "${OUTPUT}" -mindepth 1 -print -quit | grep -q .; then
  echo "Output directory must be empty: ${OUTPUT}" >&2
  exit 2
fi
mkdir -p -- "${OUTPUT}"

export NANOGS_RUNTIME_SCENE_ONLY=1
"${RUN_UAT}" BuildCookRun \
  -project="${PROJECT_ROOT}/OpenFlySplatUE.uproject" \
  -noP4 -unattended -utf8output \
  -platform=Linux -target=OpenFlySplatUE -clientconfig=Development \
  -build -cook -stage -pak -iostore -compressed -archive -nodebuginfo \
  -nocompileeditor -map=/Game/NanoGS/Template/NanoGS_Runtime \
  -archivedirectory="${OUTPUT}" \
  -ubtargs="-NoUBTMakefiles -NoHotReloadFromIDE"

mapfile -t CONTENT_DIRS < <(find "${OUTPUT}" -type d -path '*/OpenFlySplatUE/Content')
[[ ${#CONTENT_DIRS[@]} -eq 1 ]] || {
  echo "Expected one packaged OpenFlySplatUE/Content directory; found ${#CONTENT_DIRS[@]}." >&2
  exit 2
}
RUNTIME_ROOT="$(dirname -- "${CONTENT_DIRS[0]}")"
"${PROJECT_ROOT}/Scripts/install_converter_into_package.sh" "${RUNTIME_ROOT}"

MANIFEST="${RUNTIME_ROOT}/SHA256SUMS"
(cd -- "${RUNTIME_ROOT}" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum) >"${MANIFEST}"
echo "OPENFLYSPLAT_PACKAGE_OK root=${RUNTIME_ROOT} manifest=${MANIFEST}"
