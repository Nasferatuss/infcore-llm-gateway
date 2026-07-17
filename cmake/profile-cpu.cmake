# infcore — профиль сборки БЕЗ GPU (cache-init).
# Применяется так:  cmake -S infcore -B build -C infcore/cmake/profile-cpu.cmake
#
# Для чего:
#   - обкатать контур (конфиг, ключи, RBAC, аудит, TLS, ротация, метрики) на
#     сервере без видеокарты, до того как появится GPU;
#   - CPU-only контур, где GPU не будет вовсе.
#
# ВНИМАНИЕ: это НЕ прод-профиль для сервера с H100 — там profile-h100.cmake.
# Собранный этим профилем бинарь не содержит CUDA-ядер вообще и на H100 будет
# считать на CPU, молча и медленно.

# --- Бэкенды ggml -------------------------------------------------------------
# Ничего GPU-шного: не нужен ни CUDA toolkit, ни Vulkan SDK на стадии сборки.
set(GGML_CUDA      OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN    OFF CACHE BOOL "" FORCE)

# GGML_NATIVE=OFF: не завязываем бинарь на -march=native хоста сборки, иначе он
# упадёт с SIGILL на машине с другой микроархитектурой CPU. Если собираете и
# запускаете на ОДНОЙ И ТОЙ ЖЕ машине и хотите выжать скорость - можно включить:
#   cmake -S infcore -B build -C infcore/cmake/profile-cpu.cmake -DGGML_NATIVE=ON
set(GGML_NATIVE    OFF CACHE BOOL "" FORCE)

# BLAS выключен: лишняя зависимость в offline-контуре. Ускоряет в основном
# обработку промпта, а не генерацию. Включайте осознанно, если упрётесь в prompt eval.
set(GGML_BLAS      OFF CACHE BOOL "" FORCE)

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

# --- Состав сборки llama.cpp --------------------------------------------------
# Сервер НУЖЕН: gateway поднимает дочерние llama-server. Именно ради этой строки
# и существует профиль: при встраивании через add_subdirectory llama-server по
# умолчанию НЕ собирается, и «голая» команда cmake без -DLLAMA_BUILD_SERVER=ON
# даёт шлюз без бэкенда - он стартует, но каждый запрос отвечает 502.
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
