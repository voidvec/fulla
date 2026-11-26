cmake_minimum_required(VERSION 3.16)

# examples/third-party-host (B8b / Task 28b, fulla-sdk-refactor design.md
# §1.1/§6): a MINIMAL standalone consumer that proves the fulla SDK is
# independently consumable via find_package + engine assembly -- without the
# product's OAuth2Plugin / libs/drogon / libs/storage-postgres. It links ONLY
# the SDK packages (fulla-oauth2 / common / common::testing /
# storage-memory) and programmatically drives the authorization-code flow's
# core steps through the Domain-layer AuthorizationService + TokenService.
#
# This is NOT an HTTP server (that heavier end-to-end validation is deferred).
# It is a plain executable whose exit code (0 = pass, 1 = fail) is the smoke
# signal that the SDK assembles and runs.
#
# Linking note: in-tree this links the ALIAS targets (fulla::oauth2 etc.),
# exactly as libs/oauth2/test does. A real external consumer would use
# find_package(fulla-oauth2) against an installed prefix -- the SDK
# packages' install(EXPORT) (see each lib's CMakeLists.txt) already provides
# that config. Both paths exercise the same SDK surface; this in-tree form is
# what CI runs as the smoke gate.
#
# Build/run:
#   cmake --preset windows-msvc
#   cmake --build --preset windows-msvc --config Debug --target third-party-host-smoke
#   ./build/windows-msvc/examples/third-party-host/Debug/third-party-host-smoke
#   echo $?   # 0 = smoke passed

project(third-party-host-smoke LANGUAGES CXX)

add_executable(${PROJECT_NAME} src/main.cc)

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    fulla::oauth2
    fulla::common
    fulla::common::testing
    fulla::storage::memory
)

oauth2_apply_sanitizer(${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
