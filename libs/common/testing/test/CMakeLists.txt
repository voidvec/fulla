cmake_minimum_required(VERSION 3.16)

# libs/common/testing/test -- pure gtest unit tests proving the fake
# implementations behave deterministically (Task 15's acceptance
# criterion: "Domain 可用假时钟/假 crypto 做确定性单测").

find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE FULLA_COMMON_TESTING_TEST_SOURCES "*.cc")

add_executable(fulla-common-testing-test ${FULLA_COMMON_TESTING_TEST_SOURCES})

target_link_libraries(fulla-common-testing-test
    PRIVATE fulla::common::testing GTest::gtest GTest::Main
)

set_target_properties(fulla-common-testing-test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
)

oauth2_apply_compat(fulla-common-testing-test)
oauth2_apply_gcov(fulla-common-testing-test)
oauth2_apply_sanitizer(fulla-common-testing-test)

include(GoogleTest)
gtest_discover_tests(fulla-common-testing-test)
