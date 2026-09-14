cmake_minimum_required(VERSION 3.16)

# libs/storage-redis (fulla::storage::redis) -- Phase 2 of the oauth2
# storage refactor (fulla-sdk-refactor, design.md §5.4/§5.5/§7.4).
# Holds the Redis-backed oauth2 repository implementations relocated out of
# OAuth2Plugin/{include,src}/oauth2/storage/:
#   - RedisRepositoryBase (mixin: holds redisClient_ + ctor takes redisClientName)
#   - RedisClientRepository / RedisGrantRepository / RedisTokenRepository /
#     RedisConsentRepository (4 oauth2 repos, implement
#     fulla::oauth2::repository::* interfaces)
#   - RedisRepositoryBundle (aggregates the 4; ctor takes redisClientName)
#   - CachedClientRepository (cache decorator implementing
#     fulla::oauth2::repository::IClientRepository; design.md §7.4 places
#     the cache in the redis layer)
#
# Namespace rewritten from `oauth2` to `fulla::storage::redis`; every bare
# `drogon::` reference globally qualified as `::drogon::` to avoid the M3
# namespace pitfall (inside `namespace fulla::storage::redis`, bare
# `drogon::` may otherwise resolve to `fulla::drogon::*` via the include
# graph; see libs/storage-postgres/include/fulla/storage/postgres/
# PostgresRepositoryBase.h's documented rationale for the same precaution).
#
# The 4 Redis repos implement fulla::oauth2::repository::* interfaces;
# CachedClientRepository implements fulla::oauth2::repository::
# IClientRepository. They have no identity dependency (pure oauth2), so deps
# are minimal: Drogon (LOG_* macros + RedisClient + CacheMap), jsoncpp, and
# libs/oauth2.

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

project(fulla-storage-redis VERSION ${FULLA_PROJECT_VERSION} LANGUAGES CXX)

find_package(Drogon CONFIG REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)

add_library(${PROJECT_NAME} STATIC)
add_library(fulla::storage::redis ALIAS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

file(GLOB FULLA_STORAGE_REDIS_SOURCES "src/*.cc")
target_sources(${PROJECT_NAME} PRIVATE ${FULLA_STORAGE_REDIS_SOURCES})

# Redis repositories implement fulla::oauth2::repository::* interfaces
# (IClientRepository/IGrantRepository/ITokenRepository/IConsentRepository) and
# CachedClientRepository implements IClientRepository; so this package depends
# on the Domain-layer libs/oauth2 (correct direction per design.md §4.1: an
# Adapter-layer storage package implements Domain interfaces; the Domain has
# zero dependency back).
target_link_libraries(${PROJECT_NAME} PUBLIC Drogon::Drogon JsonCpp::JsonCpp fulla::oauth2)

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
    PACKAGE fulla-storage-redis
    EXPORT_NAME storage::redis
    DEPENDENCIES Drogon jsoncpp fulla-oauth2
)
