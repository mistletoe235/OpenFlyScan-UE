#!/usr/bin/env bash
set -euo pipefail

[[ $# -ge 1 && $# -le 2 ]] || {
  echo "Usage: $0 PACKAGED_OPENFLYSPLATUE_ROOT [OUTPUT.tar.zst]" >&2
  exit 2
}
ROOT="$(realpath -- "$1")"
[[ -x "${ROOT}/Binaries/Linux/OpenFlySplatUE" ]] || {
  echo "Not an OpenFlySplatUE package root: ${ROOT}" >&2
  exit 2
}
OUTPUT="$(realpath -m -- "${2:-${ROOT%/}.tar.zst}")"
[[ ! -e "${OUTPUT}" ]] || { echo "Output already exists: ${OUTPUT}" >&2; exit 2; }
command -v zstd >/dev/null || { echo "zstd is required." >&2; exit 2; }

tar --directory="$(dirname -- "${ROOT}")" --create --file=- "$(basename -- "${ROOT}")" \
  | zstd -T0 -19 -o "${OUTPUT}"
sha256sum "${OUTPUT}" >"${OUTPUT}.sha256"
echo "OPENFLYSPLAT_ARCHIVE_OK file=${OUTPUT}"
