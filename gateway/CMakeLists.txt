# infcore gateway — лицензия MIT (см. LICENSE).
# Строится РЯДОМ с tools/server (его файлы не редактируем). OpenAI-совместимый
# control-plane перед бэкендами llama-server. Использует vendored cpp-httplib +
# nlohmann/json из дерева движка; с ядром общается через C-API llama.h.
# JSON-Schema встраивается один раз в infcore/CMakeLists.txt; подключаем
# сгенерированный schema_embedded.h через INTERFACE-таргет infcore_schema.
add_executable(infcore_gateway
    main.cpp
    config.cpp
    json_schema.cpp
    server.cpp
)

# Складываем бинарь в общий build/bin (туда же движок кладёт llama-server) -
# чтобы Dockerfile/деплой копировали всё из одного каталога.
set_target_properties(infcore_gateway PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

target_include_directories(infcore_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..          # registry/model_registry.h, gateway/*.hpp
    ${INFCORE_LLAMA_ROOT}/vendor            # nlohmann/json.hpp
    ${INFCORE_LLAMA_ROOT}/vendor/cpp-httplib
)

# Прототип — proxy-front: gateway не линкует libllama (бэкенд = отдельный
# llama-server). Когда понадобится in-process режим, добавить llama-common +
# include движка (см. AUDIT.md §5, граница по C-API llama.h).
target_link_libraries(infcore_gateway PRIVATE
    infcore_schema        # schema_embedded.h (сгенерировано в infcore/CMakeLists.txt)
    infcore_security
    infcore_registry
    infcore_runtime       # lazy-supervisor дочерних llama-server
    cpp-httplib           # vendored HTTP сервер/клиент (REST + прокси)
    OpenSSL::Crypto       # model artifact SHA-256 validation
)

install(TARGETS infcore_gateway RUNTIME)
