"""End-to-end acceptance test: real inference through a RUNNING gateway.

It needs a running gateway and a real model, so it is SKIPped by default. Run it on the
target host after deployment (the acceptance step in AUDIT §7: load + inference +
multimodality + embeddings). Weights are not kept in the repository, so this test cannot be
self-contained.

Environment variables:
  INFCORE_URL          base URL of the gateway (e.g. http://127.0.0.1:8080)  [required]
  INFCORE_KEY          API key (Bearer)                                      [required]
  INFCORE_E2E_MODEL    logical_name of a text model for chat                 [required]
  INFCORE_E2E_EMBED    logical_name of an embedding model (optional)
  INFCORE_E2E_RERANK   logical_name of a rerank model (optional)
  INFCORE_E2E_VISION   logical_name of a vision model + INFCORE_E2E_IMAGE (data URL/URL) (optional)

Example:
  INFCORE_URL=http://127.0.0.1:8080 INFCORE_KEY=$ADMIN INFCORE_E2E_MODEL=qwen3-moe-a3b \
    pytest infcore/tests/e2e -v
"""
import os
import sys

import pytest

# use our own SDK (dogfooding); the path to sdk/python is relative to this file
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "sdk", "python"))

URL = os.environ.get("INFCORE_URL")
KEY = os.environ.get("INFCORE_KEY")
MODEL = os.environ.get("INFCORE_E2E_MODEL")

pytestmark = pytest.mark.skipif(
    not (URL and KEY and MODEL),
    reason="e2e: set INFCORE_URL, INFCORE_KEY, INFCORE_E2E_MODEL (a running gateway is required)",
)


@pytest.fixture(scope="module")
def client():
    from infcore import Client
    return Client(URL, api_key=KEY, timeout=float(os.environ.get("INFCORE_E2E_TIMEOUT", "180")))


def test_models_lists_target(client):
    ids = [m["id"] for m in client.models()]
    assert MODEL in ids, f"model {MODEL} is not visible to the key's role: {ids}"


def test_chat_nonstream(client):
    out = client.chat(MODEL, [{"role": "user", "content": "Answer in one word: what is the capital of France?"}])
    assert isinstance(out, str) and out.strip(), "empty chat response"


def test_chat_stream(client):
    chunks = list(client.chat_stream(MODEL, [{"role": "user", "content": "Count from 1 to 5."}]))
    assert chunks, "the stream yielded no deltas"
    assert "".join(chunks).strip(), "the stream was empty"


def test_embeddings():
    embed_model = os.environ.get("INFCORE_E2E_EMBED")
    if not embed_model:
        pytest.skip("INFCORE_E2E_EMBED is not set")
    from infcore import Client
    c = Client(URL, api_key=KEY)
    vecs = c.embeddings(embed_model, ["first text", "second text"])
    assert len(vecs) == 2 and all(len(v) > 0 for v in vecs), "unexpected embeddings shape"


def test_rerank():
    rerank_model = os.environ.get("INFCORE_E2E_RERANK")
    if not rerank_model:
        pytest.skip("INFCORE_E2E_RERANK is not set")
    from infcore import Client
    c = Client(URL, api_key=KEY)
    out = c.rerank(rerank_model, "capital of France", ["Paris", "Berlin"])
    assert out.get("results"), "rerank returned no results"


def test_vision_chat():
    vision_model = os.environ.get("INFCORE_E2E_VISION")
    image = os.environ.get("INFCORE_E2E_IMAGE")
    if not (vision_model and image):
        pytest.skip("INFCORE_E2E_VISION / INFCORE_E2E_IMAGE are not set")
    from infcore import Client
    c = Client(URL, api_key=KEY)
    out = c.chat(vision_model, [{
        "role": "user",
        "content": [
            {"type": "text", "text": "What is in the image? Briefly."},
            {"type": "image_url", "image_url": {"url": image}},
        ],
    }])
    assert isinstance(out, str) and out.strip(), "empty vision chat response"
