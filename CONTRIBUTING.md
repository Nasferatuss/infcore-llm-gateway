# Contributing to infcore

Thanks for looking. This document covers the two things that trip people up
first — infcore does not build on its own, and it is not allowed to edit the
engine — and then the ordinary mechanics.

## The one rule: wrap, don't touch

infcore sits **on top of** [llama.cpp](https://github.com/ggml-org/llama.cpp)
without modifying it. Upstream is a drop-in dependency, and it stays that way.

- **Never edit a file outside `infcore/`.** If a change seems to require
  patching the engine, it belongs upstream, not here. Open an issue describing
  what the engine cannot currently do and we will work out the wrapper-side
  answer or send a PR to llama.cpp.
- The boundary is the C API in `llama.h` and the HTTP surface of a child
  `llama-server`. The gateway does not link `libllama` at all — backends are
  separate processes.
- A PR that touches upstream files will be asked to split, no matter how good
  the change is. Keeping this boundary is why upstream updates are a version
  bump instead of a merge conflict.

## Building

infcore is a subtree of a llama.cpp checkout. Building it standalone does not
work — CMake needs the engine tree as its parent.

```bash
git clone --depth 1 https://github.com/ggml-org/llama.cpp engine-src
git clone https://github.com/Nasferatuss/infcore-llm-gateway engine-src/infcore
cd engine-src
cmake -S infcore -B build -C infcore/cmake/profile-cpu.cmake
cmake --build build -j"$(nproc)"
```

Build profiles live in `cmake/`: `profile-cpu.cmake` (no GPU, the one CI uses),
`profile-h100.cmake` (CUDA production), `profile-portable.cmake`. Pick one with
`-C`; they are cache-init files, not toolchain files.

Needs CMake ≥ 3.21, a C++17 compiler, OpenSSL headers, Ninja.

## Tests

```bash
ctest --test-dir build --output-on-failure          # unit tests
bash infcore/tests/manual/hardening_smoke.sh ./build/bin/infcore_gateway
python3 -m pytest infcore/tests/egress                # offline invariant
```

The hardening smoke exercises the production fixes — body-size limit, gateway
responsiveness during a backend `SIGTERM`→`SIGKILL`, a disable issued while a
backend is still starting, and fail-closed behaviour when the audit sink breaks.
It runs against a **fake** backend (`tests/manual/fake_llama_server.py`), so it
needs no models and no GPU. If you change the supervisor, the security layer or
anything that touches shutdown, run it.

## What a good PR looks like

- **One concern per PR.** A 200-file PR does not get reviewed, it gets closed.
- **Say what breaks if it is wrong.** The commit body is the place for the
  reasoning, the alternative you rejected, and the thing you are unsure about.
  "Fix bug" tells a future reader nothing.
- **Tests that would have failed before.** For a concurrency or shutdown change,
  that usually means a case in the hardening smoke rather than a unit test.
- **Keep it offline.** Nothing in the runtime path may reach the network. See
  the offline invariant in the README; `tests/egress/` enforces it.
- **English in source and commit messages.**
- **If you used an AI assistant, say so in the PR description** and confirm you
  have read and tested the result yourself. Assisted code is welcome; unreviewed
  generated code is not.

## Security issues

Do not open a public issue. See [SECURITY.md](SECURITY.md).

## Licence

MIT, same as the project — see [LICENSE](LICENSE). By contributing you agree
your work ships under it. Third-party components and their licences are tracked
in [NOTICE](NOTICE), `THIRD_PARTY_LICENSES/` and `sbom.cdx.json`; a new
dependency has to land in all three.
