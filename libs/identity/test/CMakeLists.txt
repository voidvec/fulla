cmake_minimum_required(VERSION 3.16)

# libs/identity/test -- pure unit tests, no DB/no Drogon (mirrors
# libs/common/test's and libs/oauth2/test's rationale: design.md §6 keeps
# Domain-layer tests on gtest, not DROGON_TEST, since DROGON_TEST requires
# linking Drogon, which this target must not depend on per design.md §4.1
# rule 1).

find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE FULLA_IDENTITY_TEST_SOURCES "*.cc")

# M5 Task 31: drop the WebAuthn/Social service tests when their feature is
# disabled -- those tests reference symbols (WebAuthnService::*,
# SocialAuthService::*) that are no longer compiled into fulla::identity.
if(NOT WITH_WEBAUTHN)
    list(FILTER FULLA_IDENTITY_TEST_SOURCES EXCLUDE REGEX "(WebAuthnServiceTest|WebAuthnCryptoTest|CborReaderTest)\\.cc$")
endif()
if(NOT WITH_SOCIAL)
    list(FILTER FULLA_IDENTITY_TEST_SOURCES EXCLUDE REGEX "SocialAuthServiceTest\\.cc$")
    list(FILTER FULLA_IDENTITY_TEST_SOURCES EXCLUDE REGEX "SocialLinkServiceTest\\.cc$")
    list(FILTER FULLA_IDENTITY_TEST_SOURCES EXCLUDE REGEX "SocialLoginStateServiceTest\\.cc$")
endif()

add_executable(fulla-identity-test ${FULLA_IDENTITY_TEST_SOURCES})

target_link_libraries(fulla-identity-test
    PRIVATE
        fulla::identity
        fulla::common::testing
        GTest::gtest
        GTest::Main
)
if(WITH_WEBAUTHN)
    # Private crypto tests include CBOR headers directly. The identity
    # library's private dependency does not provide their include paths.
    target_link_libraries(fulla-identity-test PRIVATE libcbor::libcbor)
endif()

set_target_properties(fulla-identity-test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

oauth2_apply_compat(fulla-identity-test)
oauth2_apply_gcov(fulla-identity-test)

include(GoogleTest)
gtest_discover_tests(fulla-identity-test)
