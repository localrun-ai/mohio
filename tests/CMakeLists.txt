add_executable(mohio_tests
    test_org_tree.cpp
    test_access.cpp
    test_auth.cpp
)
target_link_libraries(mohio_tests PRIVATE
    mohio_core
    Catch2::Catch2WithMain
    mohio_warnings
)
include(Catch)
catch_discover_tests(mohio_tests)
