# infcore — deployment guide (for DevOps)

A gateway to local LLMs in an offline enclave. This document covers the build,
configuration and startup. The runtime is strictly offline (zero egress);
internet access is only needed at build time.

> **Production on an H100 server:** the `sm_90` build, TLS, audit-log rotation
> and monitoring are covered separately in [DEPLOY_H100_PROD.md](DEPLOY_H100_PROD.md).

## 1. Components

After the build, `build/bin/` contains three binaries:

| Binary | Purpose |
|---|---|
| `infcore_gateway` | the main service: OpenAI-compatible gateway, auth, RBAC, audit, auto-supervision of models |
| `llama-server` | the inference engine (from llama.cpp); brought up by the gateway on demand |
| `infcore-cli` | a terminal client (diagnostics, model management) |

## 2. Build (on the target server)

The build profile fixes the component set and backends (CUDA+Vulkan on, the
rest off):

```sh
cmake -S infcore -B build -C infcore/cmake/profile-portable.cmake
cmake --build build -j"$(nproc)"
# artifacts: build/bin/{infcore_gateway,llama-server,infcore-cli}
```
The profile builds a static artifact (`BUILD_SHARED_LIBS=OFF`), so the
Docker/systemd runtime does not depend on stray `libllama.so`/`libggml*.so`
next to the binaries. The `ldd` check for "not found" runs in the Dockerfile
and in GitHub Actions.

Build-stage requirements: CMake >= 3.21, a C++17 compiler, the CUDA toolkit
and/or the Vulkan SDK from an internal package mirror. A GPU is not needed at
build time.

Docker image: `infcore/deploy/docker/Dockerfile` (context = root of the engine
tree). Base images are built ahead of time from the skeletons in
`infcore/deploy/docker/base/`.

### GPU at runtime
By default the profile builds `llama-server` for CUDA+Vulkan, so the runtime
needs:
- **NVIDIA:** the host driver + `nvidia-container-toolkit` (for Docker, pass
  through `--gpus`/`deploy.resources.devices`), and in the runtime base image,
  the CUDA runtime libraries (`libcudart` etc.) from an internal mirror.
- **Vulkan:** the Vulkan ICD/loader package in the runtime base image.

The runtime base image MUST contain these libraries, or `llama-server` will not
start (the gateway returns `502 backend start failed`). Base-image composition
and the recipe skeletons: `infcore/deploy/docker/base/`.

For a **CPU-only** enclave, rebuild without GPU support. IMPORTANT: when
embedded via `add_subdirectory`, `llama-server` is NOT built by default — the
server and tools have to be enabled explicitly (the `profile-portable.cmake`
profile does this; a bare command does not):
```sh
cmake -S infcore -B build -DGGML_CUDA=OFF -DGGML_VULKAN=OFF \
      -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_TOOLS=ON \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```
Without `-DLLAMA_BUILD_SERVER=ON` only `infcore_gateway` gets built and
`llama-server` does not — the gateway starts, but every request to a managed
model returns 502.

> Note: `cmake -S .` (configuring the ROOT of the engine tree, not `infcore/`)
> requires `-DLLAMA_BUILD_APP=OFF` — the `app/` directory was removed during the
> compliance cleanup, and upstream enables it by default in a standalone build.
> The standard paths (`-S infcore` / the profile) are unaffected.

## 3. Configuration

The single config file is `gateway.yaml` (JSON format). At startup it is
**validated against a built-in JSON Schema**; on error the service refuses to
start and prints every problem.

Key sections:
- `server` — host/port (default `127.0.0.1:8080`), `max_concurrent_requests`,
  `request_timeout_ms` (default 120000). NOTE: this timeout also applies as the
  read-timeout to the backend. For non-streaming requests the response arrives
  as a single chunk at the end of generation, so long generations (>120 s) on
  weak hardware will be cut off with a 502 — raise `request_timeout_ms` for your
  worst case, or use `stream:true`.
  - `max_concurrent_requests` (default 64) = the worker pool size and the
    ceiling on concurrent requests. An SSE stream holds a worker for the
    entire stream, so the value must exceed the expected number of concurrent
    streams (+ headroom for `/health`, `/metrics`, `/admin`).
  - **Perimeter limits** (protecting a public/semi-trusted perimeter):
    `read_timeout_ms` (30000) cuts off a slow request send (slowloris);
    `write_timeout_ms` (120000) frees the worker if an SSE consumer has "died"
    and stopped reading; `max_body_bytes` (8 MiB) rejects oversized bodies
    before buffering -> no OOM on `json::parse`. A body over the limit gets
    `413`. For a purely loopback deployment, the defaults can be left as-is.
- `security` — principals (key -> subject/role), roles (allowlists), audit.
- `runtime` — `llama_server_bin` (path to llama-server), `port_range_start`
  (ports for managed models), admission controls (`max_loaded_models`,
  `max_parallel_starts`, `rate_limit_per_minute`), idle/startup timeouts.
- `models` — the model catalog (logical_name, gguf_path, modality, n_ctx,
  n_gpu_layers, `mmproj_path`). For managed models of modality `vision`, the
  `mmproj_path` field (the mtmd projector) is MANDATORY — otherwise the gateway
  fails fast at startup. For `rerank`, the gateway starts the backend with
  `llama-server --reranking` and proxies `/v1/rerank`, `/v1/reranking`,
  `/rerank`, `/reranking`.
- `offline.require_model_integrity` — a release gate for models. When `true`,
  every managed model must declare `sha256` and `size_bytes`; the gateway
  checks that the file is a regular file, its permissions (not writable by
  group/other), its size and its SHA-256 before starting. If the gate is off,
  any declared `sha256`/`size_bytes` are still checked.

In Docker the gateway must listen on `0.0.0.0` (only `127.0.0.1:8080` is
published externally). Do not edit the mounted read-only config — set the
**`INFCORE_HOST`** / **`INFCORE_PORT`** environment variables instead (they
take priority over `server.host`/`server.port`). `docker-compose.yml` already
sets `INFCORE_HOST=0.0.0.0`. Managed `llama-server` instances still stay on
loopback and behind a per-boot key, so this is safe.

### Audit is mandatory (fail-fast)
With `security.audit.sink="file"` and `audit.require=true` (the default) the
gateway **refuses to start** if the log fails to open (missing directory/
permissions) — so that traffic is never served with audit silently disabled.
Make sure `audit.path` (default `/var/log/infcore/audit.log`) is writable by
the service user:
- **systemd** creates the directory itself (`LogsDirectory=infcore`, owned by
  `User=`);
- **Docker/compose**: the mounted `/var/log/infcore` must be owned by the uid
  of the `infcore` user from the image — `chown` on the host before the first
  run, or the container fails to start (loudly, not silently).

To deliberately lift the requirement (not for the enclave): `audit.require=false`.

### Secrets (API keys)
Do not store keys in plaintext. The `api_key` field supports:
- `"env:INFCORE_KEY_ADMIN"` — value from an environment variable;
- `"file:/run/secrets/admin_key"` — value from a file.

For systemd, variables are set in `/etc/infcore/gateway.env`; for compose, in
`gateway.env` (see `deploy/compose/gateway.env.example`). A missing
variable/file is a fatal startup error (fail-fast). Weak keys and common
placeholders (`change-me`, `REPLACE_ME`, `secret`, strings shorter than 24
characters) are also rejected at startup.

## 4. Models

- **Managed** (no `backend_url`): the gateway brings up `llama-server` itself
  on the first request and shuts it down on idle. Requires
  `runtime.llama_server_bin` and `gguf_path`.
- **External** (`backend_url` set): the gateway only proxies to an
  already-running server.

Weights (`.gguf`) are not baked into the image — they are mounted as a volume
(`/opt/infcore/models`, read-only). The path in `gguf_path` must match the
mount point.

For a final release, fill in the model's provenance fields:
- `sha256`, `size_bytes` — artifact integrity control;
- `license`, `source` — the model's origin and license, for the release manifest;
- `mmproj_sha256`, `mmproj_size_bytes` — the same, for the vision projector, if any.

## 5. Ports

| Port | Purpose | External access |
|---|---|---|
| 8080 | HTTP API (`/v1/*`, `/admin/*`, `/health`, `/metrics`) | via reverse proxy (Angie/TLS) |
| 8100+ | managed `llama-server` instances (one per model) | **localhost-only + a per-boot API key** |

`/health` and `/metrics` are reachable without authorization (for the
orchestrator and metrics scraping). `/metrics` uses the Prometheus format on
the same port 8080, scraped by VictoriaMetrics on the DevOps side. The path and
whether it's enabled are controlled by `observability.metrics_path` and
`observability.metrics_enabled`. There is no separate metrics port.

Managed `llama-server` instances listen only on `127.0.0.1` and require a
per-boot `--api-key` (generated by the gateway at startup). They cannot be
reached bypassing RBAC/audit, even if the gateway itself listens on `0.0.0.0`.

## 6. Startup

### systemd (recommended for bare-metal/VM)
```sh
# 1. service user and directories (once, as root):
useradd -r -s /usr/sbin/nologin infcore
install -d -o infcore -g infcore /opt/infcore/bin /opt/infcore/config
# 2. put the binaries in /opt/infcore/bin, the config in /opt/infcore/config/gateway.yaml
#    (systemd creates the log directory /var/log/infcore itself — LogsDirectory=)
# 3. secret keys go into /etc/infcore/gateway.env (see "Secrets" above)
cp infcore/deploy/systemd/infcore-gateway.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now infcore-gateway
```
The unit uses `KillMode=control-group` — on stop/restart ALL child
`llama-server` processes are killed too (no orphans). This is mandatory: do not
change it.

### Docker Compose
```sh
cd infcore/deploy/compose
cp gateway.env.example gateway.env     # fill in the secrets
docker compose up -d
```

## 7. Audit log

Written append-only to `audit.sink=file` at `audit.path` (default
`/var/log/infcore/audit.log`), JSONL format, with every event flushed to disk
(`fsync`) before the request completes — no event loss even on power failure.
Writing is offloaded to a dedicated writer thread with **group-commit**:
concurrent requests share a single `fsync`, so a flood (including 401/403
rejections) does not serialize on disk and become a DoS vector. The directory
must be writable by the service user. Every event carries `request_id`,
`subject`, `role`, `endpoint`, `model`, `model_sha256`, `backend_id`,
`client_ip`, `decision`, `status`; proxy requests additionally record
`latency_ms`, request/response byte counts, and token usage if the backend
returned OpenAI-compatible `usage`.

For immutability (an enclave requirement), at the OS level:
```sh
chattr +a /var/log/infcore/audit.log     # append-only, no overwrite/delete
```

**Rotation is not directly compatible with `chattr +a`** (neither
`copytruncate` nor renaming/deleting an append-only file works), and
`copytruncate` is incorrect regardless: the gateway holds an open fd with
`O_APPEND` and keeps writing through it. Pick one scheme:

- **With immutability (recommended for the enclave):** rotate by clearing the
  flag and restarting the service (control-group kill closes the fd cleanly):
  ```
  /var/log/infcore/audit.log {
      weekly
      rotate 52
      missingok
      prerotate  /usr/bin/chattr -a /var/log/infcore/audit.log
      postrotate systemctl restart infcore-gateway   # reopens the log; chattr +a is set by the service wrapper / on the next prerotate cycle
      endscript
  }
  ```
  Or skip regular rotation and archive by date with an external log collector
  (auditd/SIEM) instead.
- **Without immutability:** ordinary logrotate with
  `postrotate systemctl restart infcore-gateway` (NOT `copytruncate`).

## 8. Smoke test

```sh
export INFCORE_URL=http://127.0.0.1:8080 INFCORE_KEY=<admin key>
/opt/infcore/bin/infcore-cli models                 # list models
/opt/infcore/bin/infcore-cli chat -m <model> "test"  # verify inference
curl -s $INFCORE_URL/health                          # {"status":"ok",...}
```

## 9. Updating the engine

Updates to upstream llama.cpp are pulled by
`infcore/scripts/update-upstream.sh` (by release tag). The `infcore/` layer
does not conflict during this. After updating — rebuild and run the smoke test.

## 9.1 Release manifest

After the build, generate verifiable release artifacts:

```sh
./infcore/scripts/release-manifest.sh build dist/infcore
# optional detached signatures:
INFCORE_SIGN=1 ./infcore/scripts/release-manifest.sh build dist/infcore
```

The script checks for the presence of `infcore_gateway`, `infcore-cli`,
`llama-server`, the licenses and the SBOM, then writes
`release-manifest.json` and `SHA256SUMS`. With `INFCORE_SIGN=1` it also creates
detached GPG signatures.

## 10. Offline invariant

At runtime the service makes no outbound connections beyond the enclave.
Important: the `llama-server` engine (upstream, not edited per the
wrap-not-touch strategy) CONTAINS network-fetch code (the `-hf`/`--model-url`
flags, fetching images by URL). The gateway never passes such arguments, but
the invariant is enforced at the infrastructure level:

- **systemd:** `IPAddressDeny=any` + `IPAddressAllow=localhost` in the unit
  (add the client subnet to the allow list);
- **Docker:** the `internal: true` network (no outbound access);
- **config:** `offline.enforce_no_egress: true` — at startup the gateway
  rejects external `backend_url` values that do not point at loopback/RFC1918;
- for strict requirements — nftables/firewall egress-deny on the host.

The mechanism is verified by the `infcore/tests/egress` test (netns). In CI it
starts a real `infcore_gateway` with a managed fake backend, makes the first
request through the lazy supervisor, verifies the absence of `INFCORE_KEY_*` in
the backend process's environment, and confirms there is no route out. Any
outbound internet access is a requirements violation.
