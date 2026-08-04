#!/usr/bin/env bash
# infcore model-toolkit — offline wrappers around the llama.cpp tools (build-time).
# It does not edit upstream; it calls the stock binaries from our build. All local, no network.
set -euo pipefail

BUILD="${INFCORE_BUILD:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/build}"
BIN="${BUILD}/bin"

die() { echo "model-toolkit: $*" >&2; exit 1; }

need() {
  local tool="$1"
  [ -x "${BIN}/${tool}" ] || die "${BIN}/${tool} not found — build with a profile (LLAMA_BUILD_TOOLS=ON) or set INFCORE_BUILD"
}

usage() {
  cat >&2 <<EOF
Usage: model-toolkit.sh <command> [arguments...]
  quantize <in.gguf> <out.gguf> <type> [--imatrix f.imatrix] [other llama-quantize flags]
  split    <in.gguf> <out-prefix> [--split-max-size 20G | --split-max-tensors N]
  merge    <shard-00001-of-000NN.gguf> <out.gguf>
  imatrix  [llama-imatrix flags: -m ... -f calib.txt -o out.imatrix]
  export-lora [llama-export-lora flags: -m base.gguf --lora a.gguf -o out.gguf]
INFCORE_BUILD=${BUILD}
EOF
  exit 2
}

[ $# -ge 1 ] || usage
cmd="$1"; shift

case "${cmd}" in
  quantize)
    need llama-quantize
    # llama-quantize takes [--imatrix file] before the positional arguments; pass everything through as-is.
    exec "${BIN}/llama-quantize" "$@"
    ;;
  split)
    need llama-gguf-split
    exec "${BIN}/llama-gguf-split" --split "$@"
    ;;
  merge)
    need llama-gguf-split
    exec "${BIN}/llama-gguf-split" --merge "$@"
    ;;
  imatrix)
    need llama-imatrix
    exec "${BIN}/llama-imatrix" "$@"
    ;;
  export-lora)
    need llama-export-lora
    exec "${BIN}/llama-export-lora" "$@"
    ;;
  -h|--help|help) usage ;;
  *) die "unknown command '${cmd}' (see --help)" ;;
esac
