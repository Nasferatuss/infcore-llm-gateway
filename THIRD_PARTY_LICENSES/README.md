# Third-party license texts

Full license texts of the bundled open-source components. The texts are
extracted directly from the components' source files in the engine tree
(`./LICENSE`, `./vendor/`). A machine-readable inventory lives in
`../sbom.cdx.json` (CycloneDX).

| Component | SPDX identifier | File | Version | Text source |
|-----------|--------------------|------|--------|-----------------|
| llama.cpp / ggml (ggml ships with llama.cpp) | MIT | [ggml-llama.cpp.txt](ggml-llama.cpp.txt) | e8ecce5 | `./LICENSE` (root of the engine tree) |
| nlohmann/json | MIT | [nlohmann-json.txt](nlohmann-json.txt) | 3.12.0 | `vendor/nlohmann/json.hpp` (SPDX header) |
| cpp-httplib | MIT | [cpp-httplib.txt](cpp-httplib.txt) | 0.48.0 | `vendor/cpp-httplib/LICENSE` |
| stb (stb_image) | MIT OR Unlicense | [stb.txt](stb.txt) | 2.30 | end of `vendor/stb/stb_image.h` |
| miniaudio | Unlicense OR MIT-0 | [miniaudio.txt](miniaudio.txt) | 0.11.25 | end of `vendor/miniaudio/miniaudio.h` |
| sheredom / subprocess.h | Unlicense | [sheredom.txt](sheredom.txt) | — | start of `vendor/sheredom/subprocess.h` |

Notes:
- stb and miniaudio are dual-licensed; either of the listed licenses may be
  chosen (the full text of both alternatives is kept for distribution).
- sheredom/subprocess.h is distributed as public domain (Unlicense); no
  version is stamped in the header, so identification is by the upstream
  GitHub repository.
