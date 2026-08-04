#!/usr/bin/env python3
"""infcore gateway — load run.

Drives N concurrent chat sessions against the gateway and prints latency (p50/p95/p99),
throughput and an error breakdown. It exists to tell "works for one request" apart from
"works under load": backend queueing, timeouts, degradation and OOM only show up here.

Standard library only — in an offline deployment there is nowhere to install
requests/httpx from.

Example:
    python3 loadtest.py --url http://127.0.0.1:8080 --key-file admin.key \
        --model qwen35 --concurrency 8 --requests 32 --max-tokens 64
"""

import argparse
import json
import statistics
import sys
import threading
import time
import urllib.error
import urllib.request
from collections import Counter
from concurrent.futures import ThreadPoolExecutor


def parse_args():
    p = argparse.ArgumentParser(description="infcore gateway load test")
    p.add_argument("--url", default="http://127.0.0.1:8080")
    p.add_argument("--key", help="API key (or --key-file)")
    p.add_argument("--key-file", help="file containing the API key")
    p.add_argument("--model", required=True)
    p.add_argument("--concurrency", type=int, default=4)
    p.add_argument("--requests", type=int, default=16)
    p.add_argument("--max-tokens", type=int, default=64)
    p.add_argument("--timeout", type=float, default=600.0)
    p.add_argument("--prompt-tokens", type=int, default=0,
                   help="pad the prompt to roughly N tokens (long-context test)")
    p.add_argument("--stream", action="store_true", help="measure TTFT in streaming mode")
    return p.parse_args()


def resolve_key(args):
    if args.key_file:
        with open(args.key_file) as f:
            return f.read().strip()
    if args.key:
        return args.key
    sys.exit("--key or --key-file is required")


def build_prompt(target_tokens):
    base = "Answer briefly, in one sentence: why does a model need an integrity check?"
    if target_tokens <= 0:
        return base
    # The filler is an ASCII word with a leading space: with BPE vocabularies that is a
    # steady ~1 token per repeat. Non-Latin scripts do not work here — a short Cyrillic
    # phrase expands to roughly a dozen tokens, and the request silently overruns n_ctx.
    filler = " data" * target_tokens
    return f"Ignore this filler:{filler}\n\n{base}"


class Stats:
    """Thread-safe result collection."""

    def __init__(self):
        self.lock = threading.Lock()
        self.latencies = []
        self.ttfts = []
        self.errors = Counter()
        self.ok = 0
        self.completion_tokens = 0

    def record_ok(self, latency, ttft, tokens):
        with self.lock:
            self.latencies.append(latency)
            if ttft is not None:
                self.ttfts.append(ttft)
            self.ok += 1
            self.completion_tokens += tokens

    def record_err(self, kind):
        with self.lock:
            self.errors[kind] += 1


def one_request(args, key, prompt, stats):
    body = {
        "model": args.model,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": args.max_tokens,
        "stream": bool(args.stream),
    }
    req = urllib.request.Request(
        f"{args.url}/v1/chat/completions",
        data=json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
        method="POST",
    )
    started = time.perf_counter()
    ttft = None
    tokens = 0
    try:
        with urllib.request.urlopen(req, timeout=args.timeout) as resp:
            if args.stream:
                for raw in resp:
                    line = raw.decode("utf-8", "replace").strip()
                    if not line.startswith("data: "):
                        continue
                    if ttft is None:
                        ttft = time.perf_counter() - started
                    if line[6:] == "[DONE]":
                        break
                    tokens += 1
            else:
                payload = json.loads(resp.read())
                usage = payload.get("usage") or {}
                tokens = usage.get("completion_tokens", 0)
        stats.record_ok(time.perf_counter() - started, ttft, tokens)
    except urllib.error.HTTPError as e:
        stats.record_err(f"HTTP {e.code}")
    except urllib.error.URLError as e:
        stats.record_err(f"URLError {e.reason}")
    except TimeoutError:
        stats.record_err("timeout")
    except Exception as e:  # dropped connection, malformed JSON, and so on
        stats.record_err(type(e).__name__)


def pct(values, q):
    if not values:
        return float("nan")
    return statistics.quantiles(values, n=100)[q - 1] if len(values) > 1 else values[0]


def main():
    args = parse_args()
    key = resolve_key(args)
    prompt = build_prompt(args.prompt_tokens)
    stats = Stats()

    print(f"url={args.url} model={args.model} concurrency={args.concurrency} "
          f"requests={args.requests} max_tokens={args.max_tokens} "
          f"prompt_tokens~{args.prompt_tokens} stream={args.stream}", flush=True)

    wall_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        for _ in range(args.requests):
            pool.submit(one_request, args, key, prompt, stats)
    wall = time.perf_counter() - wall_start

    lat = sorted(stats.latencies)
    total = args.requests
    print("\n=== RESULT ===")
    print(f"succeeded:      {stats.ok}/{total}")
    print(f"failed:         {sum(stats.errors.values())}")
    for kind, n in stats.errors.most_common():
        print(f"  - {kind}: {n}")
    print(f"wall:           {wall:.1f} s")
    print(f"throughput:     {stats.ok / wall:.2f} req/s")
    if stats.completion_tokens:
        print(f"total tokens:   {stats.completion_tokens} "
              f"({stats.completion_tokens / wall:.1f} tok/s aggregate)")
    if lat:
        print(f"latency:        min={lat[0]:.2f} p50={pct(lat, 50):.2f} "
              f"p95={pct(lat, 95):.2f} p99={pct(lat, 99):.2f} max={lat[-1]:.2f} (s)")
    if stats.ttfts:
        t = sorted(stats.ttfts)
        print(f"TTFT:           p50={pct(t, 50):.2f} p95={pct(t, 95):.2f} (s)")

    # The load run counts as failed on any error: under the expected load the gateway is
    # supposed to answer, not fall over with timeouts or 5xx.
    return 1 if stats.errors else 0


if __name__ == "__main__":
    sys.exit(main())
