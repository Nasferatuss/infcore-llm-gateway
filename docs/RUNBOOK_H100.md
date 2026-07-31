# infcore — step-by-step bring-up on an H100 server (terminal)

A copy-paste sequence from bare server to a working HTTPS endpoint, for
bare-metal/VM with systemd. Everything runs over SSH.

* **Why** it is done this way — [DEPLOY_H100_PROD.md](DEPLOY_H100_PROD.md).
* **Reference** for the config, ports, audit — [DEPLOY.md](DEPLOY.md).
* The Docker path — `deploy/compose/docker-compose.yml` (needs a private registry).

Notation: `$` — as a regular user, `#` — as root (`sudo -i`).

> **Server has no GPU yet?** The whole setup can be exercised on CPU — see
> [Appendix A](#appendix-a--cpu-dry-run-no-gpu) at the end. The GPU only affects
> generation speed; the config, keys, RBAC, audit, TLS, rotation and metrics are
> verified in full.

---

## 0. Check the server (before anything else)

```sh
nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv
#   expected: NVIDIA H100 ..., 81559 MiB, driver 535+

nvcc --version | tail -1      # the CUDA toolkit is only needed for the BUILD
cmake --version | head -1     # >= 3.21 required
nproc; free -g; df -h /opt /var/log
```

Disk needed: ~30 GB for the build + the size of the models in
`/opt/infcore/models` + headroom for the audit log in `/var/log`.

> If `nvidia-smi` does not respond, there is no point going further — fix the
> driver first.

---

## 1. Code

```sh
$ git clone --depth 1 https://github.com/ggml-org/llama.cpp engine-src
$ git clone --depth 1 https://github.com/Nasferatuss/infcore-llm-gateway engine-src/infcore
$ cd engine-src
```

infcore builds as a layer on top of the llama.cpp tree: the engine comes from
upstream, the layer from this repository. The engine base commit the layer was
verified against is pinned in `NOTICE`.

The tree contains **all** of the llama.cpp source plus the `infcore/` directory;
the engine in it is byte-identical to upstream (the "wrap, don't touch the core"
rule). Nothing needs to be brought in separately — the tree is self-contained.

`--depth 1` saves time and disk: the build does not need history. **But** merging
upstream updates (`infcore/scripts/update-upstream.sh`) needs a full clone — do
that on a build/development machine, not on the production server.

In a closed enclave — from an internal mirror or an archive; same layout.

---

## 2. Building for the H100

**Do not take the build command from `TEST_PLAN_WSL.md`** — it sets `sm_89`
(RTX 4070); on an H100 such a binary carries no CUDA kernels.

```sh
$ cmake -S infcore -B build -C infcore/cmake/profile-h100.cmake
$ cmake --build build -j"$(nproc)"
```

Takes tens of minutes. Artifacts: `build/bin/{infcore_gateway,llama-server,infcore-cli}`.

**Always verify that Hopper is inside:**

```sh
$ cuobjdump build/bin/llama-server | grep -oE 'arch = sm_[0-9]+' | sort -u
# expected, exactly: arch = sm_90
$ ldd build/bin/infcore_gateway | grep -i 'not found' && echo "BROKEN" || echo "dependencies ok"
```

---

## 3. User and directories

```sh
# useradd -r -s /usr/sbin/nologin infcore
# install -d -o infcore -g infcore /opt/infcore/bin /opt/infcore/config /opt/infcore/models
# install -d -o root -g root -m 0750 /etc/infcore
```

Do **not** create `/var/log/infcore` — systemd does it (`LogsDirectory=`).

Binaries:

```sh
# install -o root -g root -m 0755 \
    build/bin/infcore_gateway build/bin/llama-server build/bin/infcore-cli \
    /opt/infcore/bin/
```

---

## 4. Models

### Small one first

**Do not start with a production model of tens of GB.** Run the whole path
(steps 5–11) on a ~1–2 GB model: the build, config, systemd, TLS, audit and
metrics get verified in minutes. A config mistake is better found before a
multi-hour upload, not after.

### How to deliver

**A. rsync from your machine** (the server needs no internet):

```sh
$ rsync -avP <local>.gguf user@server:~/staging/
```

`scp` is not suitable: it cannot resume, and a multi-gigabyte transfer breaks
easily. `-P` = `--partial --progress`; after an interruption, repeat **the same
command** — it continues where it left off. Run it under `tmux`/`screen`, or a
dropped SSH session kills the transfer.

Rough timing: 27 GB ≈ 216 Gbit → at 100 Mbit/s upload ~36 min, at 20 Mbit/s ~3
hours. A home uplink is asymmetric — that is what you will hit.

**B. Download directly on the server** — if it has internet access, the DC's
pipe is many times faster than your uplink. A one-off outbound window does not
conflict with `enforce_no_egress` and `IPAddressDeny=any`: those constrain the
gateway, not an administrator's `curl`.

**C.** In a fully closed enclave — an internal mirror or physical media.

### Install and verify

```sh
$ df -h /opt                       # enough space? model size + headroom
# install -o infcore -g infcore -m 0440 ~/staging/*.gguf /opt/infcore/models/
$ sha256sum /opt/infcore/models/*.gguf
$ stat -c '%n %s' /opt/infcore/models/*.gguf     # size_bytes
```

The separate `install` as root is needed because `/opt/infcore/models` belongs
to `infcore` and the SSH user cannot write there.

> **Compare the sha256 with the source.** A matching size does **not** prove
> integrity: an interrupted rsync leaves a file of the right length. You need
> the hash for the config anyway (step 6), so this step is unavoidable.

---

## 5. API keys

```sh
# umask 077
# cat > /etc/infcore/gateway.env <<EOF
INFCORE_KEY_ADMIN=$(openssl rand -hex 32)
EOF
# chown root:infcore /etc/infcore/gateway.env
# chmod 0640 /etc/infcore/gateway.env
```

The gateway rejects placeholders and weak values at startup — generate keys
exactly like this. Look at your key once (`cat`) and put it in your secrets
store.

---

## 6. Config

```sh
# cp infcore/config/gateway.yaml /opt/infcore/config/gateway.yaml
# chown root:infcore /opt/infcore/config/gateway.yaml && chmod 0640 /opt/infcore/config/gateway.yaml
# ${EDITOR:-vi} /opt/infcore/config/gateway.yaml
```

The file in the repository is an **example**, not a production config. The edits
mandatory for an H100:

| Field | In the example | Set to | Why |
|---|---|---|---|
| `models[].n_gpu_layers` | `999` | **`-1`** | `-1` = do not pass the flag; llama.cpp fits the offload itself. Any explicit number disables the auto-fit. |
| `models[].gguf_path` | example | the real path | — |
| `models[].sha256`, `size_bytes` | absent | from step 4 | nothing to verify without them |
| `offline.require_model_integrity` | `false` | **`true`** | otherwise swapped weights go unnoticed |
| `server.trusted_proxies` | absent | **`["127.0.0.1"]`** | Angie is on the same host; without this **every** audit record gets the proxy's `client_ip` |
| extra `models[]` | 3 models | keep what you need | set `"enabled": false` on the rest |
| `security.principals` | 3 of them | match your keys | delete the extras along with their roles |

`n_ctx` and `runtime.max_loaded_models` — to your workload: 80 GB of VRAM allows
both a large context and several models at once.

Validate the config **before** installing the service (the gateway validates it
against a built-in JSON Schema and, on error, prints every problem and refuses
to start):

```sh
# set -a; . /etc/infcore/gateway.env; set +a
# /opt/infcore/bin/infcore_gateway /opt/infcore/config/gateway.yaml
# Ctrl+C after a successful start
```

---

## 7. systemd

```sh
# cp infcore/deploy/systemd/infcore-gateway.service /etc/systemd/system/
# systemctl daemon-reload
# systemctl enable --now infcore-gateway
# systemctl status infcore-gateway --no-pager
```

> The unit sets `IPAddressDeny=any` + `IPAddressAllow=localhost`. For the
> "Angie on the same host" topology that is enough. If clients reach the gateway
> directly, without the proxy — add their subnet to `IPAddressAllow`, or there
> will be no traffic.

### Smoke test

```sh
$ KEY=$(sudo sed -n 's/^INFCORE_KEY_ADMIN=//p' /etc/infcore/gateway.env)
$ curl -s localhost:8080/health
$ curl -s localhost:8080/v1/models -H "Authorization: Bearer $KEY"
$ curl -s localhost:8080/v1/chat/completions -H "Authorization: Bearer $KEY" \
    -H 'Content-Type: application/json' \
    -d '{"model":"<logical_name>","messages":[{"role":"user","content":"2+2?"}]}'
```

The first request starts the backend and verifies the sha256 — tens of seconds
on a large model; that is normal. Check that the model landed in VRAM:

```sh
$ nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv
```

If it is empty, the model is running on CPU — check `n_gpu_layers` and the unit
journal.

---

## 8. TLS

The certificate comes from an **internal CA**: ACME/Let's Encrypt is impossible
in an offline enclave.

```sh
# install -d -m 0750 /etc/angie/tls
# install -m 0644 infcore.crt /etc/angie/tls/infcore.crt   # fullchain
# install -m 0600 infcore.key /etc/angie/tls/infcore.key
# chown -R root:angie /etc/angie/tls

# cp infcore/deploy/angie/infcore.conf /etc/angie/http.d/
# ${EDITOR:-vi} /etc/angie/http.d/infcore.conf
```

Adjust two places: `server_name` (must match the certificate's CN/SAN) and the
`allow` in `location = /metrics` (the monitoring subnet).

```sh
# angie -t && systemctl reload angie
$ curl -sI http://<FQDN>/            # expect a 308 to https
$ curl -s https://<FQDN>/health --cacert /path/to/internal-ca.crt
$ curl -s https://<FQDN>/metrics --cacert ... -o /dev/null -w '%{http_code}\n'   # expect 403
```

Verify that the audit log sees the **real** client, not the proxy:

```sh
# tail -1 /var/log/infcore/audit.log | grep -o '"client_ip":"[^"]*"'
```

If it shows `127.0.0.1` for a request from another machine — `trusted_proxies`
is not set (step 6).

---

## 9. Log rotation

```sh
# cp infcore/deploy/logrotate/infcore /etc/logrotate.d/infcore
# logrotate -d /etc/logrotate.d/infcore     # dry-run, no errors
# logrotate -f /etc/logrotate.d/infcore     # forced run
$ curl -s localhost:8080/metrics | grep audit_reopen
#   infcore_gateway_audit_reopens_total          has grown
#   infcore_gateway_audit_reopen_failures_total  = 0
```

If `reopens_total` does not grow, `postrotate` is not reaching the gateway, and
the log keeps being written to the renamed file. That is silent audit loss — fix
it immediately.

---

## 10. Monitoring

```sh
# cp infcore/deploy/monitoring/scrape.yml /etc/vm/scrape.yml
# cp infcore/deploy/monitoring/alerts.yml /etc/vm/alerts.yml
```

In `scrape.yml`, set the gateway's address on the internal network. Do not
expose `/metrics` externally: it is served **without authorization**.

**The alert thresholds must be calibrated** — they were written for a 35B model
with partial offload onto 8 GB. On an H100 the `p95 > 30s` threshold will never
fire. Take a baseline:

```sh
$ python3 infcore/tests/load/loadtest.py --url http://127.0.0.1:8080 \
    --key-file <key file> --model <logical_name> --concurrency 8 --requests 32
```

and edit `alerts.yml` from the measurements — do not disable the alerts.

---

## 11. Operations

```sh
# systemctl status infcore-gateway
# journalctl -u infcore-gateway -f
# systemctl restart infcore-gateway     # after editing the config/keys
# systemctl reload  infcore-gateway     # ONLY reopens the log; the config is NOT re-read
$ curl -s localhost:8080/metrics | grep -E 'audit_failed|backends_loaded'
```

`infcore_gateway_audit_failed 1` = the log writer has failed and the gateway is
cutting **all** traffic (fail-closed; usually ENOSPC on `/var/log`). That is an
outage, not a warning.

### Key changes — restart only

There is no hot config reload. Zero-downtime, in two passes:

1. add a new principal with the new key, keep the old one → `restart`;
2. move the clients over; watch `errors_total{type="unauthorized"}`;
3. once there are no spikes — remove the old principal → `restart`.

---

## 12. Pre-production checklist

- [ ] `cuobjdump` → `arch = sm_90`
- [ ] `n_gpu_layers: -1`; after the first request the model is visible in `nvidia-smi`
- [ ] `require_model_integrity: true`, sha256/size_bytes set
- [ ] `trusted_proxies` populated → real client IPs in the audit log
- [ ] HTTP redirects to HTTPS; `/metrics` unreachable externally (403)
- [ ] `logrotate -f` → `audit_reopens_total` grew, `reopen_failures_total` = 0
- [ ] baseline taken, alert thresholds adjusted
- [ ] alerts reach the on-call engineer (test `InfcoreAuditFailed`)

---

## Appendix A — CPU dry run (no GPU)

While there is no GPU, the whole setup can be walked through on an ordinary
server. **The GPU only affects generation speed.** The config, keys, RBAC,
audit, systemd hardening, TLS, `trusted_proxies`, rotation, metrics and alerts
are verified in full — and break in the same places they would break on an H100.

### What changes

| Step | On CPU |
|---|---|
| 0. GPU check | **skip** — no `nvidia-smi` to check |
| 2. Build | `-C infcore/cmake/profile-cpu.cmake`; the CUDA toolkit is **not needed** |
| 2. `cuobjdump` | **skip** — no CUDA kernels by definition |
| 4. Model | small only (~1–2 GB) |
| 6. `n_gpu_layers` | **keep `-1`** — no change needed, see below |
| 7. `nvidia-smi` after the request | skip |

```sh
$ cmake -S infcore -B build -C infcore/cmake/profile-cpu.cmake
$ cmake --build build -j"$(nproc)"
```

The profile exists for the sake of one line — `LLAMA_BUILD_SERVER=ON`. When
embedded via `add_subdirectory`, `llama-server` is **not built** by default, and
a bare `cmake -DGGML_CUDA=OFF ...` produces a gateway with no backend: it
starts, passes the health check, and answers **502 to every request** to a model.

### Why `n_gpu_layers: -1` needs no change

`-1` = "do not pass the flag to the backend". In a CPU build there are no GPU
layers at all; llama.cpp simply computes everything on CPU. The same config
later moves to the H100 without a single edit.

### Small model only

Without a GPU **all** the weights live in RAM: a 25 GB production model needs
25+ GB of free memory (`free -g`), and the speed will be single-digit tokens per
second. For verifying the setup that adds nothing over what a 1–2 GB model shows.

For the Docker path: `mem_limit` on CPU = the **whole** model size plus KV-cache
headroom, not "model minus VRAM" as the `docker-compose.yml` comment puts it.

In other words: the CPU run covers the enclave and the scaffolding, but not the
engine on the target hardware.
