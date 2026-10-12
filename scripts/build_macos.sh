#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-macos}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This script must be run on macOS." >&2
  exit 2
fi

for tool in cmake c++ sysctl brew; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "Required tool '${tool}' was not found. Install Xcode Command Line Tools, CMake, and Homebrew." >&2
    exit 2
  fi
done

for dependency in \
  "${ROOT_DIR}/extern/dynarmic/CMakeLists.txt" \
  "${ROOT_DIR}/extern/sqlite3/sqlite3.c" \
  "${ROOT_DIR}/extern/stb/stb_truetype.h"; do
  if [[ ! -f "${dependency}" ]]; then
    echo "Required dependency is missing: ${dependency}" >&2
    echo "Prepare dependencies as described in the README, then run this script again." >&2
    exit 2
  fi
done

if ! brew list --versions boost >/dev/null 2>&1; then
  echo "Homebrew Boost headers were not found. Install them with: brew install boost" >&2
  exit 2
fi

BUILD_JOBS="${BUILD_JOBS:-$(sysctl -n hw.ncpu)}"
if [[ ! "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "BUILD_JOBS must be a positive integer; received '${BUILD_JOBS}'." >&2
  exit 2
fi

BOOST_INCLUDE_DIR="$(brew --prefix boost)/include"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DBoost_INCLUDE_DIR="${BOOST_INCLUDE_DIR}"
cmake --build "${BUILD_DIR}" --parallel "${BUILD_JOBS}"

echo "macOS executable: ${BUILD_DIR}/clashofclans_loader"
