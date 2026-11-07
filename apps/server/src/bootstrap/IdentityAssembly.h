#pragma once

// Task 24 slice 4 (fulla-sdk-refactor, design.md §6 "apps/server/src/
// main.cc # 仅装配：读配置 → 构造实现 → 注入端口 → run"): constructs the
// identity-layer services (fulla::identity::AuthService +
// SessionManager) that were previously only unit-tested (Task 19) and
// wires them into SessionController, replacing its pre-Task-24 direct use
// of the legacy fulla::drogon::services::AuthService (static,
// Mapper<Users>-backed) and the sendBackchannelLogoutNotifications() stub.
//
// Mirrors bootstrap::wireControllerPluginDependencies()'s own
// setter-injection-in-a-registerBeginningAdvice-callback pattern (Task 23)
// -- see this .cc file's own top comment for why identity assembly needs
// the same ordering (a real DB client, not just a constructed plugin).

#include <json/json.h>

namespace bootstrap
{

// Constructs fulla::storage::postgres::PostgresIdentityRepository (an
// IUserRepository/IRoleRepository/ISubjectMappingRepository
// implementation) against drogon::app().getDbClient(), then
// fulla::identity::AuthService + SessionManager on top of it, and
// injects both into the already-registered SessionController singleton
// via setIdentityAuthService()/setSessionManager().
//
// Ordering requirement: MUST be called from inside a
// drogon::app().registerBeginningAdvice() callback, same as
// wireControllerPluginDependencies() (see that function's header comment)
// -- drogon::app().getDbClient() is only guaranteed available once
// app().run() has processed the db_clients config block.
//
// Safe to call when no default DB client is configured (e.g.
// config.ci.json's empty db_clients, or memory-storage-only test runs):
// logs a warning and returns without wiring anything, leaving
// SessionController on its pre-Task-24 fallback path (see
// SessionController.h's setIdentityAuthService()/setSessionManager()
// comments).
void wireIdentityServices(const Json::Value &runtimeConfig);

}  // namespace bootstrap
