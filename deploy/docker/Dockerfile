# infcore — an image for an offline deployment. Build context = the ROOT of the engine tree.
# The internet is permitted ONLY at build time; the runtime is strictly offline.
# The base images are not public: they are built once from the skeletons next door and kept
# in the deployment's registry. Recipes: infcore/deploy/docker/base/ (README + skeletons).

# --- build stage ---
ARG BASE_BUILD=infcore/base-devel:1.7
FROM ${BASE_BUILD} AS build
WORKDIR /src
COPY . /src
# CUDA toolkit / Vulkan SDK — from the internal package mirror.
RUN cmake -S infcore -B build -C infcore/cmake/profile-portable.cmake \
 && cmake --build build -j"$(nproc)"

# --- runtime stage (offline) ---
# NOTE: the runtime base image must contain the GPU RUNTIME libraries matching the build
# profile (libcuda/the CUDA runtime with GGML_CUDA=ON, a Vulkan ICD with GGML_VULKAN=ON), or
# llama-server will not start. The NVIDIA driver is passed through by nvidia-container-toolkit;
# the CUDA/Vulkan runtime is installed into the base image from the internal mirror. For a
# CPU-only deployment build with -DGGML_CUDA=OFF -DGGML_VULKAN=OFF (see DEPLOY.md, the GPU
# section).
ARG BASE_RUNTIME=infcore/base-runtime:1.7
FROM ${BASE_RUNTIME}
WORKDIR /opt/infcore
# Only the binaries actually needed go into the runtime (gateway, cli, llama-server); the
# other upstream tools (bench/quantize/rpc-client and so on) are left out — a smaller attack
# surface.
COPY --from=build /src/build/bin/infcore_gateway /src/build/bin/infcore-cli /src/build/bin/llama-server /opt/infcore/bin/
RUN set -eu; \
    if command -v ldd >/dev/null 2>&1; then \
      for f in /opt/infcore/bin/infcore_gateway /opt/infcore/bin/infcore-cli /opt/infcore/bin/llama-server; do \
        ldd "$f" | grep -q "not found" && { echo "missing runtime library for $f"; exit 1; } || true; \
      done; \
    fi
# A config TEMPLATE (env: keys, no secrets). In production, mount your own (host=0.0.0.0).
COPY infcore/config/gateway.yaml /opt/infcore/config/gateway.yaml
COPY infcore/docs/DEPLOY.md /opt/infcore/docs/DEPLOY.md
COPY LICENSE /opt/infcore/LICENSE.llama.cpp
COPY infcore/LICENSE infcore/NOTICE infcore/sbom.cdx.json /opt/infcore/
COPY infcore/THIRD_PARTY_LICENSES /opt/infcore/THIRD_PARTY_LICENSES
# An unprivileged user plus a directory for the append-only audit journal.
RUN useradd -r -s /usr/sbin/nologin infcore \
 && mkdir -p /var/log/infcore \
 && chown infcore:infcore /var/log/infcore
USER infcore
# Models are mounted as a volume, never baked into the image. The gateway starts managed
# llama-server processes itself (config.runtime.llama_server_bin=/opt/infcore/bin/llama-server).
EXPOSE 8080
ENTRYPOINT ["/opt/infcore/bin/infcore_gateway", "/opt/infcore/config/gateway.yaml"]
