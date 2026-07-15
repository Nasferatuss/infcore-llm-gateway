# Remediation Status: REPOSITORY_AUDIT_2026-07-14

Дата: 2026-07-15

## Закрыто кодом

- AUD-P0-01 runtime packaging: профиль `profile-rf.cmake` форсирует `BUILD_SHARED_LIBS=OFF`; Dockerfile и GitHub Actions выполняют `ldd` smoke без `not found`; runtime image включает license bundle.
- AUD-P0-02 reranking: добавлена модальность `rerank`, schema/config/example, supervisor запускает `llama-server --reranking`, gateway проксирует `/v1/rerank`, `/v1/reranking`, `/rerank`, `/reranking`, SDK и e2e получили rerank.
- AUD-P0-03 secret inheritance: backend запускается через `execve` с allowlist environment; `INFCORE_KEY_*` не наследуются; product egress smoke проверяет это на fake managed backend.
- AUD-P0-04 backend internal API key downgrade: ошибка чтения entropy source теперь фатальна; unit test покрывает failure path.
- AUD-P0-05 offline proof: egress test при `INFCORE_GATEWAY_BIN` запускает реальный gateway в netns, делает запрос через lazy supervisor и проверяет отсутствие маршрута наружу.
- AUD-P0-06 compliance placeholders: placeholders в `infcore/LICENSE`/`NOTICE` удалены; Docker runtime image включает root MIT license, infcore license, NOTICE, SBOM и third-party license bundle.
- AUD-P0-07 GitHub CI: добавлен `.github/workflows/infcore.yml` для ветки `infcore`.

## Частично закрыто / ограничено текущим scope

- AUD-P1-01 config schema: неподдержанные `mtls`/`oidc` удалены из schema; `observability.metrics_*` получил runtime-семантику; добавлены fail-fast проверки weak keys, duplicate keys/roles/models, unknown model references, `audit.require=true` + `sink=none`.
- AUD-P1-03 DoS: read/write/body/request limits сохранены; добавлены `max_loaded_models`, `max_parallel_starts` и per-subject `rate_limit_per_minute`. Full custom bounded httplib queue остаётся отдельной задачей.
- AUD-P1-04 audit payload: добавлены `request_id`, `backend_id`, `latency_ms`, bytes и token usage при наличии `usage` в ответе backend.
- AUD-P1-05 OpenAI compatibility: поддержанный subset расширен rerank; `/v1/responses` и token count явно не реализованы.
- AUD-P1-06 deployment hardening: compose/systemd усилены (`read_only`, `cap_drop`, `no-new-privileges`, pids/mem limits, systemd hardening).

## Остаётся к отдельной реализации

- Port reservation через bound socket/OS-assigned port и GPU/RAM admission control.
- Full custom bounded request queue внутри HTTP server.
- Trusted proxy/X-Forwarded-For policy.
- Автоматическая генерация полного dependency SBOM для system/CUDA/Vulkan/base image.
