cmake_minimum_required(VERSION 3.21)

project(fulla
    VERSION 1.2.0
    LANGUAGES CXX
    DESCRIPTION "Fulla — OAuth2 / OIDC Authorization Server & SDK (Drogon)")

# Load shared CMake modules (after project() so toolchain is fully applied)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Version)
include(Compatibility)
include(Sanitizers)
# Lto.cmake provides the opt-in FULLA_ENABLE_LTO option (default OFF;
# the *-lto presets turn it on). Included unconditionally so the option is
# always declared; behavior with the default is a no-op.
include(Lto)
# Coverage.cmake provides oauth2_apply_gcov(target) + the FULLA_TEST_COVERAGE
# option (GCC/Clang, Debug). Applied per-target inside each libs/* and tests
# CMakeLists so the STATIC libraries themselves are instrumented (not just the
# test binary that links them). See cmake/Coverage.cmake for the rationale.
include(Coverage)
# Warnings.cmake provides oauth2_apply_warnings(target) (/W4 resp.
# -Wall -Wextra, PRIVATE only) plus the FULLA_WERROR option (default
# OFF; the CI pipeline passes -DFULLA_WERROR=ON). Applied per-target
# inside each libs/*, apps/server and tests CMakeLists.
include(Warnings)
# Paths.cmake parses the top-level paths.env (single source of truth for
# source/build/SQL/config paths, review H2) and exposes FULLA_PATH_* vars.
include(Paths)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# M5 Task 31 (fulla-sdk-refactor, design.md F9 / §9.1): conditional-
# compilation options for the optional Fulla feature areas. Declared at
# the TOP level (before any add_subdirectory) so every lib's own option()
# calls resolve to the same value (option() is a no-op when the variable is
# already set). Conan's matching with_identity/with_social/with_webauthn
# options are threaded into these as WITH_* cache vars by conanfile.py's
# generate(). All default ON to preserve today's build behavior; turning
# WITH_SOCIAL/WITH_WEBAUTHN OFF shrinks the dependency surface (the social
# controllers + WebAuthn controllers/services drop out of the build). libcbor
# (found via conan, target libcbor::libcbor) is a hard requirement of the
# WITH_WEBAUTHN=ON identity build since #142: the WebAuthn attestation/
# assertion verification (libs/identity/src/webauthn/) parses authenticator
# attestation objects and COSE keys with it.
#
# WITH_IDENTITY is declared for symmetry with conanfile.py's `with_identity`
# and currently defaults ON; flipping it OFF is NOT yet a working
# configuration -- fulla::identity is consumed by storage-postgres,
# OAuth2Plugin, and several controllers (Session/Mfa/WebAuthn/Organization),
# so disabling it requires broader wiring that is out of scope for this task
# (tracked as follow-up). libs/identity itself always builds today.
option(WITH_IDENTITY "Build the fulla::identity SDK module" ON)
option(WITH_SOCIAL "Build social-login (Google/WeChat/GitHub) controllers + services" ON)
option(WITH_WEBAUTHN "Build the WebAuthn/FIDO2 controller + service" ON)

# libs/common (fulla::common, M2a Task 13): Domain-layer shared kernel.
# Added before OAuth2Plugin/OAuth2Server since it has no dependency on
# either (design.md §4.1: Domain layer must not depend on Drogon) -- this
# is purely additive for now; OAuth2Plugin/OAuth2Server do not yet consume
# it (that migration is a later M2a task).
add_subdirectory(${FULLA_PATH_LIBS_COMMON_DIR})

# libs/common/testing (fulla::common::testing, M2a Task 15): fake
# ("test double") implementations of every common::ports interface, kept
# separate from both libs/common (ports only) and OAuth2Plugin's
# production Adapter implementations, so production binaries never link
# test doubles and future standalone Domain packages can depend on this
# for deterministic unit tests without pulling in Drogon.
add_subdirectory(${FULLA_PATH_LIBS_COMMON_TESTING_DIR})

# libs/oauth2 (fulla::oauth2, M2b Task 17): Domain-layer OAuth2/OIDC
# protocol package. Added before OAuth2Plugin/OAuth2Server for the same
# reason as libs/common above -- it depends only on libs/common, never on
# Drogon or OAuth2Plugin (design.md §4.1). Purely additive for now:
# OAuth2Plugin/OAuth2Server do not yet consume it.
add_subdirectory(${FULLA_PATH_LIBS_FULLA_DIR})

# libs/identity (fulla::identity, M2.5 Task 19): Domain-layer identity
# & authentication package. Handles user auth, MFA, WebAuthn, social login,
# session management, RBAC. Implements IRoleProvider, ISubjectResolver,
# IUserInfoProvider ports. Depends only on libs/common (arch-guard
# enforced: never on libs/oauth2). Added BEFORE libs/storage-postgres
# because storage-postgres's PostgresIdentityRepository (Task 19)
# implements identity's repository interfaces and therefore links
# fulla::identity.
add_subdirectory(${FULLA_PATH_LIBS_IDENTITY_DIR})

# libs/storage-postgres (fulla::storage::postgres, M2b Task 18):
# drogon::orm-generated ORM model classes, relocated out of OAuth2Plugin
# (design.md §4.1/§5.5: ORM models must not live in the Domain layer;
# this is their designated Adapter-layer home). OAuth2Plugin now depends
# on this package instead of owning the models itself.
add_subdirectory(${FULLA_PATH_LIBS_STORAGE_POSTGRES_DIR})

# libs/storage-memory (fulla::storage::memory, M8 Task 40): in-memory
# repository implementations relocated out of OAuth2Plugin/storage/ (design.md
# §5.4/§5.5). Pure-Domain adapters (std + jsoncpp, zero Drogon). Added BEFORE
# OAuth2Plugin because the plugin now links this package (the Memory
# RepositoryBundle is the memory-storage backend selected by storage_type=memory).
add_subdirectory(${FULLA_PATH_LIBS_STORAGE_MEMORY_DIR})

# libs/storage-redis (fulla::storage::redis, Phase 2 of the oauth2 storage
# refactor): Redis-backed oauth2 repository implementations
# (RedisRepositoryBase / Redis{Client,Grant,Token,Consent}Repository /
# RedisRepositoryBundle) + CachedClientRepository (cache decorator), relocated
# out of the old plugin's storage/. Added BEFORE libs/drogon because the
# plugin class there links this package (RedisRepositoryBundle is the
# redis-storage backend selected by storage_type=redis).
add_subdirectory(${FULLA_PATH_LIBS_STORAGE_REDIS_DIR})

# libs/drogon (fulla::drogon, M3 Task 20): Adapter-layer package for
# Drogon-coupled code (plugin/controllers/filters/views). Phase 4 (A6):
# absorbed the former OAuth2Plugin OBJECT library (plugin class +
# IdentityService); the OAuth2Plugin/ directory and its add_subdirectory
# are gone. Added AFTER the Domain-layer and storage libs above, whose
# CMake targets it links.
add_subdirectory(${FULLA_PATH_LIBS_DROGON_DIR})

add_subdirectory(${FULLA_PATH_FULLA_SERVER_DIR})

# tests/ (Phase 7): repo-level test tree (unit/contract/integration/
# e2e-backend/security/performance) — single DROGON_TEST binary
# (fulla-tests) that compiles apps/server sources directly. Mounted
# after apps/server so all lib targets it links are defined.
add_subdirectory(tests)

# examples/third-party-host (B8b / Task 28b, design.md §1.1/§6): minimal SDK
# consumer that links ONLY the fulla SDK packages (no libs/drogon /
# libs/storage-postgres) and drives the authorization-code flow
# through the Domain-layer engine -- the smoke gate proving the SDK is
# independently consumable. Gated by BUILD_EXAMPLES (default ON so CI runs it).
option(BUILD_EXAMPLES "Build the SDK consumer examples" ON)
if(BUILD_EXAMPLES)
    add_subdirectory(examples/third-party-host)
endif()

# examples/full-stack-host (Task 28a, design.md §1.1/§6): the FULL-STACK SDK
# smoke. Unlike third-party-host (engine-only, in-tree ALIAS link), this is a
# standalone project that consumes the ENTIRE product stack through real
# find_package against THIS build's build-tree package configs (written by
# fulla_package() under ${CMAKE_BINARY_DIR}/fulla-cmake), boots an HTTP
# server reusing the product controllers/OAuth2Plugin/views, and drives the
# authorization-code flow. It is NOT add_subdirectory'd; it is configured +
# built + run by `ctest --build-and-test` so the true external-consumer
# find_package path is exercised. The Conan toolchain is forwarded so Drogon/
# OpenSSL resolve identically; the fulla-cmake root is added to
# CMAKE_PREFIX_PATH so fulla-drogon + its find_dependency closure resolve.
# Tagged LABELS "SdkSmoke" (the label M8 Task 39 depends on).
if(BUILD_EXAMPLES)
    set(_sdk_smoke_gen_platform "")
    if(CMAKE_GENERATOR_PLATFORM)
        set(_sdk_smoke_gen_platform --build-generator-platform "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    add_test(
        NAME SdkSmoke.FullStack
        COMMAND ${CMAKE_CTEST_COMMAND}
            --build-and-test
                "${CMAKE_SOURCE_DIR}/examples/full-stack-host"
                "${CMAKE_BINARY_DIR}/examples/full-stack-host"
            --build-generator "${CMAKE_GENERATOR}"
            ${_sdk_smoke_gen_platform}
            --build-config "$<CONFIG>"
            --build-options
                "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
                "-DCMAKE_PREFIX_PATH=${CMAKE_BINARY_DIR}/fulla-cmake"
            --test-command "${CMAKE_CTEST_COMMAND}" -C "$<CONFIG>" --output-on-failure
    )
    set_tests_properties(SdkSmoke.FullStack PROPERTIES LABELS "SdkSmoke")
endif()
