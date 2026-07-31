# Compliance: licenses and provenance

This document records correct use of open-source software (license
attribution) and the deployment condition.

## Deployment condition
infcore is deployed **on-premises, on a local computer/server**, strictly
offline (zero egress at runtime). No mandatory dependency on external
infrastructure.

## Provenance (must be disclosed)
infcore **is built on top of the open-source llama.cpp (ggml authors, MIT)**.
- ❌ Not okay: calling the whole product "fully original work" or "built from
  scratch".
- ✅ Correct: "built on the open-source llama.cpp/ggml (MIT); original work is
  the infcore/ layer (gateway/security/SDK) and the build/delivery for a local
  offline enclave".

## Licenses
| Component | License | Where |
|---|---|---|
| ggml / llama.cpp (engine) | MIT (The ggml authors) | `THIRD_PARTY_LICENSES/ggml-llama.cpp.txt` |
| nlohmann/json | MIT | `THIRD_PARTY_LICENSES/nlohmann-json.txt` |
| cpp-httplib / stb / miniaudio / sheredom | MIT / Public Domain / Unlicense | `THIRD_PARTY_LICENSES/*` |
| infcore (our own layer) | MIT | `LICENSE`, `NOTICE` |

Artifacts: `LICENSE`, `NOTICE`, `THIRD_PARTY_LICENSES/` (full license texts for
every component), `sbom.cdx.json` (CycloneDX 1.5, components with versions, the
upstream base commit in the purl). The Docker runtime image also includes the
engine's license as `/opt/infcore/LICENSE.llama.cpp`. See `NOTICE` for the
upstream version pin.

For a release, run `infcore/scripts/release-manifest.sh`: it generates
`release-manifest.json` and `SHA256SUMS` for the binaries, licenses and SBOM.
With `INFCORE_SIGN=1` it additionally creates detached GPG signatures.

## Engine non-modification model
Files outside `infcore/` (the engine) are never edited by hand. Updates happen
only by merging an upstream release tag (`scripts/update-upstream.sh`). This
keeps the boundary clean, the SBOM accurate, and drop-in updates possible.
