#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-linux}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_JOBS="${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"

for tool in cmake c++ getconf; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "Required tool '${tool}' was not found. Install the Linux build dependencies from the README." >&2
    exit 2
  fi
done

for dependency in \
  "${ROOT_DIR}/extern/dynarmic/CMakeLists.txt" \
  "${ROOT_DIR}/extern/sqlite3/sqlite3.c" \
  "${ROOT_DIR}/extern/stb/stb_truetype.h"; do
  if [[ ! -f "${dependency}" ]]; then
    echo "Required dependency is missing: ${dependency}" >&2
    echo "Prepare the Linux dependencies as described in the README, then run this script again." >&2
    exit 2
  fi
done

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}" --parallel "${BUILD_JOBS}"

echo "Linux executable: ${BUILD_DIR}/clashofclans_loader"
