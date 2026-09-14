cmake_minimum_required(VERSION 3.16)

# libs/oauth2 (fulla::oauth2) -- Task 17, fulla-sdk-refactor
# (design.md §8/§17). Domain-layer OAuth2/OIDC protocol package: pure
# protocol logic (PKCE, authorization/token services, consent+scope
# decision engine, aggregates), with zero Drogon dependency (design.md
# §4.1 rule 1) and zero dependency on OAuth2Plugin (the dependency
# direction is the other way: OAuth2Plugin/apps/server eventually depends
# on libs/oauth2).
#
# Scope note (Task 17 slice 1): only the oauth2::pkce pure-function module
# is populated so far. AuthorizationService/TokenService/access/ decision
# engine/model/ aggregates and the M1 repository interfaces are follow-up
# slices of this same task (mirrors Task 14's multi-slice discipline --
# design.md's own H6 guidance: small, independently-buildable/testable
# increments rather than one large migration).
#
# Dependency rule: same as libs/common (design.md §4.1 rule 1) -- this
# target MUST NOT depend on Drogon, and per design.md §4.1 rule 2 must not
# depend on libs/identity either (oauth2 <-> identity 互不依赖). It DOES
# depend on libs/common (value objects + ports).

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
if(NOT DEFINED FULLA_PROJECT_VERSION)
    set(FULLA_PROJECT_VERSION 1.3.0)
endif()
if(NOT COMMAND oauth2_apply_compat)
    include(Compatibility)
endif()
if(NOT COMMAND oauth2_apply_warnings)
    include(Warnings)
endif()

project(fulla-oauth2 VERSION ${FULLA_PROJECT_VERSION} LANGUAGES CXX)

# OpenSSL (M2b Task 17 slice 10): JwkManager (jwk/JwkManager.cc) signs
# RS256 JWTs and does RSA key loading/generation directly against
# OpenSSL, mirroring OAuth2Plugin's own find_package(OpenSSL REQUIRED) +
# OpenSSL::Crypto link (design.md §4.1 rule 1 allows OpenSSL in the
# Domain layer -- it is the one non-Drogon exception already established
# by libs/common's ICryptoProvider adapters).
find_package(OpenSSL REQUIRED)

add_library(${PROJECT_NAME} STATIC)
add_library(fulla::oauth2 ALIAS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

file(GLOB_RECURSE FULLA_FULLA_SOURCES "src/*.cc")
target_sources(${PROJECT_NAME} PRIVATE ${FULLA_FULLA_SOURCES})

target_link_libraries(${PROJECT_NAME} PUBLIC fulla::common OpenSSL::Crypto)

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Silence MSVC C4996 for std::getenv (JwkManager). PRIVATE so the
# definition never leaks into the exported SDK interface.
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

oauth2_apply_compat(${PROJECT_NAME})
oauth2_apply_warnings(${PROJECT_NAME})
oauth2_apply_gcov(${PROJECT_NAME})
oauth2_apply_sanitizer(${PROJECT_NAME})  # #104: instrument product code in sanitizer builds

# --- arch-guard note (see libs/common/CMakeLists.txt's identical note) ---
# This target must never gain a Drogon dependency, and must never depend
# on libs/identity -- manually enforced until tools/arch-guard exists (M6,
# Task 33).

include(GNUInstallDirs)
install(DIRECTORY include/fulla DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# SDK packaging: install-tree + build-tree find_package() config carrying the
# transitive find_dependency() closure (fulla-sdk-refactor Task 28a).
if(NOT COMMAND fulla_package)
    include(FullaPackage)
endif()
fulla_package(
    TARGET ${PROJECT_NAME}
    PACKAGE fulla-oauth2
    EXPORT_NAME oauth2
    DEPENDENCIES OpenSSL fulla-common
)

# --- Tests (pure unit, no DB/no Drogon) ---
add_subdirectory(test)
