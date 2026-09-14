cmake_minimum_required(VERSION 3.16)

# libs/drogon (fulla::drogon) -- M3 Task 20, fulla-sdk-refactor
# (design.md §6 "libs/drogon # fulla::drogon [drogon 绑定] ...
# 插件(装配器)/controllers/filters/views"; §5.5 "Drogon 绑定 |
# fulla::drogon | libs/drogon | 是 | Drogon 插件（DI 装配器）、
# controller、filter、view；含 Drogon 自注册符号，消费者链接须
# whole-archive").
#
# This is the Adapter-layer package that holds Drogon-coupled code (the
# plugin degraded to a thin assembler, controllers, filters, views) that
# libs/common/libs/oauth2/libs/identity (the Domain layer) are forbidden
# from depending on. Unlike those three, THIS package is explicitly
# ALLOWED to depend on Drogon (design.md §4.1 rule 1 only forbids Drogon
# in the Domain layer; libs/drogon and libs/storage-* are the designated
# Adapter-layer homes for it).
#
# Scope note (Task 20 slice 1): only the three oauth2::validation::*
# classes (Rules/RuleEngine/RuleSet/HttpResponder) are relocated so far --
# these have zero dependency on the OAuth2Plugin singleton
# (drogon::app().getPlugin<OAuth2Plugin>()) or any Domain service, so they
# are the lowest-risk first slice of the M3 Drogon-binding migration
# (mirrors Task 14/17's own "smallest independently-buildable increment"
# discipline, design.md's H6 guidance). The controllers/filters/plugin
# itself (which DO reach through the OAuth2Plugin singleton) are follow-up
# slices, coordinated with Task 21 (plugin registration) / Task 23
# (de-singletonization) so the migration order doesn't create a
# circular/premature dependency on work those tasks haven't done yet.
#
# Namespace: fulla::drogon::validation (design.md §6 namespace sync).
# The old oauth2::validation:: location becomes a compatibility shim (same
# pattern as libs/oauth2's JwkManager.h shim from Task 17 slice 10) so
# existing call sites (RequestValidationFilter, OAuth2StandardController,
# SessionController, and the unit tests under
# tests/unit/validation/) keep compiling unchanged until the
# full M8 namespace sweep (Task 40).
#
# Phase 3 (A4+A5) done: error/filters/validation/adapters/utils/
# observability all live in this target (or libs/common) now.
# Phase 4 (A6) done: the OAuth2Plugin class itself (plugin/ + the
# IdentityService it assembles, services/) migrated in from the old
# OAuth2Plugin/ OBJECT library, which is deleted; its link edge here is
# gone and the dependencies it used to carry transitively (storage
# bundles, common, oauth2, OpenSSL, CURL) are now linked explicitly
# below. Class name ::OAuth2Plugin stays in the global namespace --
# config.json plugin reflection ("name": "OAuth2Plugin") depends on it.

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

project(fulla-drogon VERSION ${FULLA_PROJECT_VERSION} LANGUAGES CXX)

find_package(Drogon CONFIG REQUIRED)
# Phase 4 (A6): the plugin sources absorbed from the old OAuth2Plugin
# OBJECT library use OpenSSL (crypto/TOTP) and libcurl (SMTP transport in
# SmtpEmailService); these were found by OAuth2Plugin/CMakeLists.txt
# before, now found here.
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

add_library(${PROJECT_NAME} STATIC)
add_library(fulla::drogon ALIAS ${PROJECT_NAME})

# Compile-time version string for the generated OpenAPI info block and the
# /health example (single source: cmake/Version.cmake via FULLA_PROJECT_VERSION).
target_compile_definitions(${PROJECT_NAME} PRIVATE
  FULLA_VERSION_TEXT="${FULLA_PROJECT_VERSION}"
)

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# M5 Task 31 (fulla-sdk-refactor, F9): conditional compilation for the
# optional social-login + WebAuthn controllers. Declared here too (no-op if
# the top-level CMakeLists.txt already set them) so this library still
# configures standalone. When OFF, the corresponding controllers are excluded
# from compilation AND the WITH_* compile definition is withheld, so their
# `#ifdef WITH_*` bodies are inactive even if a source slips through.
option(WITH_SOCIAL "Build social-login (Google/WeChat/GitHub) controllers" ON)
option(WITH_WEBAUTHN "Build the WebAuthn/FIDO2 controller" ON)

file(GLOB_RECURSE FULLA_DROGON_SOURCES "src/*.cc")

# Task 31: drop the optional social + WebAuthn controller sources when their
# feature is disabled (the .cc files already carry `#ifdef WITH_SOCIAL` bodies,
# but excluding them entirely is cleaner -- no dead TU, smaller binary, and
# the compile definition below keeps headers' #ifdef guards consistent).
foreach(_src ${FULLA_DROGON_SOURCES})
    get_filename_component(_src_dir "${_src}" DIRECTORY)
    get_filename_component(_src_name "${_src}" NAME)
    if((NOT WITH_SOCIAL) AND _src_name MATCHES "^(Google|WeChat|GitHub)Controller\\.cc$")
        list(REMOVE_ITEM FULLA_DROGON_SOURCES "${_src}")
    elseif((NOT WITH_WEBAUTHN) AND _src_name STREQUAL "WebAuthnController.cc")
        list(REMOVE_ITEM FULLA_DROGON_SOURCES "${_src}")
    endif()
endforeach()

target_sources(${PROJECT_NAME} PRIVATE ${FULLA_DROGON_SOURCES})

# Task 31: surface WITH_SOCIAL / WITH_WEBAUTHN as compile definitions so the
# controllers' `#ifdef WITH_*` guards see a consistent on/off decision across
# the whole translation unit (PUBLIC so consumers of this static lib that
# include these controller headers also see the same guards). When the option
# is OFF the definition is withheld and the guarded code compiles out.
if(WITH_SOCIAL)
    target_compile_definitions(${PROJECT_NAME} PUBLIC WITH_SOCIAL)
endif()
if(WITH_WEBAUTHN)
    target_compile_definitions(${PROJECT_NAME} PUBLIC WITH_WEBAUTHN)
endif()

# Phase 4 (A6): OAuth2Plugin's former link set, inherited explicitly now
# that the OBJECT library is merged into this STATIC target (the link
# edge and its transitive deps died with it):
#   - fulla::storage::{postgres,memory,redis}: OAuth2Plugin.cc's
#     initStorage() constructs all three bundles.
#   - fulla::common / fulla::oauth2: adapters + services here
#     implement common::ports and call oauth2 domain code (pkce, consent).
#   - OpenSSL::Crypto / CURL::libcurl: crypto utils + SMTP email service.
#
# Task 24 slice 5: DrogonOAuthHttpClient.cc implements
# fulla::identity::IOAuthHttpClient -- fulla::identity linked
# explicitly (not via the storage-postgres transitive chain).
target_link_libraries(${PROJECT_NAME} PUBLIC
    Drogon::Drogon
    OpenSSL::Crypto
    CURL::libcurl
    fulla::common
    fulla::oauth2
    fulla::identity
    fulla::storage::postgres
    fulla::storage::memory
    fulla::storage::redis
)

# Conan provides a STATIC libcurl by default. On Windows the curl headers
# decorate symbols with __declspec(dllimport) UNLESS CURL_STATICLIB is
# defined, which otherwise yields unresolved __imp_curl_* symbols when
# linking the static lib. PUBLIC so consumers that include curl headers
# transitively also get it. No-op on non-Windows. (Moved verbatim from
# OAuth2Plugin/CMakeLists.txt in Phase 4.)
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PUBLIC CURL_STATICLIB)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Silence MSVC C4996 for std::getenv (OAuth2Plugin's FULLA_ENV production
# gates read the raw environment exactly like JwkManager does). PRIVATE so
# the definition never leaks into the exported SDK interface -- same
# convention as libs/common and libs/oauth2.
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

oauth2_apply_compat(${PROJECT_NAME})
oauth2_apply_warnings(${PROJECT_NAME})
oauth2_apply_gcov(${PROJECT_NAME})
oauth2_apply_sanitizer(${PROJECT_NAME})  # #104: instrument product code in sanitizer builds

include(GNUInstallDirs)
install(DIRECTORY include/fulla DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# SDK packaging: install-tree + build-tree find_package() config carrying the
# transitive find_dependency() closure (fulla-sdk-refactor Task 28a).
if(NOT COMMAND fulla_package)
    include(FullaPackage)
endif()
fulla_package(
    TARGET ${PROJECT_NAME}
    PACKAGE fulla-drogon
    EXPORT_NAME drogon
    DEPENDENCIES
        Drogon OpenSSL CURL
        fulla-common fulla-oauth2 fulla-identity
        fulla-storage-postgres fulla-storage-memory fulla-storage-redis
)

# --- Adapter-layer unit tests (link Drogon via fulla::drogon; no DB) ---
add_subdirectory(test)
