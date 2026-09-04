#!/usr/bin/env bash
set -euo pipefail

RUNTIME_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BINARY="${RUNTIME_ROOT}/Binaries/Linux/OpenFlySplatUE"

usage() {
  cat <<'EOF'
Usage: ./run_nanogs_profile.sh PROFILE [Unreal arguments...]

Profiles:
  project-default   No rendering console-variable overrides at all.
  upstream-neutral  Emulate the unadjusted NanoGS composite parameters.
  display-parity    Explicitly restate the production display/LOD baseline.

The project-default profile is the literal zero-override production default.
The other profiles remain explicit, repeatable comparison controls.
EOF
}

[[ $# -ge 1 ]] || { usage >&2; exit 2; }
PROFILE="$1"
shift
[[ -x "${BINARY}" ]] || { echo "Runtime binary not found: ${BINARY}" >&2; exit 2; }

for argument in "$@"; do
  case "${argument}" in
    -ExecCmds=*|-execcmds=*)
      echo "Do not pass -ExecCmds to a named profile; it would invalidate the baseline." >&2
      exit 2
      ;;
  esac
done

case "${PROFILE}" in
  project-default)
    exec "${BINARY}" OpenFlySplatUE "$@"
    ;;
  upstream-neutral)
    exec "${BINARY}" OpenFlySplatUE \
      '-ExecCmds=gs.CompositeAfterTonemap 0,gs.CompositeBrightness 1,gs.ApplyPreExposure 0,gs.CompensateUEFilmic 0,gs.AfterTonemapBrightness 1' \
      "$@"
    ;;
  display-parity)
    exec "${BINARY}" OpenFlySplatUE \
      '-ExecCmds=gs.CompositeAfterTonemap 1,gs.AfterTonemapBrightness 1,gs.EnableHeuristicLOD 0,gs.DebugForceLODLevel -1,gs.TreeGPUExperimentalSelector 0,gs.TreeGPUExclusiveSelector 1,gs.TreeSelectorHierarchicalHeap 1,gs.TreeProgressiveFirstBudget -1,gs.MaxRenderBudget 0,gs.UseFloat32Position 1,gs.UseFloat32RotationScale 1' \
      "$@"
    ;;
  -h|--help)
    usage
    ;;
  *)
    echo "Unknown profile: ${PROFILE}" >&2
    usage >&2
    exit 2
    ;;
esac
