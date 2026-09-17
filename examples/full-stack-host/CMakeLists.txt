cmake_minimum_required(VERSION 3.16)

# examples/full-stack-host (Task 28a, fulla-sdk-refactor design.md §1.1/§6):
# the STANDALONE full-stack SDK smoke. Unlike examples/third-party-host (Task
# 28b -- a minimal engine-only consumer that links a handful of SDK packages
# in-tree and drives the flow through the Domain layer with no HTTP), this
# example is a real HTTP server that consumes the ENTIRE verified product
# stack via find_package (build-tree export), reusing the product's
# controllers, OAuth2Plugin and views:
#
#   find_package(fulla-drogon) -> fulla::drogon, which PUBLIC-carries
#   the whole closure (common / oauth2 / identity / storage-* / Drogon /
#   OpenSSL / CURL). No install step: the build-tree Config files written by
#   fulla_package() (see cmake/FullaPackage.cmake) are resolved through
#   CMAKE_PREFIX_PATH -> ${top-build}/fulla-cmake.
#
# This project is NOT add_subdirectory'd into the main tree. It is built and
# run by the "SdkSmoke.FullStack" CTest entry via `ctest --build-and-test`
# (registered in the top-level CMakeLists), which points CMAKE_PREFIX_PATH at
# the build-tree package root -- exercising the real find_package consumption
# path an external product would use.

project(fulla-full-stack-smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Consume the product stack. fulla-drogon transitively pulls the rest of
# the fulla closure (its Config's find_dependency block). Drogon itself is
# needed directly here for drogon_create_views().
find_package(fulla-drogon CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# apps/server product-level assembly sources reused by the smoke. These are
# NOT part of the SDK (they are the product's composition root): the controller
# registration + identity wiring + the product OrganizationController that
# registerAllControllers() references. Compiled directly, exactly as the
# tests/ target does (see tests/CMakeLists.txt).
set(SERVER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/server/src")
add_executable(${PROJECT_NAME}
    src/main.cc
    "${SERVER_SRC_DIR}/bootstrap/ControllerRegistration.cc"
    "${SERVER_SRC_DIR}/bootstrap/IdentityAssembly.cc"
    "${SERVER_SRC_DIR}/organization/OrganizationController.cc"
    "${SERVER_SRC_DIR}/organization/OrganizationService.cc"
    # v1.4.0: open platform + org membership (registered through
    # ControllerRegistration.cc, so compiled here too).
    "${SERVER_SRC_DIR}/organization/OrgMemberController.cc"
    "${SERVER_SRC_DIR}/organization/OrgMemberService.cc"
    "${SERVER_SRC_DIR}/openplatform/ApplicationController.cc"
    "${SERVER_SRC_DIR}/openplatform/ApplicationService.cc"
)

# Compile the product views (login.csp/consent.csp) into the binary, so the
# view-rendering assertion needs no on-disk view files at runtime.
drogon_create_views(${PROJECT_NAME}
    "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/server/views"
    "${CMAKE_CURRENT_BINARY_DIR}")

target_include_directories(${PROJECT_NAME} PRIVATE
    "${SERVER_SRC_DIR}"
    "${SERVER_SRC_DIR}/organization"
    "${SERVER_SRC_DIR}/openplatform"
)

# fulla::drogon carries the whole PUBLIC closure; Drogon::Drogon +
# OpenSSL::Crypto are named explicitly because this target uses them directly
# (drogon_create_views output + the HTTP harness in main.cc).
target_link_libraries(${PROJECT_NAME} PRIVATE
    fulla::drogon
    Drogon::Drogon
    OpenSSL::Crypto
)

if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Stage the memory-backed smoke config next to the executable so main.cc's
# ./config.json lookup finds it.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config/smoke.json"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config.json"
)

# Inner CTest entry: run the smoke executable from its own directory (where the
# config.json copy lives). This is what the top-level SdkSmoke.FullStack test
# invokes through `ctest --build-and-test ... --test-command ctest` -- the
# generator expression here resolves the multi-config executable path.
enable_testing()
add_test(NAME smoke COMMAND ${PROJECT_NAME})
set_tests_properties(smoke PROPERTIES
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
