# infcore runtime — MIT licence (see LICENSE).
# A lazy supervisor of child llama-server processes. Uses the vendored cpp-httplib for
# health polling; it does not link against the engine (it manages it as a separate process).
add_library(infcore_runtime STATIC
    backend_supervisor.cpp
)

target_include_directories(infcore_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..          # registry/model_registry.h
)
target_include_directories(infcore_runtime PRIVATE
    ${INFCORE_LLAMA_ROOT}/vendor/cpp-httplib
)

target_link_libraries(infcore_runtime PUBLIC
    infcore_registry
)
target_link_libraries(infcore_runtime PRIVATE
    cpp-httplib
)
