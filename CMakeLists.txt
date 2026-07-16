cmake_minimum_required(VERSION 3.21)
project(infcore VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(OpenSSL REQUIRED)

# -----------------------------------------------------------------------------
# Встраиваем движок llama.cpp (родительский каталог) БЕЗ изменений его файлов.
# llama.cpp штатно поддерживает использование через add_subdirectory.
# Флаги бэкендов/состава задаются профилем: cmake -C infcore/cmake/profile-rf.cmake
# -----------------------------------------------------------------------------
set(INFCORE_LLAMA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "Корень дерева llama.cpp (форк)")

# При встраивании через add_subdirectory LLAMA_STANDALONE=OFF, поэтому
# common/server по умолчанию НЕ собираются. Нам они нужны (gateway линкует
# llama-common; сервер — основа OpenAI-API). Форсируем до подключения движка.
set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)

if(NOT TARGET llama)
    add_subdirectory(${INFCORE_LLAMA_ROOT} ${CMAKE_BINARY_DIR}/_llama)
endif()

# -----------------------------------------------------------------------------
# Встраиваем JSON-Schema в бинарь: единый источник истины - файл схемы,
# заголовок генерируется на этапе конфигурации (config валидируется по нему при
# старте). Правка схемы должна пере-встраивать заголовок (иначе file(READ)
# кэшируется) - отсюда CMAKE_CONFIGURE_DEPENDS.
#
# Генерируем РОВНО ОДИН РАЗ здесь, а не в каждом потребителе (gateway, tests):
# повторный set_property(CMAKE_CONFIGURE_DEPENDS) на тот же файл из двух
# каталогов заставляет генератор Ninja выпустить два правила на один и тот же
# выход -> "multiple rules generate ... gateway.schema.json [-w dupbuild=err]".
# Потребители подключают заголовок через INTERFACE-таргет infcore_schema.
# -----------------------------------------------------------------------------
set(INFCORE_SCHEMA_FILE ${CMAKE_CURRENT_SOURCE_DIR}/config/schema/gateway.schema.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${INFCORE_SCHEMA_FILE})
file(READ ${INFCORE_SCHEMA_FILE} INFCORE_SCHEMA_JSON)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/gateway/schema_embedded.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/infcore_generated/schema_embedded.h
    @ONLY)

add_library(infcore_schema INTERFACE)
target_include_directories(infcore_schema INTERFACE
    ${CMAKE_CURRENT_BINARY_DIR}/infcore_generated)

# -----------------------------------------------------------------------------
# Слой «своё» (корпоративная лицензия). Общается с движком только через C-API
# llama.h и через хелперы llama-common; файлы апстрима не редактируются.
# -----------------------------------------------------------------------------
add_subdirectory(security)
add_subdirectory(registry)
add_subdirectory(runtime)
add_subdirectory(gateway)
add_subdirectory(cli)

option(INFCORE_BUILD_TESTS "Собирать юнит-тесты infcore (ctest)" ON)
if(INFCORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "infcore: движок встроен из ${INFCORE_LLAMA_ROOT}")
