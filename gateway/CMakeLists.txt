# infcore gateway — MIT licence (see LICENSE).
# Built ALONGSIDE tools/server (whose files are never edited). An OpenAI-compatible
# control plane in front of llama-server backends. Uses the vendored cpp-httplib +
# nlohmann/json from the engine tree; it talks to the core through the llama.h C API.
# The JSON schema is embedded once in infcore/CMakeLists.txt; the generated
# schema_embedded.h is pulled in through the infcore_schema INTERFACE target.
add_executable(infcore_gateway
    main.cpp
    config.cpp
    json_schema.cpp
    server.cpp
)

# The binary goes into the shared build/bin (where the engine also puts llama-server) so
# that the Dockerfile and the deployment copy everything from one directory.
set_target_properties(infcore_gateway PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

target_include_directories(infcore_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..          # registry/model_registry.h, gateway/*.hpp
    ${INFCORE_LLAMA_ROOT}/vendor            # nlohmann/json.hpp
    ${INFCORE_LLAMA_ROOT}/vendor/cpp-httplib
)

# The prototype is a proxy front: the gateway does not link libllama (the backend is a
# separate llama-server). When an in-process mode is needed, add llama-common plus the
# engine includes (see AUDIT.md §5 — the boundary is the llama.h C API).
target_link_libraries(infcore_gateway PRIVATE
    infcore_schema        # schema_embedded.h (generated in infcore/CMakeLists.txt)
    infcore_security
    infcore_registry
    infcore_runtime       # lazy supervisor of the child llama-server processes
    cpp-httplib           # vendored HTTP server/client (REST + proxy)
    OpenSSL::Crypto       # model artifact SHA-256 validation
)

install(TARGETS infcore_gateway RUNTIME)
