# infcore tests — корпоративная лицензия. Юнит-тесты слоя gateway (без движка).
# Используют ту же встроенную JSON-Schema, что и gateway (config.cpp её требует):
# заголовок генерируется один раз в infcore/CMakeLists.txt, подключается через
# INTERFACE-таргет infcore_schema.
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
    infcore_schema        # schema_embedded.h (сгенерировано в infcore/CMakeLists.txt)
    infcore_security
    infcore_registry
    infcore_runtime
    OpenSSL::Crypto
)

add_test(NAME infcore_unit COMMAND infcore_unit_tests)
