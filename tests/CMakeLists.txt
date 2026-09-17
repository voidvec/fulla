cmake_minimum_required(VERSION 3.5)
project(fulla-tests CXX)

# Phase 7 (directory restructure, design.md §6 / Testing Strategy): the
# backend test suite moved from apps/server/test/ to this top-level tests/
# tree (contract/ integration/ e2e-backend/ security/ performance/ unit/).
# Single DROGON_TEST binary retained (user decision): physical layout is
# tiered, but all tiers compile into one executable so the ctest baseline
# stays directly comparable. Per-layer binary split is a follow-up task.

# Imported targets are directory-scoped: tests/ is now a SIBLING of
# apps/server (not its subdirectory), so Drogon::Drogon found there is no
# longer visible here -- find it in this scope (also provides
# drogon_create_views()).
find_package(Drogon CONFIG REQUIRED)

# Server sources consumed by the test binary (SchemaManager/bootstrap/
# organization) now live one tree away:
set(FULLA_SERVER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../apps/server/src")

# Optional flags
option(FULLA_MEMORY_TESTS_ONLY "Build only tests that do not require external databases" OFF)
option(FULLA_TEST_COVERAGE "Enable code coverage reporting" OFF)

# M4 Task 27 (fulla-sdk-refactor, design.md F6 / Testing Strategy):
# This test binary used to GLOB the plugin/server sources directly into
# itself (the Task 16 transitional bridge), masking linking-boundary
# problems and slowing the build. The test now consumes library products
# only: it links fulla::* below and no longer compiles any plugin or
# apps/server controller/filter sources directly. Whole-archive is
# not needed (AutoCreation=false controllers are explicitly registered).
# Phase 4 (A6): the OAuth2Plugin OBJECT library is gone -- the plugin
# class lives in fulla::drogon (STATIC) and its public headers under
# libs/drogon/include, so the former OAuth2Plugin/include path and the
# bare OAuth2Plugin link edge are dropped.

# SchemaManager: needed by the SQL-splitter unit test (unit/schema/).
# Compiled directly into the test target so the splitter can be exercised
# without a running database.
set(SCHEMA_MGR_SRC "${FULLA_SERVER_SRC_DIR}/SchemaManager.cc")

# M3 Task 23 (fulla-sdk-refactor): test_main.cc now calls
# bootstrap::wireControllerPluginDependencies() (see its own header comment),
# defined in ControllerRegistration.cc alongside registerAllControllers()
# (already called by test_main.cc directly, without going through this
# glob, before this task). Task 24 slice 4 adds IdentityAssembly.cc
# (test_main.cc also calls bootstrap::wireIdentityServices()). These two
# bootstrap/ files are needed by the test target; the others
# (CorsSetup/SecurityHeaders/etc) are apps/server-only concerns
# test_main.cc does not use.
set(BOOTSTRAP_SRC
    "${FULLA_SERVER_SRC_DIR}/bootstrap/AdminBootstrapper.cc"
    "${FULLA_SERVER_SRC_DIR}/bootstrap/ClientSeeder.cc"
    "${FULLA_SERVER_SRC_DIR}/bootstrap/ControllerRegistration.cc"
    "${FULLA_SERVER_SRC_DIR}/bootstrap/IdentityAssembly.cc"
)

# M5 Task 30: the product-level OrganizationController (apps/server/src/
# organization/) is registered by test_main.cc, so the test binary must
# compile it too (same pattern as SchemaManager/bootstrap above).
# Task 29b batch 3: OrganizationService extracted alongside (DB access via
# Mapper+Criteria); both compile into the test binary.
set(ORGANIZATION_SRC
    "${FULLA_SERVER_SRC_DIR}/organization/OrganizationController.cc"
    "${FULLA_SERVER_SRC_DIR}/organization/OrganizationService.cc"
)

# v1.4.0: open platform + org membership controllers register through
# ControllerRegistration.cc, so the test binary compiles them too (same
# pattern as ORGANIZATION_SRC above).
set(OPENPLATFORM_SRC
    "${FULLA_SERVER_SRC_DIR}/openplatform/ApplicationController.cc"
    "${FULLA_SERVER_SRC_DIR}/openplatform/ApplicationService.cc"
    "${FULLA_SERVER_SRC_DIR}/organization/OrgMemberController.cc"
    "${FULLA_SERVER_SRC_DIR}/organization/OrgMemberService.cc"
)

# Collect hierarchical test sources
file(GLOB_RECURSE UNIT_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc")
file(GLOB_RECURSE INTEGRATION_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc")
file(GLOB_RECURSE E2E_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/e2e-backend/*.cc")
file(GLOB_RECURSE SECURITY_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/security/*.cc")
file(GLOB_RECURSE PERFORMANCE_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/performance/*.cc")
# Task 12 (fulla-sdk-refactor, design.md §7.3 / F5): tiered repository
# contract tests (IClientRepository/IGrantRepository/ITokenRepository/
# IConsentRepository across Postgres/Redis/Memory). Phase 7: now at the
# design §6 target location tests/contract/.
file(GLOB_RECURSE CONTRACT_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/contract/*.cc")

# Root test sources
set(ROOT_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/test_main.cc"
)

if(NOT FULLA_MEMORY_TESTS_ONLY)
    list(APPEND ROOT_TEST_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/SchemaSetup.cc"
    )
endif()

# Combine all test sources
set(ALL_TEST_SOURCES
    ${ROOT_TEST_SOURCES}
    ${UNIT_TESTS}
    ${INTEGRATION_TESTS}
    ${E2E_TESTS}
    ${SECURITY_TESTS}
    ${PERFORMANCE_TESTS}
    ${CONTRACT_TESTS}
)

add_executable(${PROJECT_NAME}
    ${ALL_TEST_SOURCES}
    ${SCHEMA_MGR_SRC}
    ${BOOTSTRAP_SRC}
    ${ORGANIZATION_SRC}
    ${OPENPLATFORM_SRC}
)

drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/../apps/server/views
                    ${CMAKE_CURRENT_BINARY_DIR})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${FULLA_SERVER_SRC_DIR}
    ${FULLA_SERVER_SRC_DIR}/organization
    ${FULLA_SERVER_SRC_DIR}/openplatform
)

# Link libraries
find_package(OpenSSL REQUIRED)
# M4 Task 27: the test consumes library products only. Phase 4 (A6): the
# plugin symbols reach the test through fulla::drogon (which absorbed
# the old OAuth2Plugin OBJECT lib and PUBLIC-provides CURL::libcurl +
# CURL_STATICLIB transitively). Plain link, no whole-archive
# (AutoCreation=false + explicit registration in test_main.cc).
target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon OpenSSL::Crypto fulla::common fulla::oauth2 fulla::storage::postgres fulla::storage::memory fulla::drogon fulla::identity)

# Platform compatibility (shared module)
oauth2_apply_compat(${PROJECT_NAME})

# First-party warning level (shared module, included from the repo root)
oauth2_apply_warnings(${PROJECT_NAME})

# Optional TSan/ASan instrumentation (shared module).
# Controlled by the cache option FULLA_SANITIZER = off|thread|address.
# GCC/Clang + Debug only; TSan and ASan are mutually exclusive (two builds).
# The test executable compiles all plugin/server sources directly into itself,
# so instrumenting this single target covers all code exercised by the suite.
oauth2_apply_sanitizer(${PROJECT_NAME})

if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
endif()

# Repo-root documentation path for the ErrorCatalog doc validator test (task 11.1).
# The committed "通用错误码" section of docs/domains/api-reference.md is validated
# against ErrorCatalog::allEntries()/allOAuthEntries() at test time. The test runs
# from the test executable directory, so the absolute source path is injected here
# (CMake normalizes the path with forward slashes, usable by std::ifstream on all
# platforms). Defined as an absolute path derived from this CMakeLists location.
get_filename_component(FULLA_REPO_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    "FULLA_DOC_API_REFERENCE=\"${FULLA_REPO_ROOT_DIR}/docs/domains/api-reference.md\"")

# Repo-source golden-snapshot directory for the success-response / route-manifest
# regression test (task 12.4). The committed golden files under
# tests/integration/error/golden/ are read (and self-seeded on first
# run) at test time. Like FULLA_DOC_API_REFERENCE above, the test runs from the
# test executable directory, so the absolute source path is injected here.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    "FULLA_GOLDEN_DIR=\"${FULLA_REPO_ROOT_DIR}/tests/integration/error/golden\"")

# In-process DB seeding paths (coverage push, docs/history/design/
# http-integration-test-coverage-plan.md Phase 1 / B1 fix): the test binary
# needs to locate the migration SQL (apps/server/migrations/V*.sql) and the
# dev seed SQL (apps/server/seed/*.sql) at runtime so the in-process seeder
# (tests/common/StorageSeed.h) can apply them when a real Postgres is
# configured. Without this, admin integration tests cannot authenticate (no
# seeded admin user) outside the Linux CI shell step. Absolute paths derived
# from this CMakeLists location (forward slashes, usable by std::ifstream
# cross-platform). See FULLA_GOLDEN_DIR above for the same pattern.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    "FULLA_MIGRATIONS_DIR=\"${FULLA_REPO_ROOT_DIR}/apps/server/migrations\""
    "FULLA_SEED_DIR=\"${FULLA_REPO_ROOT_DIR}/apps/server/seed\"")

# Coverage support: delegate to oauth2_apply_gcov (cmake/Coverage.cmake).
# The instrumented first-party libraries (each lib calls oauth2_apply_gcov
# too) are linked into this binary, so their .gcda files are written when
# the tests run; this call additionally instruments the test-compiled
# sources (test_main.cc, SchemaManager.cc, bootstrap/*.cc, organization/*.cc).
if(FULLA_TEST_COVERAGE)
    oauth2_apply_gcov(${PROJECT_NAME})
endif()

add_test(NAME OAuth2Tests COMMAND ${PROJECT_NAME} WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>)

# --- Contract test suite (Task 12, design.md §7.3 / F5): CTest label "Contract" ---
#
# DROGON_TEST's own `-r <name>` test runner does an EXACT name match (see
# drogon_test.cc's `test->name() == targetTest`), not a prefix/substring
# match -- there is no way to select "every test whose name starts with
# Contract_" in one process invocation the way the naming convention might
# suggest. To still get a `ctest -L Contract` entry point (per the task's
# acceptance criterion), each contract DROGON_TEST case below is registered
# as its own individual add_test() (same pattern the rest of this project's
# CI already uses to run named tests individually, e.g. the
# "Run Error-Standardization Backend Tests" step in ci-linux.yml -- this
# just does the equivalent registration at the CMake/CTest level instead of
# a shell script loop), all tagged with LABELS "Contract" so `ctest -L
# Contract` runs exactly this set, `ctest -L Contract -E Postgres` can
# exclude backend tiers, etc.
set(FULLA_CONTRACT_TEST_NAMES
    # ClientRepositoryContractTest.cc
    Integration_P0_Contract_Functional_ClientRepository_Postgres_NotFoundReturnsNullopt
    Integration_P0_Contract_Functional_ClientRepository_Postgres_PublicClientAcceptsAnySecret
    Integration_P0_Contract_Functional_ClientRepository_Postgres_ConfidentialClientValidatesSecret
    Integration_P0_Contract_Functional_ClientRepository_Redis_NotFoundReturnsNullopt
    Integration_P0_Contract_Functional_ClientRepository_Redis_KnownClientValidatesSecret
    Integration_P0_Contract_Functional_ClientRepository_Memory_NotFoundReturnsNullopt
    Integration_P0_Contract_Functional_ClientRepository_Memory_PublicClientAcceptsAnySecret
    Integration_P0_Contract_Functional_ClientRepository_Memory_ConfidentialClientValidatesSecret
    # GrantRepositoryContractTest.cc
    Integration_P0_Contract_Functional_GrantRepository_Postgres_SaveGetRoundTrip
    Integration_P0_Contract_Functional_GrantRepository_Postgres_NotFoundReturnsNullopt
    Integration_P0_Contract_Functional_GrantRepository_Postgres_ConsumeAuthCode_CorrectRedirectUriSucceeds
    Integration_P0_Contract_Functional_GrantRepository_Postgres_ConsumeAuthCode_WrongRedirectUriFails
    Integration_P0_Contract_Functional_GrantRepository_Postgres_ConsumeAuthCode_SingleUse
    Integration_P0_Contract_Functional_GrantRepository_Redis_SaveGetRoundTrip
    Integration_P0_Contract_Functional_GrantRepository_Redis_NotFoundReturnsNullopt
    Integration_P0_Contract_Functional_GrantRepository_Redis_ConsumeAuthCode_CorrectRedirectUriSucceeds
    Integration_P0_Contract_Functional_GrantRepository_Redis_ConsumeAuthCode_WrongRedirectUriFails
    Integration_P0_Contract_Functional_GrantRepository_Redis_ConsumeAuthCode_SingleUse
    Integration_P0_Contract_Functional_GrantRepository_Memory_SaveGetRoundTrip
    Integration_P0_Contract_Functional_GrantRepository_Memory_NotFoundReturnsNullopt
    Integration_P0_Contract_Functional_GrantRepository_Memory_ConsumeAuthCode_CorrectRedirectUriSucceeds
    Integration_P0_Contract_Functional_GrantRepository_Memory_ConsumeAuthCode_WrongRedirectUriFails
    Integration_P0_Contract_Functional_GrantRepository_Memory_ConsumeAuthCode_SingleUse
    # TokenRepositoryContractTest.cc
    Integration_P0_Contract_Functional_TokenRepository_Postgres_AccessTokenSaveGetRoundTrip
    Integration_P0_Contract_Functional_TokenRepository_Postgres_AccessTokenNotFound
    Integration_P0_Contract_Functional_TokenRepository_Redis_AccessTokenSaveGetRoundTrip
    Integration_P0_Contract_Functional_TokenRepository_Redis_AccessTokenNotFound
    Integration_P0_Contract_Functional_TokenRepository_Memory_AccessTokenSaveGetRoundTrip
    Integration_P0_Contract_Functional_TokenRepository_Memory_AccessTokenNotFound
    Integration_P0_Contract_Functional_TokenRepository_Postgres_RefreshTokenSaveGetRoundTrip
    Integration_P0_Contract_Functional_TokenRepository_Memory_RefreshTokenSaveGetRoundTrip
    Integration_P0_Contract_Functional_TokenRepository_Redis_RefreshTokenSaveIsNoOp_GetAlwaysNullopt
    Integration_P0_Contract_Functional_TokenRepository_Postgres_RevokeRefreshToken_GetReturnsRevokedFlagTrue
    Integration_P0_Contract_Functional_TokenRepository_Memory_RevokeRefreshToken_GetReturnsNullopt
    Integration_P0_Contract_Functional_TokenRepository_Memory_ExpiredAccessToken_GetReturnsNullopt
    Integration_P0_Contract_Functional_TokenRepository_Postgres_ExpiredAccessToken_StillReturnedByGet_NoActiveExpiryCheck
    Integration_P0_Contract_Atomicity_TokenRepository_Postgres_AtomicRevokeRefreshToken_ConcurrentCas
    Integration_P0_Contract_Atomicity_TokenRepository_Memory_AtomicRevokeRefreshToken_ConcurrentCas
    Integration_P0_Contract_Atomicity_TokenRepository_Redis_AtomicRevokeRefreshToken_CasTierSkipped
    Integration_P0_Contract_Atomicity_TokenRepository_Postgres_SaveTokenPair_HappyPathBothWritesSucceed
    Integration_P0_Contract_Atomicity_TokenRepository_Postgres_SaveTokenPair_AccessTokenConflict_RefreshTokenNotPersisted
    Integration_P0_Contract_Atomicity_TokenRepository_Memory_SaveTokenPair_HappyPathBothWritesSucceed
    # ConsentRepositoryContractTest.cc
    Integration_P0_Contract_Functional_ConsentRepository_Postgres_SaveHasRevokeRoundTrip
    Integration_P0_Contract_Functional_ConsentRepository_Redis_SaveHasRevokeRoundTrip
    Integration_P0_Contract_Functional_ConsentRepository_Memory_SaveHasRevokeRoundTrip
    # UserInfoRepositoryContractTest.cc (遗留事项 L2: re-homed coverage of the
    # deleted tests/services/AuthServiceGetUserInfoTest.cc onto the live
    # IUserRepository::getUserInfoWithRoles path; Postgres/Memory only --
    # identity has no Redis implementation)
    Integration_P0_Contract_Functional_UserInfoRepository_Postgres_UserWithRoles_ReturnsClaimsAndRoles
    Integration_P0_Contract_Functional_UserInfoRepository_Postgres_NoRoles_EmptyRolesArray_NameFallsBackToEmail
    Integration_P0_Contract_Functional_UserInfoRepository_Postgres_UnknownUser_ReturnsNullopt
    Integration_P0_Contract_Functional_UserInfoRepository_Memory_MappedUser_SyntheticClaimsWithConfiguredRoles
    Integration_P0_Contract_Functional_UserInfoRepository_Memory_UnmappedUser_DefaultsToUserRole_NeverNullopt
    # Coverage additions (P1, fulla coverage push): the Memory backend's
    # split-repository methods not reached by the original contract suite --
    # revokeTokenFamily (security-critical reuse cascade), introspectToken
    # branches, revokeAccessToken audit fields, atomicRevokeRefreshToken
    # not-found/already-revoked, expired-refresh filtering, purgeExpired,
    # incrementIntrospectCount, AuthorizationTransaction CRUD, markAuthCodeUsed,
    # markTransactionConsumed, consumeAuthCode empty-redirectUri rejected when bound (F-009),
    # getAuthCode lazy eviction, MemoryClientRepository initFromConfig parsing
    # branches, MemoryIdentityRepository initAdminRoles + IUserRepository
    # placeholder impls. Memory-only (no DB gate) so they run unconditionally.
    # TokenRepository (Memory)
    Integration_P0_Contract_Functional_TokenRepository_Memory_RevokeTokenFamily_RevokesRefreshAndAccess
    Integration_P0_Contract_Functional_TokenRepository_Memory_Introspect_ActiveAccess
    Integration_P0_Contract_Functional_TokenRepository_Memory_Introspect_RevokedAccess
    Integration_P0_Contract_Functional_TokenRepository_Memory_Introspect_ExpiredAccess
    Integration_P0_Contract_Functional_TokenRepository_Memory_Introspect_NotFound
    Integration_P0_Contract_Functional_TokenRepository_Memory_Introspect_ActiveRefresh
    Integration_P0_Contract_Functional_TokenRepository_Memory_RevokeAccessToken_SetsAuditFieldsAndAlsoRevokesRefresh
    Integration_P0_Contract_Functional_TokenRepository_Memory_AtomicRevokeRefreshToken_NotFoundReturnsNullopt
    Integration_P0_Contract_Functional_TokenRepository_Memory_AtomicRevokeRefreshToken_AlreadyRevokedReturnsNullopt
    Integration_P0_Contract_Functional_TokenRepository_Memory_ExpiredRefreshToken_GetReturnsNullopt
    Integration_P0_Contract_Functional_TokenRepository_Memory_PurgeExpired_RemovesExpiredRetainsValid
    Integration_P0_Contract_Functional_TokenRepository_Memory_IncrementIntrospectCount_FiresCallbackForExistingAndMissing
    # GrantRepository (Memory)
    Integration_P0_Contract_Functional_GrantRepository_Memory_AuthorizationTransaction_SaveGetRoundTrip
    Integration_P0_Contract_Functional_GrantRepository_Memory_AuthorizationTransaction_Expired_GetReturnsNullopt_AndEvicts
    Integration_P0_Contract_Functional_GrantRepository_Memory_MarkTransactionConsumed_FirstTrue_SecondFalse
    Integration_P0_Contract_Functional_GrantRepository_Memory_MarkAuthCodeUsed_MissingNoOp_ExistingMarksUsed
    Integration_P0_Contract_Functional_GrantRepository_Memory_ConsumeAuthCode_EmptyRedirectUriRejectedWhenBound
    Integration_P0_Contract_Functional_GrantRepository_Memory_GetAuthCode_Expired_EvictedAndReturnsNullopt
    Integration_P0_Contract_Functional_GrantRepository_Memory_PurgeExpired_RemovesExpiredCodes
    # ClientRepository (Memory) initFromConfig parsing branches
    Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_RedirectUri_SingleString
    Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_AllowedScopes_SingleString
    Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_VueClient_DefaultScopes
    Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_InvalidType_DefaultsToConfidential
    Integration_P0_Contract_Functional_ClientRepository_Memory_InitFromConfig_NullConfig_NoOp
    # UserInfoRepository (Memory) initAdminRoles + placeholder impls
    Integration_P0_Contract_Functional_UserInfoRepository_Memory_InitAdminRoles_NullConfig_DefaultsAdminUser
    Integration_P0_Contract_Functional_UserInfoRepository_Memory_InitAdminRoles_RolesAsString
    Integration_P0_Contract_Functional_UserInfoRepository_Memory_FindById_SyntheticUser
    Integration_P0_Contract_Functional_UserInfoRepository_Memory_FindByPublicSub_NonNumeric_ReturnsNullopt
    Integration_P0_Contract_Functional_UserInfoRepository_Memory_FindByPublicSub_Numeric_SyntheticUser
    Integration_P0_Contract_Functional_UserInfoRepository_Memory_FindByEmailAndUsername_ReturnNullopt
    Integration_P0_Contract_Functional_UserInfoRepository_Memory_Create_ReturnsNulloptUnsupported
    Integration_P0_Contract_Functional_UserInfoRepository_Memory_PasswordAndLoginMethods_PlaceholderReturns
    # Coverage additions (P2/P3): Memory consent revoke-nonexistent no-op
    # and per-scope independence.
    Integration_P0_Contract_Functional_ConsentRepository_Memory_RevokeNonexistent_IsNoOp
    Integration_P0_Contract_Functional_ConsentRepository_Memory_PerScopeIndependence
)

foreach(_contract_test_name ${FULLA_CONTRACT_TEST_NAMES})
    add_test(
        NAME "Contract.${_contract_test_name}"
        COMMAND ${PROJECT_NAME} -r ${_contract_test_name}
        WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
    set_tests_properties("Contract.${_contract_test_name}" PROPERTIES LABELS "Contract")
endforeach()

# Copy config to the test executable directory
if(MSVC OR XCODE)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/server/config/config.json
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/config.json
    )
else()
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/server/config/config.json
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/config.json
    )
endif()

# ===========================================================================
# Out-of-process endpoint tests (方案 A): wraps the PowerShell/curl endpoint
# test scripts (formerly only reachable via full_test.bat/full-test.sh Step
# 6-7) into a single ctest entry. The wrapper (run-endpoint-tests.{ps1,sh})
# starts the fulla server, waits for health readiness, runs the OAuth2 +
# Admin endpoint test scripts against it, then stops the server.
#
# This ensures `ctest` alone exercises the full out-of-process HTTP stack --
# no more silent omissions of the endpoint test layer.
#
# Prerequisites (environment, not managed by ctest):
#   - PostgreSQL + Redis running with the DB migrated + seeded
#   - On Linux: jq installed
# ===========================================================================
set(_ENDPOINT_BASE_URL "http://127.0.0.1:5555")
set(_ENDPOINT_SCRIPTS_DIR "${CMAKE_SOURCE_DIR}/scripts/backend")

if(WIN32)
    set(_ENDPOINT_WRAPPER "${_ENDPOINT_SCRIPTS_DIR}/run-endpoint-tests.ps1")
    set(_SERVER_BIN "${CMAKE_BINARY_DIR}/apps/server/Release/fulla-server.exe")
    find_program(_POWERSHELL_BIN NAMES pwsh powershell)
    if(_POWERSHELL_BIN)
        add_test(NAME EndpointTests_OutOfProcess
            COMMAND ${_POWERSHELL_BIN} -NoProfile -ExecutionPolicy Bypass
                    -File "${_ENDPOINT_WRAPPER}"
                    -ServerExe "${_SERVER_BIN}"
                    -BaseUrl "${_ENDPOINT_BASE_URL}"
        )
        set(_ENDPOINT_TEST_REGISTERED TRUE)
    endif()
else()
    set(_ENDPOINT_WRAPPER "${_ENDPOINT_SCRIPTS_DIR}/run-endpoint-tests.sh")
    set(_SERVER_BIN "${CMAKE_BINARY_DIR}/apps/server/fulla-server")
    add_test(NAME EndpointTests_OutOfProcess
        COMMAND bash "${_ENDPOINT_WRAPPER}"
                --server-exe "${_SERVER_BIN}"
                --base-url "${_ENDPOINT_BASE_URL}"
    )
    set(_ENDPOINT_TEST_REGISTERED TRUE)
endif()
# Properties are set only if add_test actually ran. The server binary is
# NOT checked at configure time (it may not exist yet -- configure runs
# before build); the wrapper checks for it at test-run time.
if(_ENDPOINT_TEST_REGISTERED)
    set_tests_properties(EndpointTests_OutOfProcess PROPERTIES
        LABELS "Endpoint"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77  # wrapper returns 77 when PG/Redis/DB unavailable -> SKIPPED, not FAILED
    )
endif()
