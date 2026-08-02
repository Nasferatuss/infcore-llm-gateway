"""infcore Python SDK — a client for the gateway (offline deployment).

A thin client for the OpenAI-compatible REST gateway. Standard library only (urllib), with
no external networking dependencies, so it works in an offline deployment as-is.

Example:
    from infcore import Client, GenerationParams
    c = Client("http://127.0.0.1:8080", api_key="...")
    print(c.chat("qwen3-moe-a3b", [{"role": "user", "content": "hello"}]))
    for tok in c.chat_stream("qwen3-moe-a3b", [...]):
        print(tok, end="", flush=True)
    vecs = c.embeddings("bge-m3-embed", ["text 1", "text 2"])
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Iterator

__all__ = ["Client", "GenerationParams", "InfcoreError"]


class InfcoreError(RuntimeError):
    """A gateway error (non-2xx) or a transport error. status is None for transport errors."""

    def __init__(self, message: str, status: int | None = None):
        super().__init__(message)
        self.status = status


@dataclass
class GenerationParams:
    max_tokens: int = 512
    temperature: float = 0.7
    top_p: float = 0.95
    stop: list[str] = field(default_factory=list)

    def to_body(self) -> dict:
        b: dict = {
            "max_tokens": self.max_tokens,
            "temperature": self.temperature,
            "top_p": self.top_p,
        }
        if self.stop:
            b["stop"] = self.stop
        return b


class Client:
    """Client for the infcore gateway (OpenAI-compatible)."""

    def __init__(self, base_url: str, api_key: str | None = None, timeout: float = 120.0):
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.timeout = timeout
        # Offline/internal client: ignore HTTP(S)_PROXY from the ambient shell.
        self._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    # --- internals -----------------------------------------------------------
    def _request(self, path: str, body: dict, stream: bool):
        data = json.dumps(body).encode("utf-8")
        headers = {"Content-Type": "application/json"}
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        req = urllib.request.Request(self.base_url + path, data=data, headers=headers, method="POST")
        try:
            resp = self._opener.open(req, timeout=self.timeout)  # noqa: S310 (internal network)
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")
            msg = detail
            try:
                msg = json.loads(detail).get("error", {}).get("message", detail)
            except Exception:  # pragma: no cover - the body is not JSON
                pass
            raise InfcoreError(f"gateway {e.code}: {msg}", status=e.code) from None
        except urllib.error.URLError as e:  # pragma: no cover - network unreachable
            raise InfcoreError(f"gateway unreachable: {e.reason}") from None
        return resp

    def models(self) -> list[dict]:
        """GET /v1/models — the models available to this role."""
        headers = {}
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        req = urllib.request.Request(self.base_url + "/v1/models", headers=headers, method="GET")
        try:
            resp = self._opener.open(req, timeout=self.timeout)  # noqa: S310
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")
            raise InfcoreError(f"gateway {e.code}: {detail}", status=e.code) from None
        except urllib.error.URLError as e:  # pragma: no cover - network unreachable
            raise InfcoreError(f"gateway unreachable: {e.reason}") from None
        return json.loads(resp.read().decode("utf-8")).get("data", [])

    def chat(self, model: str, messages: list[dict], params: GenerationParams | None = None) -> str:
        """Non-streaming chat. Returns the text of the first choice."""
        body = {"model": model, "messages": messages, "stream": False}
        if params:
            body.update(params.to_body())
        resp = self._request("/v1/chat/completions", body, stream=False)
        j = json.loads(resp.read().decode("utf-8"))
        return j["choices"][0]["message"]["content"]

    def chat_stream(self, model: str, messages: list[dict],
                    params: GenerationParams | None = None) -> Iterator[str]:
        """Streaming chat (SSE). Yields content deltas as they arrive."""
        body = {"model": model, "messages": messages, "stream": True}
        if params:
            body.update(params.to_body())
        resp = self._request("/v1/chat/completions", body, stream=True)
        for raw in resp:
            line = raw.decode("utf-8").strip()
            if not line or not line.startswith("data:"):
                continue
            payload = line[len("data:"):].strip()
            if payload == "[DONE]":
                break
            try:
                chunk = json.loads(payload)
            except json.JSONDecodeError:  # pragma: no cover
                continue
            if "error" in chunk:
                err = chunk.get("error") or {}
                raise InfcoreError(err.get("message", "stream error"))
            delta = chunk.get("choices", [{}])[0].get("delta", {}).get("content")
            if delta:
                yield delta

    def embeddings(self, model: str, inputs: list[str]) -> list[list[float]]:
        """POST /v1/embeddings — vectors for a list of strings (order is preserved)."""
        body = {"model": model, "input": inputs}
        resp = self._request("/v1/embeddings", body, stream=False)
        j = json.loads(resp.read().decode("utf-8"))
        return [item["embedding"] for item in j["data"]]

    def rerank(self, model: str, query: str, documents: list[str], top_n: int | None = None) -> dict:
        """POST /v1/rerank — returns the gateway/upstream reranker's response."""
        body: dict = {"model": model, "query": query, "documents": documents}
        if top_n is not None:
            body["top_n"] = top_n
        resp = self._request("/v1/rerank", body, stream=False)
        return json.loads(resp.read().decode("utf-8"))
