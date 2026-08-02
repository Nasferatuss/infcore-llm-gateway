#!/usr/bin/env bash
# infcore — drop-in update of the engine from upstream llama.cpp.
# The "wrap it, do not touch the core" model: an update is a merge of an upstream release
# tag. Run on a build machine with internet access.
#
#   ./infcore/scripts/update-upstream.sh b1234
#
# Steps:
#   1) fetch the upstream tags;  2) merge the tag into the infcore branch;
#   3) resolve conflicts (expected only in cmake/the root and in the removed compliance set —
#      see infcore/docs/COMPLIANCE.md);
#   4) rebuild with the profile and run the tests plus the egress test;
#   5) update UPSTREAM_COMMIT/NOTICE/SBOM.
set -euo pipefail
TAG="${1:?pass an upstream release tag, e.g. b1234}"
git remote get-url upstream >/dev/null 2>&1 || \
  git remote add upstream https://github.com/ggml-org/llama.cpp.git
git fetch --tags upstream
echo "Merging upstream tag ${TAG} into $(git branch --show-current)..."
git merge --no-ff "${TAG}" || {
  echo "There are conflicts — resolve them (cmake/the root + the compliance set), then 'git commit'." >&2
  exit 1
}
echo "Merge OK. Rebuild with ./infcore/scripts/build.sh and run the tests plus egress."
