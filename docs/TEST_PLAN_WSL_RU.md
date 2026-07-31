# Тестирование infcore на потребительской GPU под WSL2

Цель: проверить `infcore_gateway` с локальной Qwen 3.5 GGUF-моделью на обычном
ноутбуке с дискретной NVIDIA. Референсная конфигурация, на которой план
прогонялся: Core i7-13700HX, 64 GB RAM, RTX 4070 8 GB, Windows 11 Pro.

Для `infcore_gateway` основной сценарий теста — WSL2 Ubuntu. Windows остаётся хостом
с NVIDIA-драйвером и GPU, а gateway/backend запускаются внутри Linux-среды WSL2.

## 0. Что понадобится

На Windows:

- свежий NVIDIA driver;
- WSL2 Ubuntu 22.04 или 24.04;
- локальная модель `.gguf`, например `C:\Models\qwen....gguf`.

В WSL:

- `git`, `cmake`, `ninja`, `g++`, `python3`, `pytest`, `openssl`, `libssl-dev`;
- CUDA Toolkit внутри WSL, чтобы был доступен `nvcc`.

## 1. Установить и проверить WSL2

PowerShell от администратора:

```powershell
wsl --install -d Ubuntu-24.04
wsl --set-default-version 2
```

Перейти в Ubuntu/WSL и проверить GPU:

```bash
nvidia-smi
```

Если `nvidia-smi` не работает, сначала обновить NVIDIA driver на Windows.
Без этого CUDA-тесты не имеют смысла.

Проверить CUDA compiler:

```bash
nvcc --version
```

Если `nvcc` не найден, поставить CUDA Toolkit для WSL/Linux и снова проверить
`nvcc --version`.

## 2. Подготовить окружение в WSL

```bash
sudo apt update
sudo apt install -y git build-essential cmake ninja-build python3 python3-pip python3-pytest curl openssl libssl-dev pkg-config jq
```

## 3. Забрать репозиторий

```bash
cd ~
git clone https://github.com/Nasferatuss/llama.cpp.git
cd llama.cpp
git checkout infcore
git pull origin infcore
```

Проверить актуальный коммит:

```bash
git rev-parse --short HEAD
```

Ожидается `bf337ef5a` или новее.

## 4. Положить модель в WSL

Можно читать модель с Windows-диска через `/mnt/c/...`, но обычно быстрее скопировать
её в файловую систему WSL.

```bash
mkdir -p ~/models
cp "/mnt/c/Models/qwen-your-model.gguf" ~/models/qwen.gguf
ls -lh ~/models/qwen.gguf
```

## 5. Собрать CUDA-версию под RTX 4070

RTX 4070 — Ada, CUDA architecture `89`.

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

Запустить unit-тесты:

```bash
ctest --test-dir build-infcore-cuda --output-on-failure
```

## 6. Сначала проверить чистый llama-server

Это отделяет проблемы модели/GPU от gateway.

```bash
MODEL="$HOME/models/qwen.gguf"

./build-infcore-cuda/bin/llama-server \
  -m "$MODEL" \
  --host 127.0.0.1 \
  --port 18081 \
  --ctx-size 4096 \
  --n-gpu-layers 999
```

Во втором WSL-терминале:

```bash
curl -s http://127.0.0.1:18081/health
```

Тест chat:

```bash
curl -s http://127.0.0.1:18081/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen",
    "messages":[{"role":"user","content":"Ответь коротко: 2+2?"}],
    "max_tokens":64,
    "temperature":0.2
  }' | jq
```

Если CUDA OOM:

- снизить `--n-gpu-layers`, например до `60`, `40`, `30`, `20`;
- снизить `--ctx-size` до `2048`;
- для RTX 4070 8 GB большие Qwen-модели будут только частично на GPU.

Остановить `llama-server`: `Ctrl+C`.

## 7. Создать конфиг gateway

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

Если будет CUDA OOM, поменять в конфиге:

```json
"n_gpu_layers": 40,
"n_ctx": 2048
```

## 8. Запустить gateway

В первом терминале:

```bash
cd ~/llama.cpp
export INFCORE_KEY_ADMIN="$(cat ~/infcore-config/admin.key)"

./build-infcore-cuda/bin/infcore_gateway ~/infcore-config/gateway.local.json
```

Оставить этот терминал открытым.

## 9. Проверить gateway

Во втором терминале:

```bash
cd ~/llama.cpp
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

Первый chat-запрос впервые поднимет backend и может занять 1-3 минуты:

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer $INFCORE_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model":"qwen35",
    "messages":[{"role":"user","content":"Ответь коротко: столица России?"}],
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
    "messages":[{"role":"user","content":"Считай от 1 до 5."}],
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

Проверить, что в audit есть `request_id`, `model_sha256`, `latency_ms`, `status`.

## 10. Проверить CLI

```bash
./build-infcore-cuda/bin/infcore-cli --url http://127.0.0.1:8080 health

./build-infcore-cuda/bin/infcore-cli \
  --url http://127.0.0.1:8080 \
  --key-file ~/infcore-config/admin.key \
  models

./build-infcore-cuda/bin/infcore-cli \
  --url http://127.0.0.1:8080 \
  --key-file ~/infcore-config/admin.key \
  chat -m qwen35 "Напиши одно предложение про локальные LLM."
```

## 11. Запустить e2e tests

```bash
export INFCORE_URL="http://127.0.0.1:8080"
export INFCORE_KEY="$(cat ~/infcore-config/admin.key)"
export INFCORE_E2E_MODEL="qwen35"
export INFCORE_E2E_TIMEOUT="300"

python3 -m pytest infcore/tests/e2e -v
```

Embedding/vision/rerank тесты пропустятся, если не заданы соответствующие
env-переменные. Для первой проверки это нормально.

## 12. Проверить egress/product smoke

Этот тест в WSL может пройти или skip-нуться. Это зависит от того, разрешён ли
`unshare -rn`.

```bash
INFCORE_GATEWAY_BIN="$PWD/build-infcore-cuda/bin/infcore_gateway" \
python3 -m pytest infcore/tests/egress -q -m egress
```

Если `skipped` из-за netns — для WSL это допустимо. На Linux-сервере этот тест должен
проходить.

## 13. Следить за GPU

В отдельном PowerShell или WSL:

```bash
nvidia-smi -l 1
```

Во время первого запроса должна появиться загрузка VRAM. Если VRAM забилась и процесс
упал:

1. уменьшить `"n_gpu_layers"`;
2. уменьшить `"n_ctx"`;
3. перезапустить gateway.

## 14. Частые ошибки

### `502 backend start failed`

- запустить `llama-server` напрямую как в шаге 6;
- проверить путь `runtime.llama_server_bin`;
- проверить путь `gguf_path`;
- снизить `n_gpu_layers`.

### `401 unauthorized`

- используется не тот ключ;
- проверить `export INFCORE_KEY=...`;
- проверить, что gateway запущен с `INFCORE_KEY_ADMIN`.

### `sha256 mismatch`

- модель изменилась или путь не тот;
- пересчитать:

```bash
sha256sum ~/models/qwen.gguf
stat -c%s ~/models/qwen.gguf
```

### `artifact writable для group/other`

```bash
chmod 0644 ~/models/qwen.gguf
```

### CUDA не используется

- проверить `nvidia-smi`;
- проверить, что сборка была с `-DGGML_CUDA=ON`;
- в логах `llama-server` должен быть CUDA backend;
- проверить, что `n_gpu_layers` не `0`.

## 15. Что прислать после теста

```bash
git rev-parse --short HEAD
ctest --test-dir build-infcore-cuda --output-on-failure
curl -s http://127.0.0.1:8080/health
curl -s http://127.0.0.1:8080/v1/models -H "Authorization: Bearer $INFCORE_KEY"
tail -n 3 ~/infcore-logs/audit.log
nvidia-smi
```

Также указать:

- точное имя GGUF;
- размер модели;
- quantization, например `Q4_K_M`, `Q5_K_M`, `Q8_0`;
- сколько VRAM заняло;
- какой `n_gpu_layers` в итоге завёлся без OOM.
