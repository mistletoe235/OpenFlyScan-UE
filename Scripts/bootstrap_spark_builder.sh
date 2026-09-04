#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TOOL_ROOT="${PROJECT_ROOT}/Tools/NanoGSTreeBuilder"
LOCK_FILE="${TOOL_ROOT}/spark_builder.lock.json"
SOURCE_DIR="${TOOL_ROOT}/spark-src"
INSTALL_DIR="${TOOL_ROOT}/build"
REPOSITORY="${OPENFLYSPLAT_SPARK_REPOSITORY:-https://github.com/sparkjsdev/spark.git}"

command -v git >/dev/null || { echo "git is required" >&2; exit 2; }
command -v cargo >/dev/null || { echo "Rust/Cargo is required (Rust 1.82 or newer)." >&2; exit 2; }
COMMIT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["source_commit"])' "${LOCK_FILE}")"

if [[ ! -d "${SOURCE_DIR}/.git" ]]; then
  [[ ! -e "${SOURCE_DIR}" ]] || { echo "Not a Git checkout: ${SOURCE_DIR}" >&2; exit 2; }
  git clone --filter=blob:none "${REPOSITORY}" "${SOURCE_DIR}"
fi

git -C "${SOURCE_DIR}" fetch --depth=1 origin "${COMMIT}"
git -C "${SOURCE_DIR}" checkout --detach "${COMMIT}"
if ! git -C "${SOURCE_DIR}" apply --check "${TOOL_ROOT}/spark_stable_source_ids.patch" 2>/dev/null; then
  if git -C "${SOURCE_DIR}" apply --reverse --check "${TOOL_ROOT}/spark_stable_source_ids.patch" 2>/dev/null; then
    echo "Spark source-ID patch is already applied." >&2
  else
    echo "Spark source does not match the locked patch." >&2
    exit 2
  fi
else
  git -C "${SOURCE_DIR}" apply "${TOOL_ROOT}/spark_stable_source_ids.patch"
fi

cargo build --release --manifest-path "${SOURCE_DIR}/rust/build-lod/Cargo.toml"
BUILT="${SOURCE_DIR}/rust/target/release/build-lod"
[[ -x "${BUILT}" ]] || { echo "Expected builder was not produced: ${BUILT}" >&2; exit 2; }
mkdir -p -- "${INSTALL_DIR}"
install -m 0755 "${BUILT}" "${INSTALL_DIR}/build-lod"

echo "SPARK_BUILDER_OK path=${INSTALL_DIR}/build-lod commit=${COMMIT}"
