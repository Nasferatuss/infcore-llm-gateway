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
- AUD-P1-03 DoS: уже существующие read/write/body/request limits сохранены; full bounded queue/rate limit остаются отдельной задачей.
- AUD-P1-04 audit payload: документация приведена к фактическому payload. Расширение request_id/latency/tokens/model hash остаётся P1.
- AUD-P1-05 OpenAI compatibility: поддержанный subset расширен rerank; `/v1/responses` и token count явно не реализованы.
- AUD-P1-06 deployment hardening: compose/systemd усилены (`read_only`, `cap_drop`, `no-new-privileges`, pids/mem limits, systemd hardening).

## Остаётся к отдельной реализации

- Port reservation через bound socket/OS-assigned port, global capacity limits, GPU/RAM admission control.
- Bounded request queue и per-key/model rate limits.
- Расширенный audit payload: request_id, latency, backend_id, hashes, token/byte counts, trusted proxy policy.
- Model integrity manifest: sha256/size/license/source/arch validation перед стартом backend.
- Автоматическая генерация SBOM/license bundle/signatures/checksums для release artifacts.
