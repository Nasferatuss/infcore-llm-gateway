# infcore — production deployment on an H100 server

Companion to [DEPLOY.md](DEPLOY.md): the H100 build, TLS, log rotation,
monitoring. The shared material (components, models, ports, smoke test) lives there.

> Need a copy-paste, step-by-step sequence from bare server to HTTPS —
> [RUNBOOK_H100.md](RUNBOOK_H100.md). This document is the **why**; that one is
> the **what to type**.

---

## 1. Building for the H100

**The H100 is Hopper, `sm_90`.** Not `sm_89`.

```bash
cmake -S infcore -B build -C infcore/cmake/profile-h100.cmake
cmake --build build -j"$(nproc)"
```

> ⚠️ **Do not copy the build command from [TEST_PLAN_WSL.md](TEST_PLAN_WSL.md).**
> That test plan was written for an RTX 4070 Laptop and sets
> `-DCMAKE_CUDA_ARCHITECTURES=89` (Ada). A binary built that way **carries no
> CUDA kernels for the H100**.

Verify that the binary really contains `sm_90`:

```bash
cuobjdump build/bin/llama-server | grep -oE 'arch = sm_[0-9]+' | sort -u
# expected: arch = sm_90
```

`profile-h100.cmake` differs from `profile-portable.cmake` only in building a
single architecture (`90` instead of `75;80;86;89;90`) and turning Vulkan off.
If your GPU fleet is mixed, use `profile-portable.cmake` — it covers the H100 too.

### n_gpu_layers on an H100

A 25 GiB model fits entirely into 80 GiB of VRAM, so:

```json
{ "n_gpu_layers": -1 }
```

`-1` = do not pass the flag to the backend; llama.cpp fits the offload to free
VRAM by itself. Any explicit value **disables** that auto-fit
(`n_gpu_layers already set by user ... abort`), and on a GPU where the model
does not fit entirely, manual tuning loses to the auto mode almost two-fold.

---

## 2. TLS

TLS is terminated at the reverse proxy (Angie/nginx), not in the gateway:

* certificate renewal = proxy `reload`, not a gateway restart. A restart means
  unloading the model and re-verifying its sha256 — tens of seconds of downtime
  on a 25 GiB model;
* the TLS stack stays out of the gateway's own security-critical path;
* the gateway remains plain HTTP on loopback and is never published externally.

Config: [`deploy/angie/infcore.conf`](../deploy/angie/infcore.conf) →
`/etc/angie/http.d/`. The certificate comes from an **internal CA**:
ACME/Let's Encrypt is impossible in an offline enclave.

```bash
angie -t && systemctl reload angie
```

### ⚠️ trusted_proxies — mandatory

The gateway records the connection peer's IP in the audit log. Behind a proxy
the peer is always **the proxy itself**, so without this setting **every** audit
record carries the proxy's `client_ip`, and the audit trail loses its "from
where" dimension:

```json
"server": { "trusted_proxies": ["10.0.0.0/8"] }
```

Only for requests from these addresses is `client_ip` taken from `X-Real-IP` /
`X-Forwarded-For`. Everyone else's headers are not trusted — otherwise any
client could forge its IP in the audit log with one extra header. An empty list
(the default) = trust no one.

The **rightmost** element of `X-Forwarded-For` is used: nginx/Angie append their
peer on the right, while the left-hand elements came from the client and are
trivially forged.

---

## 3. Audit log rotation

Config: [`deploy/logrotate/infcore`](../deploy/logrotate/infcore) → `/etc/logrotate.d/infcore`.

The gateway holds the log fd open for the lifetime of the process, so the
rotation mode matters:

| Mode | Outcome |
|---|---|
| `copytruncate` | ❌ events are lost between the copy and the truncate |
| `create` without a signal | ❌ the gateway keeps writing to the renamed file; the new one stays empty — the audit trail silently drains into the archive |
| **`create` + SIGHUP** | ✅ the only loss-free option |

On SIGHUP the gateway reopens the log at its original path. The signal arrives
from `postrotate` via `systemctl reload` → `ExecReload=/bin/kill -HUP $MAINPID`.

The reopen happens before the next event is written: if there are no events,
there is nothing to lose.

Verify that rotation actually lands:

```bash
curl -s localhost:8080/metrics | grep audit_reopen
# infcore_gateway_audit_reopens_total         must be growing
# infcore_gateway_audit_reopen_failures_total must be 0
```

> Without rotation the log grows until it hits the file-size limit, at which
> point audit goes **fail-closed** and the gateway stops serving traffic. That
> is a matter of "when", not "if".

---

## 4. API key rotation

Keys are supplied via `env:VAR` and read **at startup**. There is no hot config
reload: `SIGHUP` only reopens the log.

Zero-downtime procedure (two keys active at once):

1. add a new principal with the new key to the config, keeping the old one;
2. `systemctl restart infcore-gateway` (the downtime = model load + sha256);
3. move clients to the new key; watch
   `infcore_gateway_errors_total{type="unauthorized"}` — a spike means someone
   is still on the old one;
4. once there are no spikes — remove the old principal and `restart` again.

Who used which key is visible in the audit log via `subject`.

---

## 5. Monitoring

* scrape: [`deploy/monitoring/scrape.yml`](../deploy/monitoring/scrape.yml)
* alerts: [`deploy/monitoring/alerts.yml`](../deploy/monitoring/alerts.yml)

> ⚠️ The gateway serves `/metrics` **without authorization** (it is designed for
> loopback). Never expose it externally: error and endpoint counters are
> operational intelligence. `angie/infcore.conf` puts an allow-list on
> `/metrics`; adjust it to your network.

What is scraped:

| Metric | Why |
|---|---|
| `requests_total{endpoint,decision}` | traffic and policy decisions |
| `request_duration_seconds` (histogram) | p95/p99; degradation under load |
| `audit_failed` (gauge) | **1 = the gateway is cutting all traffic fail-closed** |
| `audit_reopens_total` / `audit_reopen_failures_total` | whether rotation works |
| `models_configured`, `backends_loaded` (gauge) | whether the backend came up |
| `errors_total{type}` | 401, backend_*, rate_limited, etc. |

**The thresholds in `alerts.yml` need calibrating to your hardware.** A p95
threshold of 30 s makes sense for a large model with partial offload; on an H100
with the model fully in VRAM it should be several times lower. Take a baseline:

```bash
python3 infcore/tests/load/loadtest.py --url http://127.0.0.1:8080 \
    --key-file admin.key --model <model> --concurrency 8 --requests 32
```

and adjust the thresholds — do not disable the alerts.

---

## 6. Pre-production checklist

- [ ] `cuobjdump` shows `arch = sm_90`
- [ ] `n_gpu_layers: -1`; after the first request `nvidia-smi` shows the model in VRAM
- [ ] `server.trusted_proxies` is populated → the audit log shows real client IPs, not the proxy
- [ ] `angie -t` passes; HTTP redirects to HTTPS; certificate is from the internal CA
- [ ] `/metrics` is **not** reachable externally, and is reachable from the monitoring network
- [ ] `require_model_integrity: true`, sha256 and size_bytes are set;
      negatively tested (corrupt sha256 → the gateway refuses to start)
- [ ] a load run has produced a baseline; alert thresholds are calibrated
- [ ] alerts reach the on-call engineer (test `InfcoreAuditFailed` — that one is an outage)
