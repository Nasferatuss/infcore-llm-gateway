# Testing infcore on a consumer GPU under WSL2

Goal: verify `infcore_gateway` with a local Qwen 3.5 GGUF model on an ordinary
laptop with a discrete NVIDIA GPU. Reference configuration this plan was run on:
Core i7-13700HX, 64 GB RAM, RTX 4070 8 GB, Windows 11 Pro.

The primary test scenario for `infcore_gateway` is WSL2 Ubuntu. Windows stays
the host with the NVIDIA driver and the GPU, while the gateway/backend run
inside the WSL2 Linux environment.

## 0. What you need

On Windows:

- a recent NVIDIA driver;
- WSL2 Ubuntu 22.04 or 24.04;
- a local `.gguf` model, e.g. `C:\Models\qwen....gguf`.

In WSL:

- `git`, `cmake`, `ninja`, `g++`, `python3`, `pytest`, `openssl`, `libssl-dev`;
- the CUDA Toolkit inside WSL, so that `nvcc` is available.

## 1. Install and verify WSL2

PowerShell as administrator:

```powershell
wsl --install -d Ubuntu-24.04
wsl --set-default-version 2
```

Switch into Ubuntu/WSL and check the GPU:

```bash
nvidia-smi
```

If `nvidia-smi` does not work, update the NVIDIA driver on Windows first.
Without it the CUDA tests are meaningless.

Check the CUDA compiler:

```bash
nvcc --version
```

If `nvcc` is not found, install the CUDA Toolkit for WSL/Linux and check
`nvcc --version` again.

## 2. Prepare the WSL environment

```bash
sudo apt update
sudo apt install -y git build-essential cmake ninja-build python3 python3-pip python3-pytest curl openssl libssl-dev pkg-config jq
```

## 3. Get the code

infcore builds inside the llama.cpp tree as the `infcore/` subdirectory:

```bash
cd ~
git clone --depth 1 https://github.com/ggml-org/llama.cpp engine-src
git clone --depth 1 https://github.com/Nasferatuss/infcore-llm-gateway \
    engine-src/infcore
cd engine-src
```

Check which revision of the infcore layer is under test:

```bash
git -C infcore rev-parse --short HEAD
```

The upstream pin (the engine tag the layer is built against) is in
`infcore/sbom.cdx.json`.

## 4. Put the model into WSL

You can read the model from the Windows disk via `/mnt/c/...`, but copying it
into the WSL filesystem is usually faster.

```bash
mkdir -p ~/models
cp "/mnt/c/Models/qwen-your-model.gguf" ~/models/qwen.gguf
ls -lh ~/models/qwen.gguf
```

## 5. Build the CUDA version for the RTX 4070

The RTX 4070 is Ada, CUDA architecture `89`.

```bash
cmake -S infcore -B build-infcore-cuda -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=89 \
  -DGGML_VULKAN=OFF \
  -DGGML_NATIVE=OFF \
  -DLLAMA_BUILD_SERVER=ON \
  -DLLAMA_BUILD_TOOLS=ON \
  -DLLAMA_BUILD_UI=OFF \
  -DLLAMA_USE_PREBUILT_UI=OFF \
  -DLLAMA_BUILD_APP=OFF \
  -DLLAMA_BUILD_EXAMPLES=OFF \
  -DLLAMA_BUILD_TESTS=OFF \
  -DINFCORE_BUILD_TESTS=ON

cmake --build build-infcore-cuda --target infcore_gateway infcore_cli llama-server infcore_unit_tests -j"$(nproc)"
```

Run the unit tests:

```bash
ctest --test-dir build-infcore-cuda --output-on-failure
```

## 6. Verify bare llama-server first

This separates model/GPU problems from gateway problems.

```bash
MODEL="$HOME/models/qwen.gguf"

./build-infcore-cuda/bin/llama-server \
  -m "$MODEL" \
  --host 127.0.0.1 \
  --port 18081 \
  --ctx-size 4096 \
  --n-gpu-layers 999
```

In a second WSL terminal:

```bash
curl -s http://127.0.0.1:18081/health
```

Chat test:

```bash
curl -s http://127.0.0.1:18081/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen",
    "messages":[{"role":"user","content":"Answer briefly: 2+2?"}],
    "max_tokens":64,
    "temperature":0.2
  }' | jq
```

On CUDA OOM:

- lower `--n-gpu-layers`, e.g. to `60`, `40`, `30`, `20`;
- lower `--ctx-size` to `2048`;
- on an RTX 4070 8 GB, large Qwen models will only partially fit on the GPU.

Stop `llama-server` with `Ctrl+C`.

## 7. Create the gateway config

```bash
mkdir -p ~/infcore-config ~/infcore-logs

KEY="$(openssl rand -hex 32)"
echo "$KEY" > ~/infcore-config/admin.key
chmod 600 ~/infcore-config/admin.key

MODEL="$HOME/models/qwen.gguf"
SHA="$(sha256sum "$MODEL" | awk '{print $1}')"
SIZE="$(stat -c%s "$MODEL")"
LLAMA_SERVER="$PWD/build-infcore-cuda/bin/llama-server"
AUDIT="$HOME/infcore-logs/audit.log"

cat > ~/infcore-config/gateway.local.json <<EOF
{
  "server": {
    "host": "127.0.0.1",
    "port": 8080,
    "max_concurrent_requests": 8,
    "request_timeout_ms": 300000,
    "read_timeout_ms": 30000,
    "write_timeout_ms": 300000,
    "max_body_bytes": 8388608
  },
  "security": {
    "authn": "api_key",
    "rbac_enabled": true,
    "principals": [
      { "api_key": "env:INFCORE_KEY_ADMIN", "subject": "tester", "role": "admin" }
    ],
    "roles": [
      { "name": "admin", "allow_models": ["*"], "allow_endpoints": ["*"] }
    ],
    "audit": {
      "sink": "file",
      "path": "$AUDIT",
      "require": true
    }
  },
  "observability": {
    "metrics_enabled": true,
    "metrics_path": "/metrics"
  },
  "offline": {
    "enforce_no_egress": true,
    "require_model_integrity": true
  },
  "runtime": {
    "llama_server_bin": "$LLAMA_SERVER",
    "port_range_start": 18100,
    "max_loaded_models": 1,
    "max_parallel_starts": 1,
    "rate_limit_per_minute": 0,
    "idle_timeout_ms": 600000,
    "startup_timeout_ms": 300000
  },
  "models": [
    {
      "logical_name": "qwen35",
      "gguf_path": "$MODEL",
      "sha256": "$SHA",
      "size_bytes": $SIZE,
      "license": "local-test",
      "source": "local-test-laptop",
      "modality": "text",
      "enabled": true,
      "n_ctx": 4096,
      "n_gpu_layers": 999
    }
  ]
}
EOF
```

On CUDA OOM, change in the config:

```json
"n_gpu_layers": 40,
"n_ctx": 2048
```

## 8. Start the gateway

In the first terminal:

```bash
cd ~/engine-src
export INFCORE_KEY_ADMIN="$(cat ~/infcore-config/admin.key)"

./build-infcore-cuda/bin/infcore_gateway ~/infcore-config/gateway.local.json
```

Leave this terminal open.

## 9. Verify the gateway

In the second terminal:

```bash
cd ~/engine-src
export INFCORE_KEY="$(cat ~/infcore-config/admin.key)"
```

Health:

```bash
curl -s http://127.0.0.1:8080/health | jq
```

Models:

```bash
curl -s http://127.0.0.1:8080/v1/models \
  -H "Authorization: Bearer $INFCORE_KEY" | jq
```

The first chat request starts the backend for the first time and may take 1–3
minutes:

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer $INFCORE_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen35",
    "messages":[{"role":"user","content":"Answer briefly: what is the capital of France?"}],
    "max_tokens":64,
    "temperature":0.2,
    "stream":false
  }' | jq
```

Streaming:

```bash
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer $INFCORE_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen35",
    "messages":[{"role":"user","content":"Count from 1 to 5."}],
    "max_tokens":128,
    "temperature":0.2,
    "stream":true
  }'
```

Metrics:

```bash
curl -s http://127.0.0.1:8080/metrics
```

Audit:

```bash
tail -n 5 ~/infcore-logs/audit.log | jq
```

Check that the audit records carry `request_id`, `model_sha256`, `latency_ms`,
`status`.

## 10. Verify the CLI

```bash
./build-infcore-cuda/bin/infcore-cli --url http://127.0.0.1:8080 health

./build-infcore-cuda/bin/infcore-cli \
  --url http://127.0.0.1:8080 \
  --key-file ~/infcore-config/admin.key \
  models

./build-infcore-cuda/bin/infcore-cli \
  --url http://127.0.0.1:8080 \
  --key-file ~/infcore-config/admin.key \
  chat -m qwen35 "Write one sentence about local LLMs."
```

## 11. Run the e2e tests

```bash
export INFCORE_URL="http://127.0.0.1:8080"
export INFCORE_KEY="$(cat ~/infcore-config/admin.key)"
export INFCORE_E2E_MODEL="qwen35"
export INFCORE_E2E_TIMEOUT="300"

python3 -m pytest infcore/tests/e2e -v
```

The embedding/vision/rerank tests are skipped unless the corresponding env
variables are set. For a first pass that is fine.

## 12. Run the egress/product smoke test

Under WSL this test may pass or be skipped, depending on whether `unshare -rn`
is allowed.

```bash
INFCORE_GATEWAY_BIN="$PWD/build-infcore-cuda/bin/infcore_gateway" \
python3 -m pytest infcore/tests/egress -q -m egress
```

If it is `skipped` because of netns — that is acceptable on WSL. On a Linux
server this test must pass.

## 13. Watch the GPU

In a separate PowerShell or WSL terminal:

```bash
nvidia-smi -l 1
```

During the first request, VRAM usage should appear. If VRAM filled up and the
process died:

1. lower `"n_gpu_layers"`;
2. lower `"n_ctx"`;
3. restart the gateway.

## 14. Common failures

### `502 backend start failed`

- run `llama-server` directly as in step 6;
- check the `runtime.llama_server_bin` path;
- check the `gguf_path`;
- lower `n_gpu_layers`.

### `401 unauthorized`

- the wrong key is being used;
- check `export INFCORE_KEY=...`;
- check that the gateway was started with `INFCORE_KEY_ADMIN`.

### `sha256 mismatch`

- the model changed or the path is wrong;
- recompute:

```bash
sha256sum ~/models/qwen.gguf
stat -c%s ~/models/qwen.gguf
```

### `artifact writable for group/other`

```bash
chmod 0644 ~/models/qwen.gguf
```

### CUDA is not being used

- check `nvidia-smi`;
- check that the build had `-DGGML_CUDA=ON`;
- the `llama-server` logs must mention the CUDA backend;
- check that `n_gpu_layers` is not `0`.

## 15. What to report after the test

```bash
git -C infcore rev-parse --short HEAD
ctest --test-dir build-infcore-cuda --output-on-failure
curl -s http://127.0.0.1:8080/health
curl -s http://127.0.0.1:8080/v1/models -H "Authorization: Bearer $INFCORE_KEY"
tail -n 3 ~/infcore-logs/audit.log
nvidia-smi
```

Also include:

- the exact GGUF name;
- the model size;
- the quantization, e.g. `Q4_K_M`, `Q5_K_M`, `Q8_0`;
- how much VRAM it took;
- which `n_gpu_layers` value ended up working without OOM.
