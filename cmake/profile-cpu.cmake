# infcore — a build profile WITHOUT GPU support (cache-init).
# Used as:  cmake -S infcore -B build -C infcore/cmake/profile-cpu.cmake
#
# What it is for:
#   - shaking down a deployment (config, keys, RBAC, audit, TLS, rotation, metrics) on a
#     server with no graphics card, before a GPU is available;
#   - CPU-only deployments, where there will never be a GPU.
#
# NOTE: this is NOT the production profile for an H100 server — that is profile-h100.cmake.
# A binary built with this profile contains no CUDA kernels at all and will compute on the
# CPU even on an H100 — silently, and slowly.

# --- ggml backends ------------------------------------------------------------
# Nothing GPU-related: neither the CUDA toolkit nor the Vulkan SDK is needed to build.
set(GGML_CUDA      OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN    OFF CACHE BOOL "" FORCE)

# GGML_NATIVE=OFF: the binary is not tied to -march=native of the build host, which would
# make it die with SIGILL on a machine with a different CPU microarchitecture. If you build
# and run on THE SAME machine and want the extra speed, you can turn it on:
#   cmake -S infcore -B build -C infcore/cmake/profile-cpu.cmake -DGGML_NATIVE=ON
set(GGML_NATIVE    OFF CACHE BOOL "" FORCE)

# BLAS is off: an extra dependency in an offline deployment. It mainly speeds up prompt
# processing rather than generation. Enable it deliberately if prompt eval is your
# bottleneck.
set(GGML_BLAS      OFF CACHE BOOL "" FORCE)

# Everything that does not match our hardware or breaks the offline invariant:
set(GGML_METAL     OFF CACHE BOOL "" FORCE)
set(GGML_SYCL      OFF CACHE BOOL "" FORCE)
set(GGML_OPENCL    OFF CACHE BOOL "" FORCE)
set(GGML_CANN      OFF CACHE BOOL "" FORCE)
set(GGML_MUSA      OFF CACHE BOOL "" FORCE)
set(GGML_HEXAGON   OFF CACHE BOOL "" FORCE)
set(GGML_OPENVINO  OFF CACHE BOOL "" FORCE)
set(GGML_WEBGPU    OFF CACHE BOOL "" FORCE)
set(GGML_ZDNN      OFF CACHE BOOL "" FORCE)
set(GGML_ZENDNN    OFF CACHE BOOL "" FORCE)
set(GGML_VIRTGPU   OFF CACHE BOOL "" FORCE)
set(GGML_HIP       OFF CACHE BOOL "" FORCE)
set(GGML_RPC       OFF CACHE BOOL "" FORCE)   # offline: no networked RPC backend

# --- What of llama.cpp gets built ---------------------------------------------
# The server IS required: the gateway starts child llama-server processes. This one line is
# the reason the profile exists — when embedded via add_subdirectory, llama-server is NOT
# built by default, and a bare cmake invocation without -DLLAMA_BUILD_SERVER=ON produces a
# gateway with no backend: it starts, and answers 502 to every request.
set(LLAMA_BUILD_SERVER   ON  CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    ON  CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

# Web UI: neither embedded nor downloaded from HuggingFace (offline; the gateway is what
# faces outward).
set(LLAMA_BUILD_UI        OFF CACHE BOOL "" FORCE)
set(LLAMA_USE_PREBUILT_UI OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP       OFF CACHE BOOL "" FORCE)

set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)

# The release artifact is self-contained: we do not rely on libllama.so/libggml*.so ending
# up next to the binaries when installed under Docker/systemd.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
