cmake_minimum_required(VERSION 3.16)

# libs/storage-memory (fulla::storage::memory) -- M8 Task 40 (fulla-sdk-refactor,
# design.md §5.4/§5.5/§6). Holds the 4 in-memory oauth2 repository implementations
# (Memory{Client,Grant,Token,Consent}Repository implementing
# fulla::oauth2::repository::*) relocated out of OAuth2Plugin/storage/. The 3
# identity memory repos (Memory{Role,User,SubjectMapping}Repository, which implement
# the legacy ::oauth2::I*Repository interfaces still owned by OAuth2Plugin) and the
# MemoryRepositoryBundle aggregator STAY under OAuth2Plugin/storage/ until Task 39
# migrates the identity interfaces into libs/identity (moving them now would create a
# circular storage-memory <-> OAuth2Plugin dependency).
#
# These 4 repos use Drogon's LOG_* macros for diagnostics, so this package links
# Drogon (like storage-postgres).

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
if(NOT DEFINED FULLA_PROJECT_VERSION)
    set(FULLA_PROJECT_VERSION 1.3.2)
endif()
if(NOT COMMAND oauth2_apply_compat)
    include(Compatibility)
endif()
if(NOT COMMAND oauth2_apply_warnings)
    include(Warnings)
endif()

project(fulla-storage-memory VERSION ${FULLA_PROJECT_VERSION} LANGUAGES CXX)

find_package(Drogon CONFIG REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)

add_library(${PROJECT_NAME} STATIC)
add_library(fulla::storage::memory ALIAS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

file(GLOB FULLA_STORAGE_MEMORY_SOURCES "src/*.cc")
target_sources(${PROJECT_NAME} PRIVATE ${FULLA_STORAGE_MEMORY_SOURCES})

# Memory repositories implement fulla::oauth2::repository::* interfaces
# (IClientRepository/IGrantRepository/ITokenRepository/IConsentRepository), so this
# package depends on the Domain-layer libs/oauth2 (correct direction per design.md
# §4.1: an Adapter-layer storage package is free to implement Domain interfaces;
# the Domain has zero dependency back).
#
# Phase 1.5c (Task 39): MemoryIdentityRepository implements the identity
# repository interfaces (IUser/IRole/ISubjectMapping), so this package also
# depends on libs/identity (same Adapter->Domain direction).
target_link_libraries(${PROJECT_NAME} PUBLIC Drogon::Drogon JsonCpp::JsonCpp fulla::oauth2 fulla::identity)

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

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
    PACKAGE fulla-storage-memory
    EXPORT_NAME storage::memory
    DEPENDENCIES Drogon jsoncpp fulla-oauth2 fulla-identity
)
