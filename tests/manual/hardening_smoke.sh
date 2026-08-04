#!/usr/bin/env bash
# infcore — a manual hardening smoke test for the gateway. It exercises the production fixes
# against a FAKE backend (no real models needed):
#   M5  — request body limit -> 413
#   B3  — mu_ is not held during SIGTERM->SIGKILL (the gateway stays responsive while stopping)
#   F1  — a disable issued during backend start-up is not lost (502 + the backend is stopped)
#   F2  — a runtime audit failure -> fail closed (503) when audit.require=true
#
# Usage:
#   infcore/tests/manual/hardening_smoke.sh [path_to_infcore_gateway]
# If no binary is passed, ./build/bin/infcore_gateway is used (build it with:
#   cmake -S infcore -B build -DGGML_CUDA=OFF -DGGML_VULKAN=OFF -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --target infcore_gateway -j)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
GW="${1:-./build/bin/infcore_gateway}"
FAKE="$HERE/fake_llama_server.py"
RLIMIT="$HERE/rlimit_exec.py"
# Long enough, and containing none of the placeholder substrings config.cpp
# rejects. A short key here means the gateway refuses the config and exits
# before any check runs — see wait_up below for why that used to look like
# ordinary test failures.
KEY="hardening-smoke-fixed-key-0123456789"
PASS=0; FAIL=0
TMP="$(mktemp -d)"; PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done
            pkill -9 -f "fake_llama_server.py" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

if [ ! -x "$GW" ]; then echo "infcore_gateway not found: $GW"; exit 2; fi
ok(){ echo "  PASS: $1"; PASS=$((PASS+1)); }
no(){ echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
now(){ python3 -c 'import time;print(time.time())'; }

# Wait for the gateway to actually serve, and abort loudly if it never does.
# Without this, a gateway that refuses its config and exits at start-up produces
# a run where every curl gets a connection refused: most checks report FAIL for
# the wrong reason, and B3 reports PASS — because "/health answered in 0.01s" is
# satisfied by a refused connection just as well as by a fast reply. A test that
# goes green while nothing is running is worse than one that goes red.
# $1=port $2=file holding the gateway's output
wait_up(){
  local i
  for i in $(seq 1 60); do
    curl -sf -m1 -o /dev/null "http://127.0.0.1:$1/health" && return 0
    kill -0 "$GWPID" 2>/dev/null || break        # it died — no point waiting out the loop
    sleep 0.25
  done
  echo "  FATAL: the gateway never served /health on :$1. Its output:"
  sed 's/^/    | /' "$2" 2>/dev/null | tail -20
  FAIL=$((FAIL+1))
  echo; echo "=== SUMMARY: PASS=$PASS FAIL=$FAIL (aborted) ==="
  exit 1
}

# llama_server_bin wrappers: the supervisor calls them with --host/--port/--model..., and we
# append the fake's test-specific flags.
mk_wrap(){ # $1=name $2=extra flags
  local f="$TMP/llama_$1.sh"
  printf '#!/bin/sh\nexec python3 %q %s "$@"\n' "$FAKE" "$2" > "$f"; chmod +x "$f"; echo "$f"
}
mk_cfg(){ # stdin=json -> file
  local f="$TMP/$1.json"; cat > "$f"; echo "$f"
}

echo "=== infcore hardening smoke ==="; echo "gateway: $GW"; echo

# ---------- M5: body limit -> 413 ----------
echo "[M5] request body size limit"
CFG=$(mk_cfg m5 <<EOF
{"server":{"host":"127.0.0.1","port":18090,"max_body_bytes":1024},
 "security":{"rbac_enabled":true,
   "principals":[{"api_key":"$KEY","subject":"a","role":"admin"}],
   "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}],
   "audit":{"sink":"none","path":"/tmp/x","require":false}},
 "runtime":{"llama_server_bin":"/bin/true"},
 "models":[{"logical_name":"a","gguf_path":"/tmp/a.gguf"}]}
EOF
)
"$GW" "$CFG" > "$TMP/m5.log" 2>&1 & GWPID=$!; PIDS+=($GWPID); wait_up 18090 "$TMP/m5.log"
BIG=$(python3 -c 'print("x"*5000)')
c=$(curl -s -o /dev/null -w "%{http_code}" -m5 http://127.0.0.1:18090/v1/chat/completions \
      -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" \
      -d "{\"model\":\"a\",\"pad\":\"$BIG\"}")
[ "$c" = "413" ] && ok "body >1KB -> 413" || no "expected 413, got $c"
kill "$GWPID" 2>/dev/null; sleep 1; echo

# ---------- B3: the gateway stays responsive while a backend is stopping ----------
echo "[B3] mu_ is not held during SIGTERM->SIGKILL"
W=$(mk_wrap b3 "--ignore-sigterm")   # the backend ignores SIGTERM -> 5 s until SIGKILL
CFG=$(mk_cfg b3 <<EOF
{"server":{"host":"127.0.0.1","port":18091,"max_concurrent_requests":16},
 "security":{"rbac_enabled":true,
   "principals":[{"api_key":"$KEY","subject":"a","role":"admin"}],
   "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}],
   "audit":{"sink":"none","path":"/tmp/x","require":false}},
 "runtime":{"llama_server_bin":"$W","port_range_start":18200,"idle_timeout_ms":600000,"startup_timeout_ms":15000},
 "models":[{"logical_name":"a","gguf_path":"/tmp/a.gguf"},
           {"logical_name":"b","gguf_path":"/tmp/b.gguf"}]}
EOF
)
"$GW" "$CFG" > "$TMP/b3.log" 2>&1 & GWPID=$!; PIDS+=($GWPID); wait_up 18091 "$TMP/b3.log"
curl -s -m20 http://127.0.0.1:18091/v1/chat/completions -H "Authorization: Bearer $KEY" \
  -H "Content-Type: application/json" -d '{"model":"a","messages":[]}' >/dev/null   # bring backendA up
( curl -s -m10 -X POST http://127.0.0.1:18091/admin/models/a/disable -H "Authorization: Bearer $KEY" >/dev/null ) & DPID=$!
sleep 0.3
# -f, and the exit status is checked: the point is that /health *answers*
# quickly, not that curl returns quickly. A refused connection is also fast.
t0=$(now); curl -sf -m8 -o /dev/null http://127.0.0.1:18091/health; hrc=$?; t1=$(now)
hl=$(python3 -c "print(f'{$t1-$t0:.2f}')")
if [ "$hrc" -ne 0 ]; then
  no "/health did not answer at all during the shutdown (curl exit $hrc)"
elif python3 -c "import sys;sys.exit(0 if $hl < 2.0 else 1)"; then
  ok "/health answers in ${hl}s during the 5 s shutdown (not blocked)"
else
  no "/health waited ${hl}s — mu_ is held during the kill"
fi
wait "$DPID"; kill "$GWPID" 2>/dev/null; sleep 6; echo   # let the gateway finish the SIGKILL

# ---------- F1: a disable during Starting is not lost ----------
echo "[F1] disable during backend start-up"
W=$(mk_wrap f1 "--ready-delay 3")    # the backend "loads" for 3 s -> a wide Starting window
CFG=$(mk_cfg f1 <<EOF
{"server":{"host":"127.0.0.1","port":18092},
 "security":{"rbac_enabled":true,
   "principals":[{"api_key":"$KEY","subject":"a","role":"admin"}],
   "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}],
   "audit":{"sink":"none","path":"/tmp/x","require":false}},
 "runtime":{"llama_server_bin":"$W","port_range_start":18210,"idle_timeout_ms":600000,"startup_timeout_ms":15000},
 "models":[{"logical_name":"a","gguf_path":"/tmp/a.gguf"}]}
EOF
)
"$GW" "$CFG" > "$TMP/f1.log" 2>&1 & GWPID=$!; PIDS+=($GWPID); wait_up 18092 "$TMP/f1.log"
( curl -s -m20 http://127.0.0.1:18092/v1/chat/completions -H "Authorization: Bearer $KEY" \
    -H "Content-Type: application/json" -d '{"model":"a","messages":[]}' > "$TMP/f1.out" 2>&1 ) & RPID=$!
sleep 1.5   # backendA is in Starting
curl -s -m5 -X POST http://127.0.0.1:18092/admin/models/a/disable -H "Authorization: Bearer $KEY" >/dev/null
wait "$RPID"
grep -q "disabled while starting" "$TMP/f1.out" \
  && ok "the initiating request got 502 (the disable was honoured)" \
  || no "expected 502 'disabled while starting', got: $(cat "$TMP/f1.out")"
sleep 1
if pgrep -f "fake_llama_server.py" >/dev/null; then no "the backend is still alive — the disable was lost"; else ok "the backend was stopped (it did not survive to the idle timeout)"; fi
kill "$GWPID" 2>/dev/null; sleep 1; echo

# ---------- F2: a runtime audit failure -> fail closed ----------
echo "[F2] fail closed on an audit failure (audit.require=true)"
AUD="$TMP/audit.log"
CFG=$(mk_cfg f2 <<EOF
{"server":{"host":"127.0.0.1","port":18093},
 "security":{"rbac_enabled":true,
   "principals":[{"api_key":"$KEY","subject":"a","role":"admin"}],
   "roles":[{"name":"admin","allow_models":["*"],"allow_endpoints":["*"]}],
   "audit":{"sink":"file","path":"$AUD","require":true}},
 "runtime":{"llama_server_bin":"/bin/true"},
 "models":[{"logical_name":"a","gguf_path":"/tmp/a.gguf"}]}
EOF
)
python3 "$RLIMIT" 3000 "$GW" "$CFG" >/dev/null 2>"$TMP/f2.err" & GWPID=$!; PIDS+=($GWPID)
wait_up 18093 "$TMP/f2.err"
prev=""; saw503=""
for i in $(seq 1 80); do
  c=$(curl -s -o /dev/null -w "%{http_code}" -m3 http://127.0.0.1:18093/v1/models -H "Authorization: Bearer WRONG")
  [ "$c" = "503" ] && { saw503=1; break; }
done
[ -n "$saw503" ] && ok "after the audit failure requests -> 503 (fail closed)" || no "503 never happened"
h=$(curl -s -m3 http://127.0.0.1:18093/health)
echo "$h" | grep -q '"audit":"failed"' && ok "/health reflects the degradation: $h" || no "/health did not report failed: $h"
grep -qa "infcore: CRITICAL" "$TMP/f2.err" && ok "loud stderr about the audit failure" || no "no 'infcore: CRITICAL' in stderr"
kill "$GWPID" 2>/dev/null; echo

echo "=== SUMMARY: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ]
