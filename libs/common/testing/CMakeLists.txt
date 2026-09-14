cmake_minimum_required(VERSION 3.16)

# libs/common/testing (fulla::common::testing) -- Task 15,
# fulla-sdk-refactor (design.md §6/§8). Fake ("test double")
# implementations of every fulla::common::ports interface, kept in a
# SEPARATE library from both:
#   - libs/common itself (which only declares the ports, no implementations)
#   - the production Adapter implementations under
#     OAuth2Plugin/{include,src}/adapters/ (OpenSslCryptoProvider/
#     OpenSslUuidGenerator/SystemClock/DrogonLogger)
#
# Rationale (design.md: "生产实现放 Adapter；假实现放测试支持库"): production
# binaries must never link test doubles, and this test-support library must
# stay independent of OAuth2Plugin (which depends on Drogon) so that future
# standalone Domain packages (libs/oauth2, libs/identity -- M2b/M2.5) can
# depend on it for deterministic unit tests without ever pulling in Drogon,
# which is the actual acceptance criterion this task targets: "Domain 可用
# 假时钟/假 crypto 做确定性单测".
#
# Dependency rule: same as libs/common (design.md §4.1 rule 1) -- this
# target MUST NOT depend on Drogon. It depends on libs/common (for the
# port interfaces it implements) and OpenSSL (FakeCryptoProvider still
# performs REAL cryptographic operations for everything except
# secureRandomBytes -- see that class's own header comment for why "faking"
# SHA-256/HMAC/etc. would not be useful).

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake")
if(NOT DEFINED FULLA_PROJECT_VERSION)
    set(FULLA_PROJECT_VERSION 1.3.0)
endif()
if(NOT COMMAND oauth2_apply_compat)
    include(Compatibility)
endif()
if(NOT COMMAND oauth2_apply_warnings)
    include(Warnings)
endif()

project(fulla-common-testing VERSION ${FULLA_PROJECT_VERSION} LANGUAGES CXX)

find_package(OpenSSL REQUIRED)

add_library(${PROJECT_NAME} STATIC)
add_library(fulla::common::testing ALIAS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

file(GLOB_RECURSE FULLA_COMMON_TESTING_SOURCES "src/*.cc")
target_sources(${PROJECT_NAME} PRIVATE ${FULLA_COMMON_TESTING_SOURCES})

target_link_libraries(${PROJECT_NAME} PUBLIC fulla::common OpenSSL::Crypto)

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

oauth2_apply_compat(${PROJECT_NAME})
oauth2_apply_warnings(${PROJECT_NAME})
oauth2_apply_gcov(${PROJECT_NAME})

# --- arch-guard note (see libs/common/CMakeLists.txt's identical note) ---
# This target must never gain a Drogon dependency either -- manually
# enforced until tools/arch-guard exists (M6, Task 33).

include(GNUInstallDirs)
install(DIRECTORY include/fulla DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# SDK packaging: install-tree + build-tree find_package() config carrying the
# transitive find_dependency() closure (fulla-sdk-refactor Task 28a).
if(NOT COMMAND fulla_package)
    include(FullaPackage)
endif()
fulla_package(
    TARGET ${PROJECT_NAME}
    PACKAGE fulla-common-testing
    EXPORT_NAME common::testing
    DEPENDENCIES OpenSSL fulla-common
)

# --- Tests (pure unit, no DB/no Drogon) ---
add_subdirectory(test)
