#!/usr/bin/env bash
set -euo pipefail

RUNTIME_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SCENE="${RUNTIME_ROOT}/Content/NanoGSData/RuntimeActive/active_scene.runtime.json"
[[ -f "${SCENE}" ]] || { echo "Scene not installed: ${SCENE}" >&2; exit 1; }
if [[ "${1:-}" != "--preview" ]]; then
  exec "${RUNTIME_ROOT}/run_openfly_hil.sh" "$@"
fi
shift
exec "${RUNTIME_ROOT}/run_nanogs_profile.sh" project-default \
  "-settings=${RUNTIME_ROOT}/NanoGSConverter/Config/ExpoEastPreview.json" \
  "-RuntimeUI=${OPENFLY_RUNTIME_UI:-None}" \
  -windowed -ResX=1920 -ResY=1080 -WinX=100 -WinY=80 "$@"
