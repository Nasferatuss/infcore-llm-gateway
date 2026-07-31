# infcore — manual gateway hardening tests

Verify production fixes against a **fake** backend (no real models/GPU
needed). Complement the automated `tests/unit` (ctest) and `tests/egress`.

## Contents
- `hardening_smoke.sh` — runs 4 checks, prints PASS/FAIL, exits non-zero on
  failure:
  - **M5** — a request body over the limit → `413`;
  - **B3** — the supervisor's `mu_` is not held during `SIGTERM→SIGKILL`
    (`/health` responds instantly while an "unkillable" backend is still being
    reaped);
  - **F1** — a `disable` during backend startup is not lost (the initiator
    gets `502`, the backend is shut down, it does not linger to the idle
    timeout);
  - **F2** — a runtime audit failure (disk full) → fail-closed `503` when
    `audit.require=true`, `/health` = `degraded`, a loud stderr message.
- `fake_llama_server.py` — a fake llama-server (`--ready-delay`,
  `--ignore-sigterm` flags).
- `rlimit_exec.py` — runs a process under `RLIMIT_FSIZE` (a deterministic
  simulation of "disk full").

## Running
```sh
# 1. Build the gateway (CPU-only is enough):
cmake -S infcore -B build -DGGML_CUDA=OFF -DGGML_VULKAN=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --target infcore_gateway -j

# 2. Run:
infcore/tests/manual/hardening_smoke.sh ./build/bin/infcore_gateway
```
Requires `python3`, `curl`, `bash`. Expected result: `PASS=7 FAIL=0`.

The remaining blockers are verified differently: **M1** (egress bypass) — in
`tests/unit` (`ctest -R infcore_unit`); **M3** (audit durability) — by stress
load (concurrent requests against a running gateway, checked against the
number of lines in the log).
