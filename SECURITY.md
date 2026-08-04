# Security policy

infcore is a gateway that sits in front of local LLM inference and enforces
authentication, authorization and audit. A defect in that layer is a security
defect, so please report one privately rather than in a public issue.

## Reporting

Use **[Report a vulnerability](https://github.com/Nasferatuss/infcore-llm-gateway/security/advisories/new)**
(GitHub private vulnerability reporting). It opens a private thread visible only
to you and the maintainer.

Please include the version or commit, the build profile, a minimal
configuration that reproduces it, and what an attacker gets out of it. A working
reproduction is worth more than a description.

This is a personal project with a single maintainer and no bug bounty.
Expect a first response within a week. If you have not heard back in two, assume
the notification was lost and ping the issue tracker without details.

Please give a fix a reasonable window before publishing. If a report is valid
and you would like credit in the advisory, say so.

## Scope

**In scope** — anything under `infcore/`:

- `security/authn` — API-key verification. The comparison is constant-time and
  does not exit early; a timing side channel here is in scope.
- `security/rbac` — role → allowed models and endpoints. Any way to reach a
  model or endpoint a role does not allow.
- `security/audit` — the append-only journal. Log injection, a way to suppress
  an entry, or a failure that does not fail closed when `audit.require=true`.
- `gateway/` — the HTTP surface: request-size limits, header handling,
  client-address derivation, SSRF and egress checks on `backend_url`, error
  paths that leak configuration or key material.
- `runtime/backend_supervisor` — child-process handling: argument injection into
  a spawned `llama-server`, leaking the per-boot backend key, file descriptors
  surviving `exec`, a race in start/stop.
- The offline invariant — any runtime path that reaches the network.
- The deploy tree in `deploy/`, when a shipped default is itself unsafe.

**Out of scope:**

- Vulnerabilities in **llama.cpp** itself. infcore does not edit the engine —
  report those to [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp/security).
  If the engine's behaviour is only reachable *because* of how infcore drives
  it, that part is in scope here.
- Anything that requires you to already control the gateway's configuration
  file or run as its user. Config is trusted input.
- Denial of service by simply sending a lot of load. Rate limiting is documented
  as the reverse proxy's job (`deploy/angie/`), not the gateway's.
- Attacks on a deployment that ignores the documented posture — for example
  binding the gateway to a public interface without TLS in front of it.

## Assumed posture

These are design decisions, not oversights. A report that one of them is true is
not a finding; a report that one of them can be **broken** is.

- The gateway is intended to sit behind a TLS-terminating reverse proxy. It does
  not terminate TLS itself.
- The configuration file, the model files and the audit destination are trusted.
  Whoever can write them already has the gateway's privileges.
- Managed backends bind `127.0.0.1` only and start with a per-boot random
  `--api-key`, so their ports are not an unauthenticated bypass of the gateway.
- With `audit.require=true` a runtime audit failure degrades to `503`, not to
  serving without an audit trail. If you find a request path that survives an
  audit failure, that is a finding.
- The client address used for authorization decisions is taken from the
  **rightmost** `X-Forwarded-For` entry, which assumes exactly one trusted proxy
  in front. A deployment with a different topology needs to adjust it.

## Supported versions

Pre-1.0: only `main` is supported. Fixes land there and are not backported.
