add_executable(wikore_tests
    test_org_tree.cpp
    test_access.cpp
    test_auth.cpp
    test_error_mapper.cpp
    test_promote_version.cpp
)
target_link_libraries(wikore_tests PRIVATE
    wikore_core
    wikore_app
    wikore_infra
    Catch2::Catch2WithMain
    wikore_warnings
)
include(Catch)
catch_discover_tests(wikore_tests)
