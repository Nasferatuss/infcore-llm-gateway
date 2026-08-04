cmake_minimum_required(VERSION 3.21)
project(infcore VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(OpenSSL REQUIRED)

# -----------------------------------------------------------------------------
# The llama.cpp engine (the parent directory) is embedded WITHOUT editing any of its files.
# llama.cpp officially supports being used through add_subdirectory.
# Backend and component flags come from a profile: cmake -C infcore/cmake/profile-portable.cmake
# -----------------------------------------------------------------------------
set(INFCORE_LLAMA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "Root of the llama.cpp tree")

# When embedded via add_subdirectory, LLAMA_STANDALONE=OFF, so common/server are NOT built
# by default. We need both (the gateway links llama-common, and the server is the basis of
# the OpenAI API), so they are forced on before the engine is pulled in.
set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)

if(NOT TARGET llama)
    add_subdirectory(${INFCORE_LLAMA_ROOT} ${CMAKE_BINARY_DIR}/_llama)
endif()

# -----------------------------------------------------------------------------
# The JSON schema is embedded into the binary: the schema file is the single source of
# truth and the header is generated at configure time (the config is validated against it
# at start-up). Editing the schema must re-embed the header (otherwise file(READ) is
# cached) — hence CMAKE_CONFIGURE_DEPENDS.
#
# Generated EXACTLY ONCE here rather than in each consumer (gateway, tests): repeating
# set_property(CMAKE_CONFIGURE_DEPENDS) for the same file from two directories makes the
# Ninja generator emit two rules for one output ->
# "multiple rules generate ... gateway.schema.json [-w dupbuild=err]".
# Consumers pull the header in through the infcore_schema INTERFACE target.
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
# Our own layer (MIT licence). It talks to the engine only through the llama.h C API and
# the llama-common helpers; upstream files are never edited.
# -----------------------------------------------------------------------------
add_subdirectory(security)
add_subdirectory(registry)
add_subdirectory(runtime)
add_subdirectory(gateway)
add_subdirectory(cli)

option(INFCORE_BUILD_TESTS "Build the infcore unit tests (ctest)" ON)
if(INFCORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "infcore: engine embedded from ${INFCORE_LLAMA_ROOT}")
