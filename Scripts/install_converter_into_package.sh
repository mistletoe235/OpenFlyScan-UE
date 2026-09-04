#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDER="${PROJECT_ROOT}/Tools/NanoGSTreeBuilder/build/build-lod"
[[ $# -eq 1 ]] || { echo "Usage: $0 PACKAGED_OPENFLYSPLATUE_ROOT" >&2; exit 2; }
RUNTIME_ROOT="$(realpath -m -- "$1")"
[[ -d "${RUNTIME_ROOT}/Content" ]] || { echo "Runtime root must contain Content/: ${RUNTIME_ROOT}" >&2; exit 2; }
[[ -x "${BUILDER}" ]] || {
  echo "Spark builder is missing. Run Scripts/bootstrap_spark_builder.sh first." >&2
  exit 2
}

BUNDLE="${RUNTIME_ROOT}/NanoGSConverter"
mkdir -p -- "${BUNDLE}/Tools/NanoGSTreeBuilder/build" \
  "${BUNDLE}/Tools/NanoGSPageBuilder" "${BUNDLE}/Config/NanoGS" \
  "${RUNTIME_ROOT}/LICENSES"
for module in \
  build_nanogs_tree_v2.py \
  build_nanogs_tree_v3.py \
  nanogs_ground_plane.py \
  nanogs_ply_crop.py \
  nanogs_spark_cache.py \
  nanogs_spawn_geometry.py \
  prepare_active_scene.py \
  prepare_nanogs_tree.py \
  prepare_runtime_scene.py \
  spark_rad_reader.py; do
  cp -a -- "${PROJECT_ROOT}/Tools/NanoGSTreeBuilder/${module}" \
    "${BUNDLE}/Tools/NanoGSTreeBuilder/${module}"
done
cp -a -- "${BUILDER}" "${BUNDLE}/Tools/NanoGSTreeBuilder/build/build-lod"
cp -a -- "${PROJECT_ROOT}/Tools/NanoGSPageBuilder/nanogs_page_builder.py" \
  "${BUNDLE}/Tools/NanoGSPageBuilder/nanogs_page_builder.py"
cp -a -- "${PROJECT_ROOT}/Config/NanoGS/active_scene.json" "${BUNDLE}/Config/NanoGS/active_scene.json"
cp -a -- "${PROJECT_ROOT}/Tests/DjiHil/OpenFlyHil.example.json" "${BUNDLE}/Config/OpenFlyHil.json"
cp -a -- "${PROJECT_ROOT}/Tools/NanoGSPackageConverter/"*.sh "${RUNTIME_ROOT}/"
cp -a -- "${PROJECT_ROOT}/LICENSE" "${PROJECT_ROOT}/THIRD_PARTY_NOTICES.md" "${RUNTIME_ROOT}/"
cp -a -- "${PROJECT_ROOT}/LICENSES/." "${RUNTIME_ROOT}/LICENSES/"
chmod 0755 "${RUNTIME_ROOT}/"*.sh "${BUNDLE}/Tools/NanoGSTreeBuilder/build/build-lod"

echo "NANOGS_CONVERTER_OK root=${RUNTIME_ROOT}"
