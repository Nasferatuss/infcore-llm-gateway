# infcore base images (build + runtime)

`infcore/deploy/docker/Dockerfile` refers to two pre-built images:

| ARG | Default | Purpose |
|---|---|---|
| `BASE_BUILD` | `infcore/base-devel:1.7` | the build stage (toolchain, SDK) |
| `BASE_RUNTIME` | `infcore/base-runtime:1.7` | the runtime stage (runtime libs only) |

These images exist neither upstream nor in public registries — they get built
once. The **recipe skeletons** live here (`Dockerfile.base-devel`,
`Dockerfile.base-runtime`): they start from public `debian:12-slim` so they
build as-is. Adapt `FROM` and the package names to your enclave's base OS,
build and push to its registry under the same tags (or override
`--build-arg BASE_BUILD=... BASE_RUNTIME=...` when building the main image).

## What the build image MUST have
- a C++17 compiler (gcc/g++ >= 11), `make`, `ninja` (optional), `git` (for the
  build number);
- CMake >= 3.21;
- the **CUDA toolkit** (nvcc) — a version matching your GPU fleet (the arch is
  pinned in `profile-portable.cmake`: `75;80;86;89;90`); a GPU is NOT needed at
  build time, since `native` is not used;
- the **Vulkan SDK** — headers + `glslc`/`glslangValidator` (needed to compile
  ggml-vulkan's shaders at build time).
- Everything from an internal package mirror (internet is available ONLY at
  the build-image stage).

## What the runtime image MUST have
- glibc + libstdc++ (compatible with the build image), `libgomp` (OpenMP);
- the **CUDA runtime** (`libcudart`, `libcublas`) — when `GGML_CUDA=ON`;
- the **Vulkan loader** (`libvulkan.so` + your driver's ICD) — when
  `GGML_VULKAN=ON`;
- NO toolchain/SDK (smaller attack surface). The NVIDIA driver is passed
  through by `nvidia-container-toolkit` from the host, not baked into the
  image.
- For a **CPU-only** enclave, neither the CUDA nor the Vulkan runtime is
  needed — build the main image from a profile with
  `-DGGML_CUDA=OFF -DGGML_VULKAN=OFF`.

## Compatibility check
The CUDA runtime version in base-runtime must be >= the CUDA toolkit version in
base-devel; the Vulkan loader must be no older than the SDK. Otherwise
`llama-server` fails to start and the gateway returns `502 backend start
failed` (with `audit.require=true` this is a loud error, not a silent outage).
