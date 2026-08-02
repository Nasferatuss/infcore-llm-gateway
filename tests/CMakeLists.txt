# infcore tests — MIT licence (see LICENSE). Unit tests for the gateway layer (no engine).
# They use the same embedded JSON schema as the gateway (config.cpp requires it): the header
# is generated once in infcore/CMakeLists.txt and pulled in through the infcore_schema
# INTERFACE target.
add_executable(infcore_unit_tests
    unit/test_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../gateway/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../gateway/json_schema.cpp
)
target_include_directories(infcore_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..          # gateway/*.hpp, security/*, registry/*
    ${CMAKE_CURRENT_SOURCE_DIR}/../gateway  # config.hpp, json_schema.hpp
    ${INFCORE_LLAMA_ROOT}/vendor            # nlohmann/json.hpp
)
target_link_libraries(infcore_unit_tests PRIVATE
    infcore_schema        # schema_embedded.h (generated in infcore/CMakeLists.txt)
    infcore_security
    infcore_registry
    infcore_runtime
    OpenSSL::Crypto
)

add_test(NAME infcore_unit COMMAND infcore_unit_tests)
