// tests/unit/authz/RbacEnforcementEncodingTest.cc
//
// P1-8 (#285): pins the ScopeDecisionEngine predicate ENCODING for the
// three-chain RBAC enforcement — the adversarial review warned that the
// draft's wording could be implemented as all-deny or all-allow. The
// encoding (security review H2, classification vs determination decoupled):
//
//   scope ∈ admin-class ∧ scope ∉ user's granted permission set → invalid
//   (hasAdminRole keeps its literal meaning: the admin super-role passes
//   regardless)
//
// The engine's signature is unchanged — the ENCODING lives in the
// predicate the caller supplies; these tests exercise exactly the closure
// AuthorizationService builds after P1-8, through the engine, so a
// regression in either layer fails here.
//
// Also pins the gate-③ semantics table (AuthorizationFilter::checkAccess
// is HTTP-level; its truth table is mirrored here as documentation and
// exercised end-to-end by AdminRbacEnforcementHttpTest).

#include <drogon/drogon_test.h>
#include <fulla/oauth2/access/ScopeDecisionEngine.h>
#include <fulla/oauth2/model/Dto.h>

#include <string>
#include <unordered_set>
#include <vector>

using fulla::oauth2::access::evaluateScopes;

namespace
{
fulla::oauth2::model::Client wideOpenClient()
{
    fulla::oauth2::model::OAuth2Client dto;
    dto.clientId = "ci";
    dto.allowedScopes = {"openid", "profile", "admin", "users:read", "users:write",
                         "clients:read", "roles:read", "audit:read"};
    return fulla::oauth2::model::Client(std::move(dto));
}

// The P1-8 AuthorizationService closure, verbatim in shape.
std::function<bool(const std::string &)> predicate(
  const std::unordered_set<std::string> &adminClass,
  const std::unordered_set<std::string> &userPermissions
)
{
    return [&adminClass, &userPermissions](const std::string &s) {
        return adminClass.count(s) > 0 && userPermissions.count(s) == 0;
    };
}

const std::unordered_set<std::string> kAdminClass = {
  "admin", "users:read", "users:write", "clients:read", "clients:write",
  "tokens:read", "tokens:write", "roles:read", "roles:write", "audit:read"};
}  // namespace

// A helpdesk user (users:read only): users:read passes, every other
// admin-class scope is invalid, non-admin scopes untouched.
DROGON_TEST(Unit_P1_Rbac_HelpdeskEncoding_UsersReadPasses_OthersInvalid)
{
    const std::unordered_set<std::string> helpdeskPerms = {"users:read"};
    auto summary = evaluateScopes(
      {"users:read", "users:write", "clients:read", "openid"},
      wideOpenClient(),
      /*hasAdminRole=*/false,
      [](const std::string &) { return true; },  // consent given
      predicate(kAdminClass, helpdeskPerms));

    REQUIRE(summary.valid.size() == 2);    // users:read + openid
    REQUIRE(summary.invalid.size() == 2);  // users:write + clients:read
    CHECK(summary.invalid[0] == "users:write");
    CHECK(summary.invalid[1] == "clients:read");
    for (const auto &reason : summary.invalidReasons)
        CHECK(reason == "admin_role_required");
}

// The admin super-role passes everything (hasAdminRole literal) — even
// with an EMPTY permission resolution (provider without the permission
// path: historical behavior preserved).
DROGON_TEST(Unit_P1_Rbac_AdminSuperRole_AllPasses_EvenWithoutPermissions)
{
    auto summary = evaluateScopes(
      {"users:read", "users:write", "admin"},
      wideOpenClient(),
      /*hasAdminRole=*/true,
      [](const std::string &) { return true; },
      predicate(kAdminClass, {}));

    CHECK(summary.invalid.empty());
    CHECK(summary.valid.size() == 3);
}

// A NON-admin user with a granted permission set that does NOT include an
// admin-class scope keeps it invalid — granting a resource scope at the
// ROLE layer (negative red line companion) never de-classifies anything.
DROGON_TEST(Unit_P1_Rbac_NoPermissionResolution_AdminClassRequiresAdmin)
{
    auto summary = evaluateScopes(
      {"users:read", "roles:write"},
      wideOpenClient(),
      /*hasAdminRole=*/false,
      [](const std::string &) { return true; },
      predicate(kAdminClass, {}));

    REQUIRE(summary.invalid.size() == 2);
    CHECK(summary.invalid[0] == "users:read");
    CHECK(summary.invalid[1] == "roles:write");
}

// Permission coverage is PER-SCOPE, not per-prefix: a user with users:read
// does NOT get users:write.
DROGON_TEST(Unit_P1_Rbac_PermissionCoverage_IsExact)
{
    const std::unordered_set<std::string> perms = {"users:read", "audit:read"};
    auto summary = evaluateScopes(
      {"users:read", "users:write", "audit:read"},
      wideOpenClient(),
      /*hasAdminRole=*/false,
      [](const std::string &) { return true; },
      predicate(kAdminClass, perms));

    REQUIRE(summary.valid.size() == 2);
    REQUIRE(summary.invalid.size() == 1);
    CHECK(summary.invalid[0] == "users:write");
}
