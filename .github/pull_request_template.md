## What this changes

<!-- One paragraph. What behaviour is different after this PR, and why. -->

## Why this way

<!-- The reasoning, the alternative you rejected, and anything you are unsure
     about. If the change is obvious, delete this section. -->

## How it was tested

<!-- Commands you actually ran, and what they printed. Concurrency, shutdown
     and security changes need the hardening smoke:
       bash infcore/tests/manual/hardening_smoke.sh ./build/bin/infcore_gateway -->

- [ ] `ctest --test-dir build --output-on-failure`
- [ ] `hardening_smoke.sh` (if this touches the supervisor, shutdown, or `security/`)
- [ ] No file outside `infcore/` is modified — the engine stays untouched
- [ ] Nothing in the runtime path reaches the network

## AI usage

<!-- Required. "None", or which assistant and for what. Assisted code is fine;
     unreviewed generated code is not. By ticking the box you confirm you have
     read the diff and run it yourself. -->

- [ ] I have reviewed and tested every line I am submitting
