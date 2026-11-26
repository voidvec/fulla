cmake_minimum_required(VERSION 3.16)

# libs/oauth2/test -- pure unit tests, no DB/no Drogon (mirrors
# libs/common/test's rationale: design.md §6 keeps Domain-layer tests on
# gtest, not the DROGON_TEST framework the rest of this repo's backend
# tests use, since DROGON_TEST requires linking Drogon
# (<drogon/drogon_test.h>), which this target must not depend on per
# design.md §4.1 rule 1).

find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE FULLA_FULLA_TEST_SOURCES "*.cc")

add_executable(fulla-oauth2-test ${FULLA_FULLA_TEST_SOURCES})

target_link_libraries(fulla-oauth2-test
    PRIVATE
        fulla::oauth2
        fulla::common::testing
        GTest::gtest
        GTest::Main
)

set_target_properties(fulla-oauth2-test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

oauth2_apply_compat(fulla-oauth2-test)
oauth2_apply_gcov(fulla-oauth2-test)
oauth2_apply_sanitizer(fulla-oauth2-test)

include(GoogleTest)
gtest_discover_tests(fulla-oauth2-test)
