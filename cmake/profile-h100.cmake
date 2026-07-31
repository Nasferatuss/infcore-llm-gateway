# infcore — профиль сборки под сервер с NVIDIA H100 (cache-init).
# Применяется так:  cmake -S infcore -B build -C infcore/cmake/profile-h100.cmake
#
# Отличия от profile-portable.cmake (общий парк GPU):
#   - только CUDA sm_90 вместо "75;80;86;89;90" -> сборка кратно быстрее, бинарь
#     и образ меньше. Профиль под ОДНОРОДНЫЙ парк H100;
#   - Vulkan выключен: на выделенном CUDA-сервере он лишний код и лишняя
#     зависимость на Vulkan SDK в base-образе.
# Если парк разнородный (H100 + что-то ещё) - берите profile-portable.cmake.

# --- Бэкенды ggml -------------------------------------------------------------
set(GGML_CUDA      ON  CACHE BOOL "" FORCE)
set(GGML_VULKAN    OFF CACHE BOOL "" FORCE)

# H100 = Hopper = sm_90. ВНИМАНИЕ: 89 (Ada: 4090/L40) здесь НЕ подходит - бинарь,
# собранный под 89, не несёт ядер для H100. Тест-план MSI (WINDOWS_WSL_MSI_TEST_PLAN_RU.md)
# использует 89, потому что писался под RTX 4070 Laptop; копировать оттуда команду
# сборки на сервер нельзя.
# "native" тоже не годится: он требует ВИДИМЫЙ GPU на стадии configure, а в
# `docker build` GPU нет -> конфигурация падает.
set(CMAKE_CUDA_ARCHITECTURES "90" CACHE STRING "" FORCE)

# GGML_NATIVE=OFF: не завязываем бинарь на -march=native хоста сборки, иначе образ,
# собранный на одной CPU-микроархитектуре, упадёт с SIGILL на другой.
set(GGML_NATIVE    OFF CACHE BOOL "" FORCE)

# Всё, что не под наше железо / нарушает offline:
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
set(GGML_RPC       OFF CACHE BOOL "" FORCE)   # offline: без сетевого RPC-бэкенда
set(GGML_BLAS      OFF CACHE BOOL "" FORCE)   # CPU-путь не горячий: веса живут в VRAM

# --- Состав сборки llama.cpp --------------------------------------------------
# Сервер НУЖЕН: gateway поднимает дочерние llama-server; Dockerfile его копирует.
set(LLAMA_BUILD_SERVER   ON  CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    ON  CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

# Web UI: не встраивать и не качать из HuggingFace (offline; наружу работает gateway).
set(LLAMA_BUILD_UI        OFF CACHE BOOL "" FORCE)
set(LLAMA_USE_PREBUILT_UI OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP       OFF CACHE BOOL "" FORCE)

set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)

# Release-артефакт самодостаточен: не полагаемся на то, что libllama.so/libggml*.so
# окажутся рядом с бинарями при install в Docker/systemd.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
