# infcore — внутренний слой над llama.cpp

Слой **gateway/SDK** для локального инференса LLM (offline РФ-контур), построенный
**на базе open-source llama.cpp (ggml authors, MIT)** — см. `NOTICE`.

## Модель сопровождения: «обернуть, не трогая ядро»
- **Движок llama.cpp не редактируется.** Слой продукта вынесен в `infcore/`, чтобы
  обновления забирались максимально близко к **drop-in** (см. `scripts/update-upstream.sh`).
- **Своё** живёт только здесь, в `infcore/` (каталога нет в апстриме → нулевой
  конфликт при слиянии). Runtime gateway общается с движком по HTTP через отдельные
  `llama-server` процессы. Файлы `tools/server` не правим — gateway строится рядом.
- Подробности и карта «ядро/периферия/своё» — в `../AUDIT.md` (в корне форка).

## Возможности
- OpenAI-совместимый API (chat/completions, completions, embeddings, rerank, models) + SSE.
- Любые локальные GGUF-модели: text / embeddings / rerank / vision (VLM). Audio — вне области проекта.
- Multi-model registry, ленивый супервайзер (авто-подъём/гашение llama-server),
  authn/RBAC, audit, pull-метрики на `/metrics`, клиентский SDK/CLI.
- Изоляция бэкендов: управляемые `llama-server` слушают только 127.0.0.1 и защищены
  per-boot `--api-key`; прямой доступ к их портам без ключа -> 401.
- Полностью offline: нулевой исходящий трафик в рантайме (`enforce_no_egress` +
  инфра-контроль systemd/docker; проверяется `tests/egress/`).

## Сборка
```
./infcore/scripts/build.sh           # из корня форка; профиль cpu+cuda+vulkan
# или вручную:
cmake -S infcore -B build -C infcore/cmake/profile-rf.cmake
cmake --build build -j
```
Бинари в `build/bin`: `llama-server` (движок-сервер), `infcore_gateway`, `mtmd` и т.д.

## Обновление движка из апстрима
```
./infcore/scripts/update-upstream.sh b1234   # release-тег ggml-org/llama.cpp
```

## Структура
```
cmake/profile-rf.cmake   профиль бэкендов/состава (cpu+cuda+vulkan, server+mtmd)
CMakeLists.txt           супер-проект: add_subdirectory(.. ) движка + слой infcore
gateway/                 надстройка над tools/server: OpenAI-surface, routing, policy, supervisor,
                         pull-метрики на /metrics (VictoriaMetrics/Prometheus)
security/                authn / rbac / audit
registry/                реестр моделей (multi-model, идея из llm_gateway)
runtime/                 lazy-supervisor дочерних llama-server
sdk/python/              клиентский SDK (stdlib-клиент REST)
config/                  конфиги + JSON-Schema
deploy/                  docker / compose / systemd (РФ-образы)
tests/unit/              ctest: RBAC / authn / json-schema / config / supervisor token failure
tests/egress/            проверка нулевого egress + product smoke в netns
docs/                    STATUS, ARCHITECTURE, COMPLIANCE, DEPLOY
NOTICE / sbom.cdx.json / THIRD_PARTY_LICENSES   compliance
```
