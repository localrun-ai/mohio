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

CPMAddPackage(
    NAME jwt-cpp
    GITHUB_REPOSITORY Thalhammer/jwt-cpp
    GIT_TAG v0.7.0
    OPTIONS "JWT_BUILD_EXAMPLES OFF" "JWT_BUILD_TESTS OFF"
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUUID REQUIRED IMPORTED_TARGET uuid)

CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.7.1
    OPTIONS "CATCH_INSTALL_DOCS OFF"
)
