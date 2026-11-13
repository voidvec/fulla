# fulla-identity library
cmake_minimum_required(VERSION 3.20)

project(fulla-identity LANGUAGES CXX)

# M5 Task 31: WITH_WEBAUTHN / WITH_SOCIAL are now declared at the TOP-LEVEL
# CMakeLists.txt (and threaded from conanfile.py's with_* options). The
# option() calls below are no-ops when the variable is already set, kept so
# this library still configures standalone (add_subdirectory from a foreign
# root, per OAuth2Plugin/CMakeLists.txt's self-contained-bootstrap pattern).
option(WITH_WEBAUTHN "Build WebAuthn identity module" ON)
option(WITH_SOCIAL "Build social auth identity module" ON)

# Find dependencies
#
# Task 19 (fulla-sdk-refactor, design.md §4.1 rule 1): this is a
# Domain-layer package -- it must not depend on Drogon. No source file
# under libs/identity/src includes any <drogon/...> header (verified by
# grep); jsoncpp alone (used directly for Json::Value in
# IUserRepository.h/AuthService.h) is sufficient, matching libs/common's
# and libs/oauth2's own "允许依赖 jsoncpp" allowance.
find_package(jsoncpp REQUIRED)

# M2.5 identity completion: TotpUtils.cc calls OpenSSL's HMAC directly
# (RFC 6238 TOTP over HMAC-SHA1), mirroring libs/oauth2's own
# find_package(OpenSSL REQUIRED) + OpenSSL::Crypto link for JwkManager --
# design.md §4.1 rule 1 allows OpenSSL in the Domain layer as the one
# non-Drogon exception already established by libs/common's
# ICryptoProvider adapters.
find_package(OpenSSL REQUIRED)

# #142: CBOR parsing for WebAuthn attestation/assertion verification
# (libs/identity/src/webauthn/); same Domain-layer exception rationale as
# OpenSSL above.
set(_identity_webauthn_sources)
set(_identity_webauthn_deps)
if(WITH_WEBAUTHN)
    find_package(libcbor REQUIRED)
    list(APPEND _identity_webauthn_sources
        src/webauthn/WebAuthnService.cc
        src/webauthn/WebAuthnCrypto.cc)
    list(APPEND _identity_webauthn_deps libcbor::libcbor)
endif()

# Sources
#
# Task 19 (fulla-sdk-refactor): RoleProvider/SubjectResolver/
# UserInfoProvider are header-only (no corresponding .cc) -- each is a
# thin one-method forwarding adapter over an identity repository
# interface, same pattern as OAuth2Plugin's existing
# adapters/StorageRoleProvider.h. AuthService.cc/MfaService.cc/
# WebAuthnService.cc/social/*.cc are real (non-placeholder)
# implementations.
set(IDENTITY_SOURCES
    src/AuthService.cc
    # MFA
    src/mfa/MfaService.cc
    src/mfa/TotpUtils.cc
    # WebAuthn (conditional)
    ${_identity_webauthn_sources}
    # Social (conditional)
    $<$<BOOL:${WITH_SOCIAL}>:src/social/GoogleAuthService.cc>
    $<$<BOOL:${WITH_SOCIAL}>:src/social/WeChatAuthService.cc>
    $<$<BOOL:${WITH_SOCIAL}>:src/social/GitHubAuthService.cc>
    $<$<BOOL:${WITH_SOCIAL}>:src/social/SocialLinkService.cc>
    $<$<BOOL:${WITH_SOCIAL}>:src/social/SocialLoginStateService.cc>
    # Session
    src/session/SessionManager.cc
)

# Headers
set(IDENTITY_HEADERS
    include/fulla/identity/AuthService.h
    include/fulla/identity/IUserRepository.h
    include/fulla/identity/IRoleRepository.h
    include/fulla/identity/ISubjectMappingRepository.h
    include/fulla/identity/IMfaRepository.h
    include/fulla/identity/RoleProvider.h
    include/fulla/identity/SubjectResolver.h
    include/fulla/identity/UserInfoProvider.h
    include/fulla/identity/MfaService.h
    include/fulla/identity/TotpUtils.h
    include/fulla/identity/SessionManager.h
    include/fulla/identity/IBackchannelLogoutNotifier.h
    include/fulla/identity/IOAuthHttpClient.h
    $<$<BOOL:${WITH_WEBAUTHN}>:include/fulla/identity/WebAuthnService.h>
    $<$<BOOL:${WITH_WEBAUTHN}>:include/fulla/identity/IWebAuthnRepository.h>
    $<$<BOOL:${WITH_SOCIAL}>:include/fulla/identity/SocialAuthService.h>
    $<$<BOOL:${WITH_SOCIAL}>:include/fulla/identity/SocialLinkService.h>
    $<$<BOOL:${WITH_SOCIAL}>:include/fulla/identity/ISocialAccountRepository.h>
)

add_library(fulla-identity STATIC
    ${IDENTITY_SOURCES}
)
add_library(fulla::identity ALIAS fulla-identity)

target_include_directories(fulla-identity
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(fulla-identity
    PUBLIC
        fulla-common
    PRIVATE
        JsonCpp::JsonCpp
        OpenSSL::Crypto
        ${_identity_webauthn_deps}
)

# Compile definitions for conditional features
if(WITH_WEBAUTHN)
    target_compile_definitions(fulla-identity PUBLIC WITH_WEBAUTHN)
endif()

if(WITH_SOCIAL)
    target_compile_definitions(fulla-identity PUBLIC WITH_SOCIAL)
endif()

# First-party warning level (shared module, same convention as the other
# libs/*). Guarded for the standalone-configure path, mirroring the
# oauth2_apply_compat guard before add_subdirectory(test) below.
if(NOT COMMAND oauth2_apply_warnings)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
    include(Warnings)
endif()
oauth2_apply_warnings(fulla-identity)
oauth2_apply_gcov(fulla-identity)
oauth2_apply_sanitizer(fulla-identity)  # #104: instrument product code in sanitizer builds

# Install rules
install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# SDK packaging: install-tree + build-tree find_package() config carrying the
# transitive find_dependency() closure (fulla-sdk-refactor Task 28a).
# Note: fulla_package() routes install destinations through GNUInstallDirs
# (consistent with the other libs/* packages); the header install above keeps
# this package's original FILES_MATCHING glob.
if(NOT COMMAND fulla_package)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
    include(FullaPackage)
endif()
set(_identity_package_deps jsoncpp OpenSSL)
if(WITH_WEBAUTHN)
    list(APPEND _identity_package_deps libcbor)
endif()
list(APPEND _identity_package_deps fulla-common)
fulla_package(
    TARGET fulla-identity
    PACKAGE fulla-identity
    EXPORT_NAME identity
    DEPENDENCIES ${_identity_package_deps}
)

# --- Tests (pure unit, no DB/no Drogon) ---
# Task 19: mirrors libs/common/test's and libs/oauth2/test's rationale
# (design.md §6) -- gtest, not DROGON_TEST, since this Domain-layer
# target must not depend on Drogon.
if(NOT COMMAND oauth2_apply_compat)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
    include(Compatibility)
endif()
add_subdirectory(test)
