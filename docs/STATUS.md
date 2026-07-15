# STATUS — отчёт по проекту infcore (форк llama.cpp)

**Дата:** 2026-07-15 · **Статус:** remediation после `REPOSITORY_AUDIT_2026-07-14` · **Ветка:** `infcore`

> Дубликат ключевого контекста живёт в авто-памяти ассистента
> (`llama-cpp-internal-fork.md`). Этот файл — версионируемая копия в самом репо.

---

## 1. Что это за проект
Форк `llama.cpp` → внутренняя **gateway/SDK-библиотека** для локального инференса
LLM (offline, локальное развёртывание на компьютере/сервере в РФ).
- **Модели:** любые локальные GGUF (цель — Qwen3-MoE `qwen3moe`, поддержана из
  коробки; на Qwen не завязываемся).
- **Модальности:** текст, embeddings, rerank, vision (VLM). Audio — вне области проекта (не нужен).
- **Железо:** NVIDIA CUDA + Vulkan + CPU.
- **Жёстко:** полностью offline (нулевой egress в рантайме), требования рос. ПО,
  open-source compliance без сокрытия происхождения.

## 2. Стратегия: «обернуть, не трогая ядро» (wrap-not-touch fork)
- Движок llama.cpp **не редактируем** → обновления из апстрима забираем drop-in
  (release-теги `b####`, не master).
- Всё своё — только в каталоге `infcore/` (апстрим его не создаёт → 0 конфликтов).
- Runtime-граница «чужое/своё» = HTTP к loopback `llama-server`; C/C++ API движка
  остаётся внутри upstream `tools/server`.
- Лишнее железо/бэкенды — гасим **флагами** CMake, исходники в ядре **не удаляем**.
- Физически удаляем только короткий compliance/branding-набор.
- Из Go-репо `llm_gateway` берём **идеи**, не код.

## 3. Состояние репозитория
```
origin/infcore  87b9affab  unit tests, real egress test, CI (P1.3)
                2cfaa0468  low-priority polish (P3)
                4f903d397  fix deploy package (P0.4)
                219e29add  vision mmproj + close RBAC/metrics/authn gaps (P1.1, P1.2)
                c4694409f  harden gateway/supervisor (P0)
                ...        CLI/admin, JSON-Schema, deploy-обвязка, каркас+gateway
base            e8ecce5    апстрим ggml-org/llama.cpp (2026-06-25)
```
remotes: `origin`=Nasferatuss/llama.cpp, `upstream`=ggml-org/llama.cpp.

## 4. Что СДЕЛАНО
- **Git/структура:** ветка `infcore`, upstream добавлен, слой `infcore/` внутри форка.
- **Сборка:** `infcore/CMakeLists.txt` встраивает движок через `add_subdirectory(..)`
  без правок апстрима (форсит `LLAMA_BUILD_COMMON=ON`); профиль
  `infcore/cmake/profile-rf.cmake` (cpu+cuda+vulkan ON; metal/sycl/opencl/cann/musa/
  hexagon/openvino/webgpu/zdnn/zendnn/virtgpu/hip/rpc=OFF; server+mtmd ON;
  UI/app/examples=OFF; `BUILD_SHARED_LIBS=OFF` для самодостаточного runtime artifact).
- **Gateway** (`infcore/gateway/`, proxy-front): OpenAI-совместимый control-plane
  перед бэкендами `llama-server`. `/health`, `/v1/models`, Bearer-auth,
  `/v1/chat/completions`, `/v1/completions`, `/v1/embeddings`, `/v1/rerank`,
  `/v1/reranking`, `/rerank`, `/reranking`, маршрутизация по
  registry, SSE, OpenAI-формат ошибок, `/metrics` (на основном порту).
- **Ленивый супервайзер:** авто-подъём `llama-server` из registry по первому
  запросу, health-check, гашение по простою. Порт назначается под локом (без гонки),
  backoff + сброс порта при неудачном старте, liveness через `waitpid(WNOHANG)`
  (без зомби, детект упавших бэкендов), RAII active-token (аборт клиента не течёт).
- **Изоляция бэкендов:** управляемые `llama-server` всегда слушают 127.0.0.1 и
  стартуют с per-boot случайным `--api-key`; прокси добавляет ключ на `Authorization`.
  Прямой доступ к портам бэкендов (8100+) без ключа -> 401. Закрывает прежний обход
  RBAC/audit. fd-гигиена: audit fd `O_CLOEXEC`, ребёнок закрывает унаследованные fd
  перед exec, backend получает allowlist environment без `INFCORE_KEY_*`.
- **RBAC + audit:** доступ роль -> модель/эндпоинт (allow_endpoints, в т.ч. для
  `/v1/models`); audit пишет и отказы (400/404/409/502), и реальный статус бэкенда.
- **JSON-Schema валидация конфига** при старте (fail-fast).
- **Release gates:** `offline.require_model_integrity` проверяет SHA-256/размер/права
  model artifacts; `runtime.max_loaded_models`, `max_parallel_starts` и
  `rate_limit_per_minute` дают базовый admission control.
- **SSE hardening:** статус апстрима проверяется ДО коммита стрима; не-2xx бэкенд
  возвращается обычным JSON-ошибкой (OpenAI shape), а не SSE внутри 200; синтетические
  ошибки завершают стрим `data: [DONE]`.
- **Vision mmproj:** модель задаёт `mmproj_path` -> `llama-server --mmproj`;
  управляемые vision без `mmproj_path` отклоняются при загрузке конфига.
- **enforce_no_egress / секреты:** любой внешний `backend_url` валидируется как
  loopback/RFC1918 (иначе fail-fast); API-ключи сравниваются constant-time;
  слабые/placeholder-ключи отклоняются на старте; legacy `security.api_keys`
  даёт deprecation-warning.
- **CLI + админ-ручка** `/admin/models`.
- **Deploy-пакет** (`infcore/deploy/`): docker/compose/systemd под РФ-контур,
  kernel-level egress deny (systemd `IPAddressDeny=any`), docker `internal: true`.
- **Audit:** JSONL append-only с `request_id`, subject/role, endpoint/model,
  `model_sha256`, backend_id, outcome/status, latency/bytes и token usage при наличии `usage`.
- **Тесты + CI:** ctest unit-тесты (`infcore/tests/unit`: RBAC/authn/json-schema/
  config/supervisor token failure), product egress-тест (`infcore/tests/egress`,
  netns-based, real gateway + fake managed backend when `INFCORE_GATEWAY_BIN` is set),
  release manifest smoke, `.github/workflows/infcore.yml` и `.gitlab-ci.yml`.

## 5. Технические «грабли» (важно)
- `tools/ui` физически удалять НЕЛЬЗЯ — ломает сборку сервера; только флаги UI=OFF.
- Супер-проект обязан форсить `LLAMA_BUILD_COMMON=ON` (при встраивании standalone=OFF).
- `scripts/ui-assets.cmake` не удалять (нужен сборке tools/ui даже при UI=OFF).
- gateway = подпроцессы llama-server (proxy-front), не in-process — ради drop-in + изоляции падений.
- CUDA/Vulkan проверяются только на целевом железе.
- Движок (upstream) СОДЕРЖИТ сетевой код загрузки (`common/download.cpp`, `-hf`/
  `--model-url`, fetch картинок в `server-common.cpp`) — по wrap-not-touch его НЕ
  удаляем, а нейтрализуем: супервайзер не передаёт download-триггерящих аргументов,
  egress режется на инфра-уровне (systemd/docker) и валидацией конфига.

## 6. Следующие шаги (по приоритету)
- ✅ `model-toolkit` (offline quantize/split/imatrix/export-lora обёртки; не рантайм) —
  `infcore/model-toolkit/`.
- ⚪ Полноценное хранилище секретов (secrets store).
- ⚪ platform-pass (убрать `scripts/apple`,`hip`), целевая сборка CUDA/Vulkan,
  обкатка РФ-деплоя.

**RAG:** исключён из проекта (решение заказчика 2026-06-29).

## 7. Как возобновить
Рабочий каталог `/Users/nasferatus/projects/llama/fork` (ветка `infcore`). Правки —
через worktree → commit → `git -C fork merge --ff-only <wt-branch>` → push origin
infcore → ExitWorktree remove. Сборка: `./infcore/scripts/build.sh` (целевое железо).
Обновление движка: `./infcore/scripts/update-upstream.sh b####`.
