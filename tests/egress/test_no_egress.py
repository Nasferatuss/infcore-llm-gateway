"""Egress test: the deployment's offline invariant.

If INFCORE_GATEWAY_BIN is set, the test runs a real gateway inside `unshare -rn`, issues a
request to a managed model through the fake llama-server, and checks that the child backend
does not inherit the INFCORE_KEY_* secrets. Without the binary it falls back to a preflight
check of the netns mechanism itself, for dev environments.
"""
import os
import shutil
import subprocess
import sys
import textwrap

import pytest

_NETNS_PREFLIGHT = textwrap.dedent(
    """
    import socket, subprocess, sys
    subprocess.run(["ip", "link", "set", "lo", "up"], check=False)
    # 1) loopback must work (internal traffic)
    srv = socket.socket(); srv.bind(("127.0.0.1", 0)); srv.listen(1)
    port = srv.getsockname()[1]
    c = socket.socket()
    try:
        c.settimeout(2); c.connect(("127.0.0.1", port))
    except Exception as e:
        print("LOOPBACK_FAIL", e); sys.exit(2)
    finally:
        c.close(); srv.close()
    # 2) an external address must be unreachable (no route out == zero egress)
    ext = socket.socket()
    try:
        ext.settimeout(2); ext.connect(("8.8.8.8", 53))
        print("EGRESS_LEAK"); sys.exit(3)
    except OSError:
        pass
    finally:
        ext.close()
    print("OK"); sys.exit(0)
    """
)

_PRODUCT_CHILD = textwrap.dedent(
    r"""
    import json, os, socket, subprocess, sys, tempfile, time, urllib.request, urllib.error

    subprocess.run(["ip", "link", "set", "lo", "up"], check=False)
    gateway = os.environ["INFCORE_GATEWAY_BIN"]
    fake = os.environ["INFCORE_FAKE_LLAMA_BIN"]
    key = "0123456789abcdef01234567"
    tmp = tempfile.mkdtemp(prefix="infcore-egress-")
    env_dump = os.path.join(tmp, "backend.envdump")
    audit = os.path.join(tmp, "audit.log")
    cfg = {
        "server": {"host": "127.0.0.1", "port": 18080, "request_timeout_ms": 10000,
                   "read_timeout_ms": 5000, "write_timeout_ms": 10000, "max_body_bytes": 1048576},
        "security": {"rbac_enabled": True,
            "principals": [{"api_key": "env:INFCORE_KEY_ADMIN", "subject": "ci", "role": "admin"}],
            "roles": [{"name": "admin", "allow_models": ["*"], "allow_endpoints": ["*"]}],
            "audit": {"sink": "file", "path": audit, "require": True}},
        "observability": {"metrics_enabled": True, "metrics_path": "/metrics"},
        "offline": {"enforce_no_egress": True},
        "runtime": {"llama_server_bin": fake, "port_range_start": 18100,
                    "startup_timeout_ms": 10000, "idle_timeout_ms": 60000},
        "models": [{"logical_name": "m", "gguf_path": env_dump, "modality": "text", "n_ctx": 512}]
    }
    cfg_path = os.path.join(tmp, "gateway.json")
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(cfg, f)

    env = os.environ.copy()
    env["INFCORE_KEY_ADMIN"] = key
    p = subprocess.Popen([gateway, cfg_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, env=env)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                with opener.open("http://127.0.0.1:18080/health", timeout=1) as r:
                    if r.status == 200:
                        break
            except Exception:
                time.sleep(0.2)
        else:
            raise RuntimeError("gateway did not become healthy")

        req = urllib.request.Request(
            "http://127.0.0.1:18080/v1/chat/completions",
            data=json.dumps({"model": "m", "messages": [{"role": "user", "content": "ping"}]}).encode(),
            headers={"Content-Type": "application/json", "Authorization": "Bearer " + key},
            method="POST")
        with opener.open(req, timeout=15) as r:
            body = r.read().decode("utf-8")
            assert r.status == 200, body

        deadline = time.time() + 5
        while time.time() < deadline and not os.path.exists(env_dump):
            time.sleep(0.1)
        assert os.path.exists(env_dump), "backend env dump was not written"
        dumped = open(env_dump, encoding="utf-8").read()
        assert "INFCORE_KEY_ADMIN=" not in dumped, dumped
        assert "INFCORE_KEY_" not in dumped, dumped

        ext = socket.socket()
        try:
            ext.settimeout(2); ext.connect(("8.8.8.8", 53))
            raise AssertionError("EGRESS_LEAK")
        except OSError:
            pass
        finally:
            ext.close()
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait(timeout=5)
    print("OK"); sys.exit(0)
    """
)


def _netns_available():
    if sys.platform != "linux":
        return False
    if not shutil.which("unshare") or not shutil.which("ip"):
        return False
    try:
        r = subprocess.run(["unshare", "-rn", "true"], capture_output=True, timeout=10)
        return r.returncode == 0
    except Exception:
        return False


@pytest.mark.egress
def test_runtime_has_zero_egress():
    if not _netns_available():
        pytest.skip("requires Linux + unshare -rn (unprivileged netns) + iproute2")
    child = _NETNS_PREFLIGHT
    env = None
    gateway = os.environ.get("INFCORE_GATEWAY_BIN")
    if gateway:
        fake = os.environ.get(
            "INFCORE_FAKE_LLAMA_BIN",
            os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "manual", "fake_llama_server.py")),
        )
        child = _PRODUCT_CHILD
        env = os.environ.copy()
        env["INFCORE_GATEWAY_BIN"] = gateway
        env["INFCORE_FAKE_LLAMA_BIN"] = fake
    r = subprocess.run(
        ["unshare", "-rn", "python3", "-c", child],
        capture_output=True, text=True, timeout=45, env=env,
    )
    assert r.returncode == 0, (
        f"offline invariant violated: rc={r.returncode} out={r.stdout!r} err={r.stderr!r}"
    )
