#!/usr/bin/env bash
# infcore — build for an isolated deployment (profile: cpu+cuda+vulkan, server+mtmd ON).
# Run from the ROOT of the engine tree:  ./infcore/scripts/build.sh [build_dir]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${1:-${ROOT}/build}"
cmake -S "${ROOT}/infcore" -B "${BUILD}" -C "${ROOT}/infcore/cmake/profile-portable.cmake"
cmake --build "${BUILD}" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
echo "Done. Binaries: ${BUILD}/bin (llama-server, infcore_gateway, mtmd, ...)"
