# infcore — a build profile for a server with an NVIDIA H100 (cache-init).
# Used as:  cmake -S infcore -B build -C infcore/cmake/profile-h100.cmake
#
# Differences from profile-portable.cmake (a mixed GPU fleet):
#   - CUDA sm_90 only instead of "75;80;86;89;90" -> the build is several times faster and
#     both the binary and the image are smaller. This profile targets a HOMOGENEOUS H100
#     fleet;
#   - Vulkan is off: on a dedicated CUDA server it is dead code plus an extra Vulkan SDK
#     dependency in the base image.
# If the fleet is mixed (H100 plus something else), use profile-portable.cmake.

# --- ggml backends ------------------------------------------------------------
set(GGML_CUDA      ON  CACHE BOOL "" FORCE)
set(GGML_VULKAN    OFF CACHE BOOL "" FORCE)

# H100 = Hopper = sm_90. NOTE: 89 (Ada: 4090/L40) will NOT do here — a binary built for 89
# carries no kernels for an H100. The WSL bring-up plan (docs/TEST_PLAN_WSL.md) uses 89
# because it was written for a consumer laptop GPU; its build command must not be copied
# onto the server.
# "native" is no good either: it requires a VISIBLE GPU at configure time, and there is no
# GPU inside `docker build` -> configuration fails.
set(CMAKE_CUDA_ARCHITECTURES "90" CACHE STRING "" FORCE)

# GGML_NATIVE=OFF: the binary is not tied to -march=native of the build host, otherwise an
# image built on one CPU microarchitecture would die with SIGILL on another.
set(GGML_NATIVE    OFF CACHE BOOL "" FORCE)

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
set(GGML_BLAS      OFF CACHE BOOL "" FORCE)   # the CPU path is not hot: the weights live in VRAM

# --- What of llama.cpp gets built ---------------------------------------------
# The server IS required: the gateway starts child llama-server processes and the Dockerfile
# copies it.
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
