# infcore — a portable build profile for an isolated deployment (cache-init).
# Used as:  cmake -S infcore -B build -C infcore/cmake/profile-portable.cmake
# It sets the engine flags BEFORE llama.cpp is configured. Upstream files are not modified.

# --- ggml backends: keep cpu (always) + cuda + vulkan -------------------------
set(GGML_CUDA      ON  CACHE BOOL "" FORCE)
set(GGML_VULKAN    ON  CACHE BOOL "" FORCE)

# CUDA architectures: we do NOT rely on the "native" default (llama.cpp sets it with
# CUDA>=11.6 + CMake>=3.24). "native" requires a VISIBLE GPU at configure time, and there is
# no GPU inside `docker build`, so configuration fails. Pin a concrete architecture set:
#   75=Turing(T4/2080) 80=Ampere(A100) 86=Ampere(A10/3090) 89=Ada(L40/4090) 90=Hopper(H100).
# Narrow or widen it for your GPU fleet: -DCMAKE_CUDA_ARCHITECTURES=86 (faster build,
# smaller image).
set(CMAKE_CUDA_ARCHITECTURES "75;80;86;89;90" CACHE STRING "" FORCE)

# GGML_NATIVE=OFF: the binary is not tied to -march=native of the build host. Otherwise an
# image built on one CPU microarchitecture would die with SIGILL on another in the fleet.
# To build strictly for one specific server you can put -DGGML_NATIVE=ON back.
set(GGML_NATIVE    OFF CACHE BOOL "" FORCE)

# Turn off everything that does not match our hardware or breaks the offline invariant:
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
# BLAS is optional for CPU acceleration; off by default
set(GGML_BLAS      OFF CACHE BOOL "" FORCE)

# --- What of llama.cpp gets built ---------------------------------------------
# The server and mtmd ARE required (the gateway builds on tools/server; mtmd provides
# multimodality).
set(LLAMA_BUILD_SERVER   ON  CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    ON  CACHE BOOL "" FORCE)
# Upstream examples and tests are not needed in a runtime build:
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

# Web UI: neither embedded NOR downloaded from HuggingFace (offline, and there is no browser
# UI; our gateway is what faces outward). The server is built against a stub ui.h and
# llama-ui is empty. The tools/ui directory is NOT physically removed: server-http.cpp has a
# hard #include "ui.h" and calls llama_ui_*, so deleting it would break both the server build
# and drop-in updates.
set(LLAMA_BUILD_UI        OFF CACHE BOOL "" FORCE)
set(LLAMA_USE_PREBUILT_UI OFF CACHE BOOL "" FORCE)
# The unified desktop binary (app/, which contained a networked download.cpp) is not built.
set(LLAMA_BUILD_APP       OFF CACHE BOOL "" FORCE)

set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)

# The release/runtime artifact must be self-contained: we do not rely on
# libllama.so/libggml*.so landing next to the binaries in a Docker/systemd install.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
