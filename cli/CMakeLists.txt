# infcore CLI — MIT licence (see LICENSE).
# A terminal client for the gateway (HTTP). It does not link against the core: only the
# vendored cpp-httplib + nlohmann/json.
add_executable(infcore_cli main.cpp)
set_target_properties(infcore_cli PROPERTIES
    OUTPUT_NAME infcore-cli
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # the shared build/bin used by the deployment

target_include_directories(infcore_cli PRIVATE
    ${INFCORE_LLAMA_ROOT}/vendor            # nlohmann/json.hpp
    ${INFCORE_LLAMA_ROOT}/vendor/cpp-httplib
)
target_link_libraries(infcore_cli PRIVATE cpp-httplib)

install(TARGETS infcore_cli RUNTIME)
