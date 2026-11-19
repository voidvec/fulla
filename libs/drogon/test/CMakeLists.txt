cmake_minimum_required(VERSION 3.16)

# libs/drogon/test -- Adapter-layer unit tests. Unlike the Domain-layer test
# dirs (libs/{common,oauth2,identity}/test) these tests DO link Drogon (via
# fulla::drogon), because the units under test (e.g.
# BackchannelLogoutNotifier) are Drogon-coupled adapters. gtest (not
# DROGON_TEST) so binaries run with no event loop; tests here must not require
# a running server or database (DB-touching paths are covered by the
# integration tests under tests/integration/).

find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE FULLA_DROGON_TEST_SOURCES "*.cc")
# The real-Session fixture has its own event loop and compiled main.
list(REMOVE_ITEM FULLA_DROGON_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/PasswordLoginSessionStateTest.cc")

add_executable(fulla-drogon-test ${FULLA_DROGON_TEST_SOURCES})

target_link_libraries(fulla-drogon-test
    PRIVATE
        fulla::drogon
        fulla::common::testing
        GTest::gtest
)

# NOTE: deliberately NOT linking GTest::Main; main() is provided by a
# compiled object in the test sources (BackchannelLogoutNotifierTest.cc).
# Historically this also dodged an MSVC link trap: the transitive Boost
# umbrella linked Boost::test_exec_monitor (a main() that calls test_main),
# which MSVC could resolve before GTest::Main's .lib, leaving test_main
# undefined. drogon/*:with_boost=False removed Boost from the graph, so
# that hazard is gone; the compiled-main pattern is kept as-is.

set_target_properties(fulla-drogon-test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

oauth2_apply_compat(fulla-drogon-test)
oauth2_apply_gcov(fulla-drogon-test)

include(GoogleTest)
gtest_discover_tests(fulla-drogon-test)

# Run all real-Session cases in one process: Drogon starts and stops once.
# This target registers only its private loopback handler, with no DB/plugins.
add_executable(fulla-password-login-session-test PasswordLoginSessionStateTest.cc)
target_link_libraries(fulla-password-login-session-test PRIVATE fulla::drogon GTest::gtest)
set_target_properties(fulla-password-login-session-test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
oauth2_apply_compat(fulla-password-login-session-test)
oauth2_apply_warnings(fulla-password-login-session-test)
oauth2_apply_gcov(fulla-password-login-session-test)
oauth2_apply_sanitizer(fulla-password-login-session-test)
add_test(NAME PasswordLoginSessionState COMMAND fulla-password-login-session-test)
set_tests_properties(PasswordLoginSessionState PROPERTIES TIMEOUT 60)
