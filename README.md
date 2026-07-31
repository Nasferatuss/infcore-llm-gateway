# infcore — an OpenAI-compatible gateway for local LLM inference

[![CI](https://github.com/Nasferatuss/infcore-llm-gateway/actions/workflows/ci.yml/badge.svg)](https://github.com/Nasferatuss/infcore-llm-gateway/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A production layer on top of [llama.cpp](https://github.com/ggml-org/llama.cpp):
multi-model routing, lazy backend supervision, authn/RBAC, audit logging and
Prometheus metrics — behind a single OpenAI-compatible API, fully offline.

Built **on top of** llama.cpp (ggml authors, MIT) without editing the engine.
See [NOTICE](NOTICE).

## The problem

`llama-server` serves one model per process, with no authentication, no
authorization, no audit trail and no multi-tenancy. Running it as real
infrastructure means answering questions it does not answer on its own:

- **One endpoint, many models.** Clients want to send `"model": "..."` and get
  routed, not to memorise a port per model.
- **Memory is finite.** Keeping every model resident does not scale; starting
  them by hand does not either.
- **Who called what?** Regulated and air-gapped deployments need an audit trail
  of principal, model, endpoint, status and outcome — including refusals.
- **The engine is a moving target.** Patching `llama.cpp` in place makes every
  upstream update a merge conflict.

infcore answers these as a wrapper, so that upstream stays a drop-in dependency.

## Architecture

```
┌────────────────────────────────────────────────────────────────┐
│  infcore/sdk (Python client) · infcore-cli                     │  clients
├────────────────────────────────────────────────────────────────┤
│  gateway: OpenAI surface + routing + policy                    │
│    authn / RBAC / audit  (security/)                           │  infcore
│    model registry (multi-model)  ·  lazy supervisor            │
├──────────── runtime boundary: HTTP to loopback llama-server ───┤
│  llama-server (OpenAI-compatible, SSE) · libllama · ggml       │  upstream
│    backends: cpu / cuda / vulkan — binds 127.0.0.1 only        │
└────────────────────────────────────────────────────────────────┘
```

**Request path.** `/v1/chat/completions` → `authn` (API key, constant-time
compare) → `rbac` (role → allowed models and endpoints) → `routing` (model name
→ registry → backend, started on demand) → proxy with SSE when
`stream: true` → `audit` + `metrics`.

**Lazy supervisor.** Managed models start on first request and stop when idle.
Ports are assigned under a lock, failed starts back off and release the port,
liveness uses `waitpid(WNOHANG)` so dead backends are detected and no zombies
accumulate. The in-flight counter is RAII-bound, so a client abort cannot leak it.

**Backend isolation.** Managed `llama-server` processes bind `127.0.0.1` only
and start with a per-boot random `--api-key`; hitting their ports directly
without that key returns 401. The gateway injects it when proxying.

**Streaming correctness.** Upstream status is checked *before* the stream is
committed: a non-2xx comes back as a normal JSON error in OpenAI shape rather
than an error buried inside a `200` SSE body.

**Offline invariant.** No outbound traffic at runtime. llama.cpp itself contains
download paths (`common/download.cpp`, `-hf`/`--model-url`, remote image fetch);
under wrap-not-touch these are not deleted but *neutralised* — the supervisor
never passes download-triggering arguments, egress is blocked at the
infrastructure layer (systemd `IPAddressDeny`, Docker `internal: true`), and
`enforce_no_egress` restricts external `backend_url` values to loopback/RFC1918.

**Operational surface.** TLS termination and rate limiting via Angie/nginx,
`systemd` units with hardening options, Docker Compose deployment, log rotation
with `SIGHUP` reopen for the audit journal, and pull-style metrics on `/metrics`
(`request_duration_seconds` histogram, per-model counters) for
Prometheus/VictoriaMetrics.

## Quickstart (CPU profile)

The CPU profile builds without a CUDA toolkit or Vulkan SDK — useful for
exercising the full control plane (config, keys, RBAC, audit, TLS, rotation,
metrics) on a machine with no GPU.

```sh
# 1. Engine tree + this layer inside it as infcore/
git clone --depth 1 https://github.com/ggml-org/llama.cpp engine-src
git clone --depth 1 https://github.com/Nasferatuss/infcore-llm-gateway \
    engine-src/infcore
cd engine-src

# 2. Configure with the GPU-less profile and build
cmake -S infcore -B build -C infcore/cmake/profile-cpu.cmake
cmake --build build -j"$(nproc)"

# 3. Point the registry at a local GGUF, then run the gateway
$EDITOR infcore/config/gateway.yaml
./build/bin/infcore_gateway --config infcore/config/gateway.yaml
```

```sh
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'Authorization: Bearer <your-api-key>' \
  -H 'Content-Type: application/json' \
  -d '{"model":"<logical-name>","messages":[{"role":"user","content":"ping"}]}'
```

Config is validated against `config/schema/gateway.schema.json` at startup —
an invalid config fails fast rather than half-starting.

Build profiles: `profile-cpu.cmake` (no GPU), `profile-h100.cmake` (`sm_90`),
`profile-rf.cmake` (cpu + cuda + vulkan).

## Testing

- `tests/unit/` — ctest: RBAC, authn, JSON-Schema validation, config parsing,
  supervisor token failure.
- `tests/egress/` — asserts zero outbound traffic in a network namespace, plus a
  product smoke test.
- `tests/load/loadtest.py` — concurrency harness reporting latency
  percentiles (p50/p95/p99), TTFT, throughput and an error breakdown.

## Relationship to llama.cpp

This repository contains **only** the infcore layer. The engine is not vendored
here: infcore builds as a subtree inside a llama.cpp checkout and talks to it
over HTTP through separate `llama-server` processes.

Engine files are never edited. Upstream is taken as a release tag
(`scripts/update-upstream.sh`), which keeps the boundary clean, the SBOM
accurate and updates drop-in.

## Licensing

infcore is MIT — see [LICENSE](LICENSE).

llama.cpp / ggml is MIT, Copyright (c) 2023-2026 The ggml authors. Full
attribution, the pinned upstream base commit and the licence texts of every
bundled dependency are in [NOTICE](NOTICE), `THIRD_PARTY_LICENSES/` and
`sbom.cdx.json` (CycloneDX 1.5).

## Documentation

Detailed docs are in Russian under `docs/`:

| File | Contents |
|---|---|
| `ARCHITECTURE.md` | layers, process topology, request flow, offline invariants |
| `DEPLOY.md` | deployment, Docker, systemd |
| `DEPLOY_H100_PROD.md` | production H100: `sm_90` build, TLS, rotation, monitoring |
| `RUNBOOK_H100_RU.md` | step-by-step server bring-up |
| `TEST_PLAN_WSL_RU.md` | bring-up on a consumer GPU under WSL2 |
| `COMPLIANCE.md` | licence attribution and provenance |
