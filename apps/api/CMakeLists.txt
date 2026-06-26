add_executable(mohio_api main.cpp)

target_link_libraries(mohio_api PRIVATE
    mohio_core
    mohio_rag
    mohio_wiki
    mohio_mcp
    mohio_warnings
)

# Git hash baked in at link time
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE MOHIO_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
target_compile_definitions(mohio_api PRIVATE
    MOHIO_GIT_HASH="${MOHIO_GIT_HASH}"
)
