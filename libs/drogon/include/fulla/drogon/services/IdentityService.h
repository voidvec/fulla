#pragma once

// Phase 1.5d (Task 39): the 3 identity-side repos now use the NEW
// fulla::identity::* interfaces (Memory/Postgres impls widened in
// 1.5a-c to carry every method this service calls).
#include <fulla/identity/IRoleRepository.h>
#include <fulla/identity/IUserRepository.h>
#include <fulla/identity/ISubjectMappingRepository.h>
#include <fulla/oauth2/repository/IConsentRepository.h>
#include <fulla/oauth2/model/UserRef.h>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <functional>
#include <optional>

namespace fulla::identity
{

// Defect 1.9 fix (async-chain dangling `this`): IdentityService inherits
// std::enable_shared_from_this so its asynchronous storage chains
// (ensureSubjectMapping / handleFirstTimeLogin / validateUserRolesForScopes)
// can capture `auto self = shared_from_this();` at the outermost async call and
// thread that same `self` through every nested continuation, keeping the
// service alive until the in-flight callback completes (no use-after-free on
// teardown). The service is always created via std::make_shared
// (OAuth2Plugin::initAndStart), so shared_from_this() is valid at runtime. The
// synchronous pure-function call site (scopeRequiresAdminRole via a
// stack-constructed IdentityService({}) temporary) never calls
// shared_from_this(), so it keeps working without shared ownership.
//
// Phase 4.6a (fulla-sdk-refactor): this service is no longer keyed on the
// god IOAuth2Storage facade. It now holds the four identity-side split
// repositories (role / user / subject-mapping / consent). The consent repo is
// the NEW fulla::oauth2::repository::IConsentRepository (UserRef-based);
// the int32 internalUserId exposed by this service's API is wrapped into a
// UserRef at the boundary. Phase 1.5d (Task 39): the other three repos now
// use the NEW fulla::identity::* interfaces.
class IdentityService : public std::enable_shared_from_this<IdentityService>
{
  public:
    struct Repos
    {
        std::shared_ptr<fulla::identity::IRoleRepository> role;
        std::shared_ptr<fulla::identity::IUserRepository> user;
        std::shared_ptr<fulla::identity::ISubjectMappingRepository> subjectMapping;
        std::shared_ptr<fulla::oauth2::repository::IConsentRepository> consent;
    };

    // A default-constructed Repos (all empty) is accepted for the pure-function
    // call sites (e.g. OAuth2Plugin::scopeRequiresAdminRole via
    // IdentityService({})).
    explicit IdentityService(Repos repos);

    void getUserRoles(
      const std::string &userId,
      std::function<void(std::vector<std::string>)> &&callback
    );

    void ensureSubjectMapping(
      const std::string &subject,
      const std::string &username,
      int32_t internalUserId,
      std::function<void()> &&callback
    );

    void handleFirstTimeLogin(
      const std::string &subject,
      const std::string &provider,
      std::function<void(int32_t)> &&callback
    );

    void getInternalUserId(
      const std::string &subject,
      std::function<void(std::optional<int32_t>)> &&callback
    );

    void hasUserConsent(
      int32_t internalUserId,
      const std::string &clientId,
      const std::string &scope,
      std::function<void(bool)> &&callback
    );

    void saveUserConsent(
      int32_t internalUserId,
      const std::string &clientId,
      const std::string &scope,
      std::function<void(bool)> &&callback
    );

    void validateUserRolesForScopes(
      const std::string &userId,
      const std::vector<std::string> &scopes,
      std::function<void(bool, std::string)> &&callback
    );

    bool scopeRequiresAdminRole(const std::string &scope);

    // ------------------------------------------------------------------
    // P1-8 (#285) chain② data source: the DB-loaded admin-class set pushed
    // by OAuth2Plugin's startup load (same source AuthorizationService
    // consumes via setAdminScopes). Signature-preserving re-point; the
    // compile-time default above answers before the first push / memory.
    // ------------------------------------------------------------------
    static void setAdminScopeClass(std::unordered_set<std::string> scopes);

  private:
    static std::unordered_set<std::string> &adminScopeClassSet();
    static std::shared_mutex &adminScopeClassMutex();

  private:
    Repos repos_;
};

}  // namespace fulla::identity
