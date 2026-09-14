cmake_minimum_required(VERSION 3.16)

# libs/common (fulla::common) -- Task 13, fulla-sdk-refactor
# (design.md §4.1/§5.1/§6). Domain-layer shared kernel: Result<T,Error>,
# framework-agnostic ErrorCatalog/ErrorEnvelope, value objects (Subject,
# Scope, ClientId, RedirectUri, PkceChallenge, TokenValue, TenantId), ports
# (ISubjectResolver, IRoleProvider, IUserInfoProvider, IClock,
# ICryptoProvider, IUuidGenerator, IEmailSender, ILogger, IMetrics), and the
# AuditEvent observability model.
#
# Dependency rule (design.md §4.1 rule 1): this target MUST NOT depend on
# Drogon. It is allowed to depend on jsoncpp (Json::Value appears in
# Error::toJson/AuditEvent::details/IUserInfoProvider's claims bag).
#
# Self-contained bootstrap: mirrors OAuth2Plugin/CMakeLists.txt's pattern so
# this directory can also be added standalone (e.g. from
# examples/third-party-host in a later milestone) without requiring the
# repo root's shared CMake modules to already be included.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
if(NOT DEFINED FULLA_PROJECT_VERSION)
    set(FULLA_PROJECT_VERSION 1.2.0)
endif()
if(NOT COMMAND oauth2_apply_compat)
    include(Compatibility)
endif()
if(NOT COMMAND oauth2_apply_warnings)
    include(Warnings)
endif()

project(fulla-common VERSION ${FULLA_PROJECT_VERSION} LANGUAGES CXX)

find_package(jsoncpp CONFIG REQUIRED)

add_library(${PROJECT_NAME} STATIC)
add_library(fulla::common ALIAS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

file(GLOB_RECURSE FULLA_COMMON_SOURCES "src/*.cc")
target_sources(${PROJECT_NAME} PRIVATE ${FULLA_COMMON_SOURCES})

target_link_libraries(${PROJECT_NAME} PUBLIC JsonCpp::JsonCpp)

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Silence MSVC C4996 for std::getenv (ConfigManager/ErrorContext). PRIVATE so
# the definition never leaks into the exported SDK interface.
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Compiler compatibility (shared module) -- same convention as
# OAuth2Plugin/OAuth2Server, even though libs/common's own sources do not
# use ORM-generated code; keeps compile flags (e.g. /utf-8 on MSVC)
# consistent across every target in the tree.
oauth2_apply_compat(${PROJECT_NAME})

# First-party warning level (shared module); PRIVATE, no SDK-consumer leak.
oauth2_apply_warnings(${PROJECT_NAME})
# gcov instrumentation (no-op unless -DFULLA_TEST_COVERAGE=ON); PRIVATE.
oauth2_apply_gcov(${PROJECT_NAME})
oauth2_apply_sanitizer(${PROJECT_NAME})  # #104: instrument product code in sanitizer builds

# --- arch-guard note (enforced by tooling in a later M6 task, Task 33) ---
# This target must never gain a Drogon dependency. There is no automated
# check yet (tools/arch-guard is a later milestone); until then, this is a
# manually-enforced rule reviewers should watch for on any PR touching
# libs/common.

include(GNUInstallDirs)
install(DIRECTORY include/fulla DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# SDK packaging: install-tree + build-tree find_package() config carrying the
# transitive find_dependency() closure (fulla-sdk-refactor Task 28a).
if(NOT COMMAND fulla_package)
    include(FullaPackage)
endif()
fulla_package(
    TARGET ${PROJECT_NAME}
    PACKAGE fulla-common
    EXPORT_NAME common
    DEPENDENCIES jsoncpp
)

# --- Tests (pure unit, no DB/no Drogon; design.md §6's libs/common/test/) ---
add_subdirectory(test)
