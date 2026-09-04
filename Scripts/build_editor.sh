#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
UE_ROOT="${OPENFLYSPLAT_UE_ROOT:-}"

if [[ -z "${UE_ROOT}" ]]; then
  echo "Set OPENFLYSPLAT_UE_ROOT to an Unreal Engine 5.5 source-build directory." >&2
  exit 2
fi
BUILD_SH="${UE_ROOT}/Engine/Build/BatchFiles/Linux/Build.sh"
[[ -x "${BUILD_SH}" ]] || { echo "Build.sh not found: ${BUILD_SH}" >&2; exit 2; }

exec "${BUILD_SH}" OpenFlySplatUEEditor Linux Development \
  "${PROJECT_ROOT}/OpenFlySplatUE.uproject" \
  -WaitMutex -NoHotReloadFromIDE "${@}"
