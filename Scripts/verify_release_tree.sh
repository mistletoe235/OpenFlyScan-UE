#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "${PROJECT_ROOT}"

[[ -f OpenFlySplatUE.uproject ]]
[[ -f Content/NanoGS/Template/NanoGS_Runtime.umap ]]
[[ ! -e Content/Carla ]]
[[ ! -e Plugins/Carla ]]
[[ ! -e Plugins/AirSim/Content/VehicleAdv ]]
[[ ! -e Plugins/AirSim/Content/Weather ]]
[[ ! -e Plugins/AirSim/Content/HUDAssets/OptionsMenu.uasset ]]
rg -F 'Formats.Add(TEXT("ply;PLY Gaussian Splatting File"));' \
  Plugins/NanoGS/Source/NanoGSEditor/Private/GaussianSplatAssetFactory.cpp >/dev/null

find_release_files() {
  find . \
    \( -path './.git' -o -path './Binaries' -o -path './Intermediate' \
       -o -path './Saved' -o -path './DerivedDataCache' \
       -o -path './Plugins/NanoGS/Binaries' -o -path './Plugins/NanoGS/Intermediate' \
       -o -path './Plugins/AirSim/Binaries' -o -path './Plugins/AirSim/Intermediate' \
       -o -path './Tools/NanoGSTreeBuilder/build' \
       -o -path './Tools/NanoGSTreeBuilder/spark-src' -o -path './Artifacts' \) \
    -prune -o -type f -print
}

if find_release_files | rg -i '\.(ply|rad|radc|ngsp|ngst|ngstree)$' -m 1; then
  echo "Generated scene data is present in the release tree." >&2
  exit 1
fi
if find_release_files | while IFS= read -r file; do
  [[ "$(stat -c '%s' "${file}")" -le 104857600 ]] || { echo "${file}"; break; }
done | grep -q .; then
  echo "A file larger than 100 MiB is present in the release tree." >&2
  exit 1
fi
if rg -n "(/Game/Carla|Plugins/Carla|CarlaUnreal|CARLAGS_UE_ROOT)" \
  Config Content Plugins/NanoGS/Source Plugins/AirSim/Source Scripts Tools Tests \
  --glob '!**/Binaries/**' --glob '!**/Intermediate/**' \
  --glob '!verify_release_tree.sh'; then
  echo "A stale CARLA project dependency remains." >&2
  exit 1
fi

python3 -m json.tool OpenFlySplatUE.uproject >/dev/null
python3 -m json.tool Config/NanoGS/active_scene.json >/dev/null
python3 -m json.tool Tests/DjiHil/OpenFlyHil.example.json >/dev/null
echo "RELEASE_TREE_OK"
