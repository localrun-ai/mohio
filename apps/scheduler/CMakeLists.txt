add_executable(wikore_scheduler main.cpp)

target_link_libraries(wikore_scheduler PRIVATE
    wikore_core
    wikore_app
    wikore_warnings
)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE WIKORE_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
target_compile_definitions(wikore_scheduler PRIVATE
    WIKORE_GIT_HASH="${WIKORE_GIT_HASH}"
)
