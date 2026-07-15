#!/usr/bin/env bash
# Generate release checksums/manifest for an already built infcore tree.
# Usage: ./infcore/scripts/release-manifest.sh [build_dir] [dist_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${1:-${ROOT}/build}"
DIST="${2:-${ROOT}/dist/infcore}"

mkdir -p "${DIST}"

artifacts=(
  "${BUILD}/bin/infcore_gateway"
  "${BUILD}/bin/infcore-cli"
  "${BUILD}/bin/llama-server"
  "${ROOT}/infcore/LICENSE"
  "${ROOT}/infcore/NOTICE"
  "${ROOT}/infcore/sbom.cdx.json"
)

for f in "${artifacts[@]}"; do
  if [[ ! -f "${f}" ]]; then
    echo "missing release artifact: ${f}" >&2
    exit 1
  fi
done

python3 - "${DIST}" "${artifacts[@]}" <<'PY'
import hashlib
import json
import os
import sys
from datetime import datetime, timezone

dist = sys.argv[1]
files = sys.argv[2:]
entries = []
for path in files:
    h = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            size += len(chunk)
            h.update(chunk)
    entries.append({
        "path": os.path.abspath(path),
        "name": os.path.basename(path),
        "size_bytes": size,
        "sha256": h.hexdigest(),
    })

manifest = {
    "format": "infcore-release-manifest-v1",
    "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "artifacts": entries,
}
manifest_path = os.path.join(dist, "release-manifest.json")
with open(manifest_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, ensure_ascii=False)
    f.write("\n")

with open(os.path.join(dist, "SHA256SUMS"), "w", encoding="utf-8") as f:
    for e in entries:
        f.write(f"{e['sha256']}  {e['path']}\n")
PY

if [[ "${INFCORE_SIGN:-0}" == "1" ]]; then
  gpg --batch --yes --detach-sign --armor "${DIST}/release-manifest.json"
  gpg --batch --yes --detach-sign --armor "${DIST}/SHA256SUMS"
fi

echo "release manifest: ${DIST}/release-manifest.json"
echo "checksums:        ${DIST}/SHA256SUMS"
