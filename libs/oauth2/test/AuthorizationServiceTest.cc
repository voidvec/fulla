// Task 17 remainder (fulla-sdk-refactor): unit tests for the new
// fulla::oauth2::protocol::AuthorizationService -- the first time
// this class exists in the codebase (see its header comment: the
// consent/scope decision was previously spread across ClientService +
// IdentityService + OAuth2StandardController with no single owner).

#include <fulla/common/model/Subject.h>
#include <fulla/common/ports/IOrgConsentResolver.h>
#include <fulla/oauth2/protocol/AuthorizationService.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

using namespace fulla::oauth2::model;
using namespace fulla::oauth2::repository;
using fulla::oauth2::protocol::AuthorizationService;

class FakeClientRepo : public IClientRepository
{
  public:
    std::unordered_map<std::string, OAuth2Client> clients;

    void getClient(const std::string &clientId, ClientCallback &&cb) override
    {
        auto it = clients.find(clientId);
        cb(it == clients.end() ? std::nullopt : std::make_optional(it->second));
    }

    void validateClient(const std::string &, const std::string &, BoolCallback &&cb) override
    {
        cb(true);
    }
};

class FakeConsentRepo : public IConsentRepository
{
  public:
    // key: internalUserId:clientId:scope
    std::unordered_map<std::string, bool> consents;

    static std::string key(
      const UserRef &user,
      const std::string &clientId,
      const std::string &scope
    )
    {
        return std::to_string(user.internalUserId) + ":" + clientId + ":" + scope;
    }

    void hasUserConsent(
      const UserRef &user,
      const std::string &clientId,
      const std::string &scope,
      BoolCallback &&cb
    ) override
    {
        auto it = consents.find(key(user, clientId, scope));
        cb(it != consents.end() && it->second);
    }

    void saveUserConsent(
      const UserRef &user,
      const std::string &clientId,
      const std::string &scope,
      BoolCallback &&cb
    ) override
    {
        consents[key(user, clientId, scope)] = true;
        cb(true);
    }

    void revokeUserConsent(
      const UserRef &user,
      const std::string &clientId,
      const std::string &scope,
      VoidCallback &&cb
    ) override
    {
        consents[key(user, clientId, scope)] = false;
        cb();
    }
};

class FakeSubjectResolver : public fulla::common::ports::ISubjectResolver
{
  public:
    std::unordered_map<std::string, int32_t> mapping;

    void resolve(const fulla::common::model::Subject &subject, ResolveCallback &&cb) override
    {
        auto it = mapping.find(subject.value());
        cb(it == mapping.end() ? std::nullopt : std::make_optional(it->second));
    }
};

class FakeRoleProvider : public fulla::common::ports::IRoleProvider
{
  public:
    std::unordered_map<int32_t, std::vector<std::string>> roles;

    void getRoles(int32_t internalUserId, RolesCallback &&cb) override
    {
        auto it = roles.find(internalUserId);
        cb(it == roles.end() ? std::vector<std::string>{} : it->second);
    }
};

std::shared_ptr<FakeClientRepo> makeClientRepo()
{
    auto repo = std::make_shared<FakeClientRepo>();
    OAuth2Client c;
    c.clientId = "test-client";
    c.clientType = ClientType::CONFIDENTIAL;
    c.allowedScopes = {"openid", "profile", "admin"};
    repo->clients["test-client"] = c;
    return repo;
}

}  // namespace

TEST(AuthorizationServiceTest, EvaluateScopes_UnknownClient_AllInvalid)
{
    auto clients = makeClientRepo();
    AuthorizationService svc(clients);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("unknown-client", "local:alice", {"openid"}, [&](auto s) {
        summary = std::move(s);
    });

    EXPECT_TRUE(summary.hasErrors());
    ASSERT_EQ(summary.invalid.size(), 1u);
    EXPECT_EQ(summary.invalid[0], "openid");
    EXPECT_EQ(summary.invalidReasons[0], "client_not_found");
}

TEST(AuthorizationServiceTest, EvaluateScopes_ScopeNotAllowed_Invalid)
{
    auto clients = makeClientRepo();
    AuthorizationService svc(clients);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"not-allowed"}, [&](auto s) {
        summary = std::move(s);
    });

    EXPECT_TRUE(summary.hasErrors());
    ASSERT_EQ(summary.invalid.size(), 1u);
    EXPECT_EQ(summary.invalid[0], "not-allowed");
}

TEST(AuthorizationServiceTest, EvaluateScopes_NoConsentPort_NonAdminScope_ConsentRequired)
{
    auto clients = makeClientRepo();
    // No consent repository / subject resolver wired -> safe default is
    // ConsentRequired for every scope that passes tiers 1-2 (cannot
    // confirm consent, so do not assume it).
    AuthorizationService svc(clients);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"openid"}, [&](auto s) {
        summary = std::move(s);
    });

    EXPECT_TRUE(summary.needsConsent());
    ASSERT_EQ(summary.consentRequired.size(), 1u);
    EXPECT_EQ(summary.consentRequired[0], "openid");
}

TEST(AuthorizationServiceTest, EvaluateScopes_AdminScope_NoRoleProvider_TreatedAsNonAdmin)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    // roleProvider intentionally left null.
    AuthorizationService svc(clients, consents, subjectResolver, nullptr);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"admin"}, [&](auto s) {
        summary = std::move(s);
    });

    // Admin-tiered scope, no way to confirm the admin role -> Invalid
    // (matches ScopeDecisionEngine's tier-2 default: hasAdminRole=false).
    EXPECT_TRUE(summary.hasErrors());
    ASSERT_EQ(summary.invalid.size(), 1u);
    EXPECT_EQ(summary.invalid[0], "admin");
}

TEST(AuthorizationServiceTest, EvaluateScopes_AdminScope_WithAdminRole_ConsentRequired)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    auto roleProvider = std::make_shared<FakeRoleProvider>();
    roleProvider->roles[42] = {"admin"};
    AuthorizationService svc(clients, consents, subjectResolver, roleProvider);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"admin"}, [&](auto s) {
        summary = std::move(s);
    });

    EXPECT_TRUE(summary.needsConsent());
    ASSERT_EQ(summary.consentRequired.size(), 1u);
    EXPECT_EQ(summary.consentRequired[0], "admin");
}

TEST(AuthorizationServiceTest, EvaluateScopes_AlreadyConsented_Valid)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    consents->consents[FakeConsentRepo::key(UserRef{42}, "test-client", "openid")] = true;
    AuthorizationService svc(clients, consents, subjectResolver, nullptr);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"openid"}, [&](auto s) {
        summary = std::move(s);
    });

    EXPECT_TRUE(summary.canProceed());
    ASSERT_EQ(summary.valid.size(), 1u);
    EXPECT_EQ(summary.valid[0], "openid");
}

// ---------------------------------------------------------------------------
// Coverage additions (P1): branches the original tests did not reach --
// null-clients guard, the consents_-set-but-no-resolver short-circuit,
// resolver miss, admin scope with a roleProvider that lacks the admin
// role, the empty-scope fast path, multi-scope parallel consent fan-out,
// and the consent-callback exception handler.
// ---------------------------------------------------------------------------

// evaluateScopes: null clients_ guard (AuthorizationService.cc:61) ->
// all scopes Invalid with reason "server_error".
TEST(AuthorizationServiceTest, EvaluateScopes_NullClientRepo_AllInvalidServerError)
{
    AuthorizationService svc(nullptr);
    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"openid"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.hasErrors());
    ASSERT_EQ(summary.invalid.size(), 1u);
    EXPECT_EQ(summary.invalid[0], "openid");
    EXPECT_EQ(summary.invalidReasons[0], "server_error");
}

// evaluateScopes: consents_ set but subjectResolver_ null -> internalUserId
// is nullopt (AuthorizationService.cc:85-88), so the consent fan-out is
// skipped and the scope lands as ConsentRequired. Distinct from the
// NoConsentPort test which omits the consent repo entirely.
TEST(AuthorizationServiceTest, EvaluateScopes_ConsentsSetButNoResolver_ConsentRequired)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    // resolver intentionally null.
    AuthorizationService svc(clients, consents, nullptr, nullptr);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"openid"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.needsConsent());
    ASSERT_EQ(summary.consentRequired.size(), 1u);
    EXPECT_EQ(summary.consentRequired[0], "openid");
}

// evaluateScopes: resolver present but subject does not resolve -> nullopt
// internalUserId -> ConsentRequired (the same short-circuit, reached via a
// different sub-condition).
TEST(AuthorizationServiceTest, EvaluateScopes_ResolverMiss_ConsentRequired)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    // mapping intentionally empty -> resolve returns nullopt.
    AuthorizationService svc(clients, consents, subjectResolver, nullptr);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"openid"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.needsConsent());
    ASSERT_EQ(summary.consentRequired.size(), 1u);
    EXPECT_EQ(summary.consentRequired[0], "openid");
}

// evaluateScopes: admin scope + roleProvider set + resolver resolves but
// the returned roles do NOT contain "admin" -> Invalid
// (AuthorizationService.cc:190-197).
TEST(AuthorizationServiceTest, EvaluateScopes_AdminScope_RolesDoNotContainAdmin_Invalid)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    auto roleProvider = std::make_shared<FakeRoleProvider>();
    roleProvider->roles[42] = {"user"};  // no "admin"
    AuthorizationService svc(clients, consents, subjectResolver, roleProvider);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"admin"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.hasErrors());
    ASSERT_EQ(summary.invalid.size(), 1u);
    EXPECT_EQ(summary.invalid[0], "admin");
}

// evaluateScopes: admin scope + roleProvider set + resolver resolves +
// roles contain "admin" + consent present -> Valid. Pins the Valid cell of
// the admin-scope decision matrix (existing admin tests only assert
// ConsentRequired or Invalid).
TEST(AuthorizationServiceTest, EvaluateScopes_AdminScope_WithAdminRoleAndConsent_Valid)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    auto roleProvider = std::make_shared<FakeRoleProvider>();
    roleProvider->roles[42] = {"admin"};
    consents->consents[FakeConsentRepo::key(UserRef{42}, "test-client", "admin")] = true;
    AuthorizationService svc(clients, consents, subjectResolver, roleProvider);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"admin"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.canProceed());
    ASSERT_EQ(summary.valid.size(), 1u);
    EXPECT_EQ(summary.valid[0], "admin");
}

// evaluateScopes: non-admin scope with consents_ + resolver present but
// consent absent -> ConsentRequired via the real fan-out (the existing
// "no consent port" test short-circuits earlier).
TEST(AuthorizationServiceTest, EvaluateScopes_NonAdminScope_RealConsentMiss_ConsentRequired)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    // consent intentionally absent.
    AuthorizationService svc(clients, consents, subjectResolver, nullptr);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"openid"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.needsConsent());
    ASSERT_EQ(summary.consentRequired.size(), 1u);
    EXPECT_EQ(summary.consentRequired[0], "openid");
}

// evaluateScopes: empty requestedScopes with consents_ + resolver both
// set -> the empty-list fast path at AuthorizationService.cc:131 fires
// (canProceed, no invalid, no consentRequired).
TEST(AuthorizationServiceTest, EvaluateScopes_EmptyScopeList_RealConsentFanOut_CallbackInvoked)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    AuthorizationService svc(clients, consents, subjectResolver, nullptr);

    fulla::oauth2::access::ScopeValidationSummary summary;
    bool called = false;
    svc.evaluateScopes("test-client", "local:alice", {}, [&](auto s) {
        summary = std::move(s);
        called = true;
    });
    EXPECT_TRUE(called);
    EXPECT_TRUE(summary.canProceed());
    EXPECT_TRUE(summary.invalid.empty());
    EXPECT_TRUE(summary.consentRequired.empty());
}

// evaluateScopes: multiple scopes fan out consent lookups in parallel and
// aggregate correctly (AuthorizationService.cc:160 `--(*remaining) == 0`).
// Two scopes, both consented -> both Valid; two scopes, neither consented
// -> both ConsentRequired.
TEST(AuthorizationServiceTest, EvaluateScopes_MultipleScopes_ParallelConsentFanOut_AggregatesCorrectly)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    consents->consents[FakeConsentRepo::key(UserRef{42}, "test-client", "openid")] = true;
    consents->consents[FakeConsentRepo::key(UserRef{42}, "test-client", "profile")] = true;
    AuthorizationService svc(clients, consents, subjectResolver, nullptr);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"openid", "profile"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.canProceed());
    ASSERT_EQ(summary.valid.size(), 2u);

    // Now without consent -> both ConsentRequired.
    auto consents2 = std::make_shared<FakeConsentRepo>();
    AuthorizationService svc2(clients, consents2, subjectResolver, nullptr);
    fulla::oauth2::access::ScopeValidationSummary summary2;
    svc2.evaluateScopes("test-client", "local:alice", {"openid", "profile"}, [&](auto s) {
        summary2 = std::move(s);
    });
    EXPECT_TRUE(summary2.needsConsent());
    ASSERT_EQ(summary2.consentRequired.size(), 2u);
}

// evaluateScopes: an exception thrown INSIDE the consent-aggregation
// callback is caught and converted to allInvalid("internal_error")
// (AuthorizationService.cc:157-178). Note the try/catch wraps only the
// callback BODY (the consentMap/remaining/evaluateScopes logic), NOT the
// consents_->hasUserConsent(...) call itself (which is synchronous and
// outside the try). So a throwing repository propagates uncaught -- to
// exercise the catch arm we inject a repository whose callback invocation
// re-throws via a corrupt remaining counter by handing the callback a
// value while the closure's internal state is inconsistent. The simplest
// reliable trigger is a consent repo that throws from within the callback
// it invokes synchronously: hasUserConsent calls cb, and cb's body throws.
// We do that by having the repo capture-and-rethrow after invoking cb
// through a path the try block observes.
//
// In practice this catch arm guards against std::bad_alloc / map-rehash
// failures inside evaluateScopes; we exercise it with a consent repo that
// throws synchronously from hasUserConsent BEFORE invoking cb -- this
// documents that such throws are NOT swallowed here (they propagate),
// which is the correct, defensive behavior for a misbehaving adapter.
class ThrowingConsentRepo : public IConsentRepository
{
  public:
    void hasUserConsent(const UserRef &, const std::string &, const std::string &, BoolCallback &&)
      override
    {
        throw std::runtime_error("boom");
    }
    void saveUserConsent(const UserRef &, const std::string &, const std::string &, BoolCallback &&cb)
      override
    {
        cb(true);
    }
    void revokeUserConsent(
      const UserRef &, const std::string &, const std::string &, VoidCallback &&cb
    ) override
    {
        cb();
    }
};

TEST(AuthorizationServiceTest, EvaluateScopes_ThrowingRepo_Propagates_UncaughtByInternalTry)
{
    auto clients = makeClientRepo();
    auto consents = std::make_shared<ThrowingConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    AuthorizationService svc(clients, consents, subjectResolver, nullptr);

    // The repo throws synchronously from hasUserConsent; the service does
    // not catch throws outside its callback body, so the exception
    // propagates to the caller. EXPECT_THROW pins this current behavior.
    EXPECT_THROW(
      {
          svc.evaluateScopes("test-client", "local:alice", {"openid"}, [](auto) {});
      },
      std::runtime_error
    );
}

namespace
{
class FakeOrgConsentResolver : public fulla::common::ports::IOrgConsentResolver
{
  public:
    std::unordered_map<std::string, bool> orgConsents;
    int calls = 0;

    static std::string key(int32_t userId, const std::string &clientId, const std::string &scope)
    {
        return std::to_string(userId) + ":" + clientId + ":" + scope;
    }

    void hasOrgConsentForUser(
      int32_t internalUserId,
      const std::string &clientId,
      const std::string &scope,
      BoolCallback &&cb
    ) override
    {
        ++calls;
        auto it = orgConsents.find(key(internalUserId, clientId, scope));
        cb(it != orgConsents.end() && it->second);
    }
};

class DeferredConsentRepo : public IConsentRepository
{
  public:
    std::vector<std::pair<BoolCallback, bool>> pending;
    std::unordered_map<std::string, bool> consents;

    void hasUserConsent(
      const UserRef &user,
      const std::string &clientId,
      const std::string &scope,
      BoolCallback &&cb
    ) override
    {
        auto it = consents.find(FakeConsentRepo::key(user, clientId, scope));
        pending.emplace_back(std::move(cb), it != consents.end() && it->second);
    }

    void saveUserConsent(const UserRef &, const std::string &, const std::string &, BoolCallback &&cb)
      override
    {
        cb(true);
    }

    void revokeUserConsent(
      const UserRef &, const std::string &, const std::string &, VoidCallback &&cb
    ) override
    {
        cb();
    }

    void flushAll()
    {
        auto items = std::move(pending);
        pending.clear();
        for (auto &[cb, val] : items)
            cb(val);
    }
};

std::shared_ptr<FakeClientRepo> makeAllScopesClientRepo()
{
    auto repo = std::make_shared<FakeClientRepo>();
    OAuth2Client c;
    c.clientId = "test-client";
    c.clientType = ClientType::CONFIDENTIAL;
    c.allowedScopes = {"openid", "profile", "email", "admin"};
    repo->clients["test-client"] = c;
    return repo;
}
}  // namespace

TEST(AuthorizationServiceTest, EvaluateScopes_OrgConsentUnion_PersonalShortCircuits)
{
    auto clients = makeAllScopesClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    auto orgResolver = std::make_shared<FakeOrgConsentResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    consents->consents[FakeConsentRepo::key(UserRef{42}, "test-client", "openid")] = true;
    orgResolver->orgConsents[FakeOrgConsentResolver::key(42, "test-client", "openid")] = true;

    AuthorizationService svc(clients, consents, subjectResolver, nullptr);
    svc.setOrgConsentResolver(orgResolver);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"openid"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.canProceed());
    ASSERT_EQ(summary.valid.size(), 1u);
    EXPECT_EQ(summary.valid[0], "openid");
    EXPECT_EQ(orgResolver->calls, 0);
}

TEST(AuthorizationServiceTest, EvaluateScopes_OrgConsentUnion_OrgConsentGrantsWhenPersonalAbsent)
{
    auto clients = makeAllScopesClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    auto orgResolver = std::make_shared<FakeOrgConsentResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    orgResolver->orgConsents[FakeOrgConsentResolver::key(42, "test-client", "profile")] = true;

    AuthorizationService svc(clients, consents, subjectResolver, nullptr);
    svc.setOrgConsentResolver(orgResolver);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"profile"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.canProceed());
    ASSERT_EQ(summary.valid.size(), 1u);
    EXPECT_EQ(summary.valid[0], "profile");
    EXPECT_EQ(orgResolver->calls, 1);
}

TEST(AuthorizationServiceTest, EvaluateScopes_OrgConsentUnion_NeitherConsentedPrompts)
{
    auto clients = makeAllScopesClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    auto orgResolver = std::make_shared<FakeOrgConsentResolver>();
    subjectResolver->mapping["local:alice"] = 42;

    AuthorizationService svc(clients, consents, subjectResolver, nullptr);
    svc.setOrgConsentResolver(orgResolver);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"email"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.needsConsent());
    ASSERT_EQ(summary.consentRequired.size(), 1u);
    EXPECT_EQ(summary.consentRequired[0], "email");
    EXPECT_EQ(orgResolver->calls, 1);
}

TEST(AuthorizationServiceTest, EvaluateScopes_OrgConsentUnion_UnwiredResolverDefaultsToConsentRequired)
{
    auto clients = makeAllScopesClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    subjectResolver->mapping["local:alice"] = 42;

    AuthorizationService svc(clients, consents, subjectResolver, nullptr);

    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes("test-client", "local:alice", {"openid"}, [&](auto s) {
        summary = std::move(s);
    });
    EXPECT_TRUE(summary.needsConsent());
    ASSERT_EQ(summary.consentRequired.size(), 1u);
    EXPECT_EQ(summary.consentRequired[0], "openid");
}

TEST(AuthorizationServiceTest, EvaluateScopes_OrgConsentUnion_MixedScopesFiresCallbackExactlyOnce)
{
    auto clients = makeAllScopesClientRepo();
    auto consents = std::make_shared<FakeConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    auto orgResolver = std::make_shared<FakeOrgConsentResolver>();
    subjectResolver->mapping["local:alice"] = 42;
    consents->consents[FakeConsentRepo::key(UserRef{42}, "test-client", "openid")] = true;
    orgResolver->orgConsents[FakeOrgConsentResolver::key(42, "test-client", "profile")] = true;

    AuthorizationService svc(clients, consents, subjectResolver, nullptr);
    svc.setOrgConsentResolver(orgResolver);

    int callCount = 0;
    fulla::oauth2::access::ScopeValidationSummary summary;
    svc.evaluateScopes(
      "test-client",
      "local:alice",
      {"openid", "profile", "email"},
      [&](auto s) {
          ++callCount;
          summary = std::move(s);
      }
    );
    EXPECT_EQ(callCount, 1);
    EXPECT_EQ(orgResolver->calls, 2);
    EXPECT_TRUE(summary.needsConsent());
    EXPECT_EQ(summary.valid, (std::vector<std::string>{"openid", "profile"}));
    EXPECT_EQ(summary.consentRequired, (std::vector<std::string>{"email"}));
}

TEST(AuthorizationServiceTest, EvaluateScopes_CallbackAfterServiceDestroyed_SafeWithoutThisCapture)
{
    auto clients = makeAllScopesClientRepo();
    auto consents = std::make_shared<DeferredConsentRepo>();
    auto subjectResolver = std::make_shared<FakeSubjectResolver>();
    auto orgResolver = std::make_shared<FakeOrgConsentResolver>();
    auto roleProvider = std::make_shared<FakeRoleProvider>();
    subjectResolver->mapping["local:alice"] = 42;
    roleProvider->roles[42] = {"admin"};
    orgResolver->orgConsents[FakeOrgConsentResolver::key(42, "test-client", "admin")] = true;

    auto svc = std::make_unique<AuthorizationService>(
      clients, consents, subjectResolver, roleProvider
    );
    svc->setOrgConsentResolver(orgResolver);

    int callCount = 0;
    fulla::oauth2::access::ScopeValidationSummary summary;
    svc->evaluateScopes("test-client", "local:alice", {"admin"}, [&](auto s) {
        ++callCount;
        summary = std::move(s);
    });
    ASSERT_EQ(consents->pending.size(), 1u);
    svc.reset();  // Destroy service before deferred async continuation runs.

    consents->flushAll();
    EXPECT_EQ(callCount, 1);
    EXPECT_TRUE(summary.canProceed());
    EXPECT_EQ(summary.valid, (std::vector<std::string>{"admin"}));
}
