cmake_minimum_required(VERSION 3.28)
project(mohio CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_program(CCACHE ccache)
if(CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ---------------------------------------------------------------------------
# Release flags
# ---------------------------------------------------------------------------
add_compile_options($<$<CONFIG:Release>:-O3>)

option(MOHIO_NATIVE_OPTS "Apply -march=native and -flto=thin in Release" ON)
if(MOHIO_NATIVE_OPTS)
    add_compile_options($<$<CONFIG:Release>:-flto=thin>)
    add_compile_options($<$<CONFIG:Release>:-march=native>)
    add_compile_options($<$<CONFIG:Release>:-fno-plt>)
    add_link_options($<$<CONFIG:Release>:-flto=thin>)
    add_link_options($<$<CONFIG:Release>:-Wl,-z,now,-z,relro>)
endif()

# ---------------------------------------------------------------------------
# Debug flags
# ---------------------------------------------------------------------------
add_compile_options($<$<CONFIG:Debug>:-O0>)
add_compile_options($<$<CONFIG:Debug>:-g>)
add_compile_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)
add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)

add_compile_definitions($<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>)

# ---------------------------------------------------------------------------
# Warning interface target (our code only, not third-party)
# ---------------------------------------------------------------------------
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wno-missing-designated-field-initializers
    _WNO_MISSING_DESIGNATED_SUPPORTED)

add_library(mohio_warnings INTERFACE)
target_compile_options(mohio_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers
    $<$<BOOL:${_WNO_MISSING_DESIGNATED_SUPPORTED}>:-Wno-missing-designated-field-initializers>
    $<$<CONFIG:Debug>:-Werror>
)

# ---------------------------------------------------------------------------
# System dependencies
# ---------------------------------------------------------------------------
find_package(spdlog REQUIRED)
find_package(OpenSSL REQUIRED)       # JWT RS256 + AES-256-GCM credential encryption

# hiredis (Redis)
find_package(hiredis CONFIG QUIET)
if(NOT hiredis_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
    add_library(hiredis::hiredis ALIAS PkgConfig::HIREDIS)
endif()

# ---------------------------------------------------------------------------
# CPM packages
# ---------------------------------------------------------------------------
include(cmake/CPM.cmake)

CPMAddPackage(
    NAME drogon
    GITHUB_REPOSITORY drogonframework/drogon
    GIT_TAG v1.9.7
    OPTIONS
        "BUILD_EXAMPLES OFF"
        "BUILD_CTL OFF"
        "BUILD_TESTING OFF"
        "BUILD_DROGON_SHARED OFF"
)
if(drogon_ADDED)
    set_property(TARGET drogon PROPERTY SYSTEM ON)
    target_compile_options(drogon PRIVATE -Wno-deprecated-declarations)
endif()

CPMAddPackage(
    NAME glaze
    GITHUB_REPOSITORY stephenberry/glaze
    GIT_TAG v4.4.3
    OPTIONS "glaze_ENABLE_AVX2 OFF"
)

# jwt-cpp: header-only RS256/HS256 JWT validation
CPMAddPackage(
    NAME jwt-cpp
    GITHUB_REPOSITORY Thalhammer/jwt-cpp
    GIT_TAG v0.7.0
    OPTIONS "JWT_BUILD_EXAMPLES OFF" "JWT_BUILD_TESTS OFF"
)

# ---------------------------------------------------------------------------
# mohio_core: config, auth, DB helpers, org tree, access resolution
# ---------------------------------------------------------------------------
add_library(mohio_core STATIC
    src/config.cpp
    src/auth.cpp
    src/db.cpp
    src/org_tree.cpp
    src/access.cpp
    src/redis.cpp
)
target_include_directories(mohio_core PUBLIC include)
target_link_libraries(mohio_core PUBLIC
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
    drogon
    glaze::glaze
    jwt-cpp::jwt-cpp
    mohio_warnings
)
target_link_libraries(mohio_core PRIVATE hiredis::hiredis)

# ---------------------------------------------------------------------------
# mohio_rag: embedder, retriever, reranker, generator, pipeline
# ---------------------------------------------------------------------------
add_library(mohio_rag STATIC
    src/rag/embedder.cpp
    src/rag/retriever.cpp
    src/rag/reranker.cpp
    src/rag/generator.cpp
    src/rag/pipeline.cpp
)
target_link_libraries(mohio_rag PUBLIC mohio_core)

# ---------------------------------------------------------------------------
# mohio_wiki: wiki store + ops (DB-backed, org-scoped)
# ---------------------------------------------------------------------------
add_library(mohio_wiki STATIC
    src/wiki/store.cpp
    src/wiki/ops.cpp
)
target_link_libraries(mohio_wiki PUBLIC mohio_core mohio_rag)

# ---------------------------------------------------------------------------
# mohio_mcp: MCP tool registry + built-in integration adapters
# ---------------------------------------------------------------------------
add_library(mohio_mcp STATIC
    src/mcp/registry.cpp
    src/mcp/slack.cpp
    src/mcp/jira.cpp
    src/mcp/confluence.cpp
    src/mcp/github.cpp
    src/mcp/webhook.cpp
)
target_link_libraries(mohio_mcp PUBLIC mohio_core)

# ---------------------------------------------------------------------------
# Apps
# ---------------------------------------------------------------------------
option(MOHIO_BUILD_APPS "Build the API server binary" ON)
if(MOHIO_BUILD_APPS)
    add_subdirectory(apps/api)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(MOHIO_BUILD_TESTS "Build Catch2 unit tests" ON)
if(MOHIO_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
