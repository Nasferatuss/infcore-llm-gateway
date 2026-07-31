# model-toolkit — offline model preparation (build-time, NOT runtime)

Thin wrappers over llama.cpp's tools for preparing GGUF weights in the
enclave: quantization, shard split/merge, importance-matrix, LoRA merging.
These utilities are built by the profile (`LLAMA_BUILD_TOOLS=ON`) but **do not
ship in the runtime image** (the Dockerfile copies only `infcore_gateway`,
`infcore-cli`, `llama-server`) — working with weights happens separately from
the production gateway, on a preparation host.

Everything is offline: the input is a local `.gguf` (converted from HF with
upstream's `convert_hf_to_gguf.py` converter), the output is a local `.gguf`.
No network downloads.

## Usage
```sh
# the build directory with the binaries (default ./build/bin from the root of the engine tree)
export INFCORE_BUILD=/path/to/build

infcore/model-toolkit/model-toolkit.sh quantize  model-f16.gguf  model-Q4_K_M.gguf  Q4_K_M
infcore/model-toolkit/model-toolkit.sh split      model.gguf      model-shard        --split-max-size 20G
infcore/model-toolkit/model-toolkit.sh merge      model-shard-00001-of-00003.gguf   model-merged.gguf
infcore/model-toolkit/model-toolkit.sh imatrix    -m model-f16.gguf -f calib.txt -o model.imatrix
infcore/model-toolkit/model-toolkit.sh export-lora -m base.gguf --lora adapter.gguf -o merged.gguf
```

`quantize` with an imatrix (best quality at low bit-widths):
```sh
infcore/model-toolkit/model-toolkit.sh quantize --imatrix model.imatrix model-f16.gguf out-Q4_K_M.gguf Q4_K_M
```

Common quantization types: `Q8_0`, `Q6_K`, `Q5_K_M`, `Q4_K_M`, `Q4_0`,
`Q3_K_M`, `Q2_K`. Full list: `model-toolkit.sh quantize --help`.

## Compliance with the strategy
The wrappers do not edit upstream; they call the standard `llama-quantize` /
`llama-gguf-split` / `llama-imatrix` / `llama-export-lora` from our own build.
On a drop-in engine update the wrappers keep working (the binary names are
stable).
