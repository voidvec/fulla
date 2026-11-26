cmake_minimum_required(VERSION 3.16)

# libs/common/test -- pure unit tests, no DB/no Drogon (design.md §6:
# "libs/common/test/ # 纯单元（无 DB/无 drogon）"). Uses gtest (already a
# Conan test_requires per conanfile.py), NOT the DROGON_TEST framework the
# rest of this repo's backend tests use -- DROGON_TEST requires linking
# Drogon (<drogon/drogon_test.h>), which this Domain-layer target must not
# depend on (design.md §4.1 rule 1).

find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE FULLA_COMMON_TEST_SOURCES "*.cc")

add_executable(fulla-common-test ${FULLA_COMMON_TEST_SOURCES})

target_link_libraries(fulla-common-test PRIVATE fulla::common GTest::gtest GTest::Main)

set_target_properties(fulla-common-test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

oauth2_apply_compat(fulla-common-test)
oauth2_apply_gcov(fulla-common-test)
oauth2_apply_sanitizer(fulla-common-test)

include(GoogleTest)
gtest_discover_tests(fulla-common-test)
