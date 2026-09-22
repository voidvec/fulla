// tests/integration/concurrency/Property4_OpenApiValidationBaselineTest.cc
//
// Spec: concurrency-lifetime-safety-audit — Task 4 (Property 4: Preservation).
// Behavioral Equivalence on ¬C(X). Observation object **3.1**: under the normal
// (sequential, correctly-initialized) path, the OpenApi document content and
// the `getValidationRules(path)` rule set (endpoints / params / rules) must be
// preserved byte-for-byte across the category-A fix (tasks 6.1/6.2, re-verified
// at 6.4).
//
// ─────────────────────────────────────────────────────────────────────────
// RELATIONSHIP TO TASK 1 (reference + extend, do NOT duplicate)
// ─────────────────────────────────────────────────────────────────────────
// Task 1 (unit/initorder/CategoryA_InitOrderSnapshotTest.cc) already froze the
// DEEP per-endpoint snapshots (token endpoint params/enums/response examples,
// discovery+JWKS summaries, security wiring) and the per-path validation
// accept/reject behavior. Those are the SIOF bug-condition baselines.
//
// This Task-4 file does NOT re-assert those deep field snapshots. It adds the
// PRESERVATION-specific artifacts that Task 1 did not capture and that the
// F-vs-F' diff at 6.4 needs:
//   (1) DETERMINISM: generateOpenApiSpec() is byte-identical across repeated
//       generations (the SIOF fix must not introduce nondeterministic / partial
//       registration).
//   (2) A CANONICAL whole-spec FINGERPRINT: the complete sorted (path, method)
//       set serialized to a single string. This is the byte-comparable baseline
//       recorded here on F; task 6.4 regenerates it on F' and asserts equality.
//   (3) A PBT-randomized validation-outcome baseline: for randomly generated
//       normal requests (random configured paths × valid/invalid variants), the
//       accept/reject decision of RequestValidationFilter is stable.
//
// METHODOLOGY: observation-first. Runs on UNFIXED code (F) and MUST PASS — it
// records the must-be-preserved baseline. No production code is modified.
//
// **Validates: Requirements 3.1**
//
// _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7_

#include <drogon/drogon_test.h>
#include <drogon/HttpRequest.h>
#include <json/json.h>

#include <set>
#include <string>
#include <vector>

#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/filters/RequestValidationFilter.h>

#include "Property4_PreservationSupport.h"

using namespace fulla::drogon::observability::openapi;
using namespace fulla::test::concurrency;

namespace
{
// Canonical, order-independent fingerprint of the whole OpenApi spec: every
// "METHOD path" pair, sorted, joined by '\n'. Two structurally identical specs
// produce an identical fingerprint string regardless of member insertion order.
// This is the byte-comparable preservation artifact recorded on F and diffed on
// F' at task 6.4.
std::string openApiPathMethodFingerprint(const Json::Value &spec)
{
    std::set<std::string> lines;
    if (spec.isMember("paths"))
    {
        const Json::Value &paths = spec["paths"];
        for (const auto &path : paths.getMemberNames())
        {
            for (const auto &method : paths[path].getMemberNames())
            {
                std::string m = method;
                for (auto &c : m)
                    c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
                lines.insert(m + " " + path);
            }
        }
    }
    std::string out;
    for (const auto &l : lines)
    {
        out += l;
        out += "\n";
    }
    return out;
}

// The complete, frozen (path, method) contract. Recorded here as the Task-4
// preservation baseline for the WHOLE spec (Task 1 froze the deep fields of a
// representative subset; this freezes the full set as one comparable unit).
//
// NOTE: tools/openapi-governance/check_spec_governance.py (CI static-checks)
// parses the string literals below as the authoritative "docs-registered
// endpoint set" and diffs them against the ADD_METHOD_TO routes and
// apps/server/openapi.yaml. If you change this baseline, re-run that gate.
const std::string &expectedFingerprint()
{
    // Sorted "METHOD path" lines, matching openApiPathMethodFingerprint().
    // Recorded as the observation-first baseline of F's COMPLETE registered
    // (path, method) contract (the whole test binary links every controller, so
    // every endpoint's docs are registered). Task 6.4 regenerates this on F'
    // and asserts byte-for-byte equality.
    //
    // 2026-08-16 reconciliation (openapi-spec-governance M0): removed ghost
    // registrations for routes that never existed (/oauth2/device/verify GET+POST,
    // /oauth2/mfa/{setup,setup/verify,disable}); added missing registrations for
    // real routes (/api/me/mfa/{setup,verify,disable}, /oauth2/device/approve,
    // /oauth2/logout, /oauth2/end_session GET+POST, /health/{live,ready},
    // /.well-known/oauth-authorization-server).
    static const std::string kFingerprint =
      "DELETE /api/admin/clients/{clientId}\n"
      "DELETE /api/admin/roles/{roleId}\n"
      "DELETE /api/admin/scopes/{scopeId}\n"
      "DELETE /api/admin/tokens/{tokenPrefix}\n"
      "DELETE /api/admin/users/{userId}\n"
      "DELETE /api/me\n"
      "DELETE /api/me/applications/{clientId}\n"
      "DELETE /api/me/authorized-apps/{clientId}\n"
      "DELETE /api/me/organizations/{slug}/invitations/{invitationId}\n"
      "DELETE /api/me/organizations/{slug}/members/{userId}\n"
      "DELETE /api/me/social/links/{provider}\n"
      "GET /.well-known/jwks.json\n"
      "GET /.well-known/oauth-authorization-server\n"
      "GET /.well-known/openid-configuration\n"
      "GET /api/admin/clients\n"
      "GET /api/admin/clients/{clientId}\n"
      "GET /api/admin/clients/{clientId}/scopes\n"
      "GET /api/admin/dashboard\n"
      "GET /api/admin/dashboard/stats\n"
      "GET /api/admin/logs\n"
      "GET /api/admin/oidc/keys\n"
      "GET /api/admin/organizations\n"
      "GET /api/admin/organizations/{slug}\n"
      "GET /api/admin/roles\n"
      "GET /api/admin/scopes\n"
      "GET /api/admin/scopes/resources\n"
      "GET /api/admin/tokens\n"
      "GET /api/admin/users\n"
      "GET /api/admin/users/{userId}\n"
      "GET /api/admin/users/{userId}/roles\n"
      "GET /api/me\n"
      "GET /api/me/applications\n"
      "GET /api/me/authorized-apps\n"
      "GET /api/me/organizations\n"
      "GET /api/me/organizations/{slug}/invitations\n"
      "GET /api/me/organizations/{slug}/members\n"
      "GET /api/me/social/links\n"
      "GET /api/me/webauthn/credentials\n"
      "GET /api/verify-email\n"
      "GET /docs/api/\n"
      "GET /docs/api/openapi.json\n"
      "GET /health\n"
      "GET /health/live\n"
      "GET /health/ready\n"
      "GET /oauth2/authorize\n"
      "GET /oauth2/consent/context\n"
      "GET /oauth2/end_session\n"
      "GET /oauth2/userinfo\n"
      "PATCH /api/me/applications/{clientId}\n"
      "PATCH /api/me/profile\n"
      "POST /api/admin/clients\n"
      "POST /api/admin/clients/{clientId}/reset-secret\n"
      "POST /api/admin/clients/{clientId}/resume\n"
      "POST /api/admin/clients/{clientId}/suspend\n"
      "POST /api/admin/organizations\n"
      "POST /api/admin/organizations/{slug}/transfer-ownership\n"
      "POST /api/admin/roles\n"
      "POST /api/admin/scopes\n"
      "POST /api/admin/tokens/revoke-by-client\n"
      "POST /api/admin/tokens/revoke-by-user\n"
      "POST /api/admin/users\n"
      "POST /api/admin/users/{userId}/enable\n"
      "POST /api/github/login\n"
      "POST /api/google/login\n"
      "POST /api/me/applications\n"
      "POST /api/me/applications/{clientId}/rotate-secret\n"
      "POST /api/me/applications/{clientId}/transfer\n"
      "POST /api/me/mfa/disable\n"
      "POST /api/me/mfa/setup\n"
      "POST /api/me/mfa/verify\n"
      "POST /api/me/org-invitations/accept\n"
      "POST /api/me/organizations\n"
      "POST /api/me/organizations/{slug}/invitations\n"
      "POST /api/me/social/links/{provider}\n"
      "POST /api/me/social/links/{provider}/authorize\n"
      "POST /api/me/webauthn/register/begin\n"
      "POST /api/me/webauthn/register/finish\n"
      "POST /api/password-reset/confirm\n"
      "POST /api/password-reset/request\n"
      "POST /api/register\n"
      "POST /api/verify-email/resend\n"
      "POST /api/verify-email/resend-by-email\n"
      "POST /api/wechat/login\n"
      "POST /oauth2/consent\n"
      "POST /oauth2/device/approve\n"
      "POST /oauth2/device_authorization\n"
      "POST /oauth2/end_session\n"
      "POST /oauth2/introspect\n"
      "POST /oauth2/login\n"
      "POST /oauth2/logout\n"
      "POST /oauth2/mfa/verify\n"
      "POST /oauth2/password/change\n"
      "POST /oauth2/register\n"
      "POST /oauth2/revoke\n"
      "POST /oauth2/token\n"
      "POST /oauth2/webauthn/authenticate/begin\n"
      "POST /oauth2/webauthn/authenticate/finish\n"
      "PUT /api/admin/clients/{clientId}\n"
      "PUT /api/admin/clients/{clientId}/scopes\n"
      "PUT /api/admin/roles/{roleId}\n"
      "PUT /api/admin/scopes/{scopeId}\n"
      "PUT /api/admin/users/{userId}\n"
      "PUT /api/admin/users/{userId}/disable\n"
      "PUT /api/admin/users/{userId}/roles\n"
      "PUT /api/me/password\n";
    return kFingerprint;
}

enum class FilterOutcome
{
    Passed,
    Rejected,
    Neither
};

FilterOutcome runValidation(const ::drogon::HttpRequestPtr &req)
{
    RequestValidationFilter filter;
    FilterOutcome outcome = FilterOutcome::Neither;
    auto fcb = [&outcome](const ::drogon::HttpResponsePtr &) { outcome = FilterOutcome::Rejected; };
    auto fccb = [&outcome]() { outcome = FilterOutcome::Passed; };
    filter.doFilter(req, std::move(fcb), std::move(fccb));
    return outcome;
}
}  // namespace

// 3.1 PRESERVATION (determinism): the spec generation is byte-identical across
// repeated calls. The SIOF fix (6.1) must not turn this into partial / order-
// dependent output.
DROGON_TEST(Integration_P1_OpenApiSpec_Property4_3_1_Deterministic_Baseline)
{
    Json::Value spec1 = OpenApiGenerator::generateOpenApiSpec();
    Json::Value spec2 = OpenApiGenerator::generateOpenApiSpec();

    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    const std::string s1 = Json::writeString(b, spec1);
    const std::string s2 = Json::writeString(b, spec2);

    CHECK(s1 == s2);  // byte-for-byte stable generation
    CHECK(!s1.empty());
}

// 3.1 PRESERVATION (whole-spec fingerprint): the complete (path, method) set
// equals the frozen baseline. This is the byte-comparable artifact diffed F vs
// F' at 6.4.
DROGON_TEST(Integration_P1_OpenApiSpec_Property4_3_1_PathMethodFingerprint_Baseline)
{
    Json::Value spec = OpenApiGenerator::generateOpenApiSpec();
    const std::string fp = openApiPathMethodFingerprint(spec);
    CHECK(fp == expectedFingerprint());
}

// 3.1 PRESERVATION (validation rules, PBT): randomized normal requests against
// configured paths yield stable accept/reject decisions. A valid request on a
// configured path must PASS; a request missing a required field must be
// REJECTED. The SIOF fix (6.2, map -> Meyers singleton) must not change which
// path has which rules.
DROGON_TEST(Integration_P1_ValidationRules_Property4_3_1_RandomizedOutcomes_Baseline)
{
    PreservationInputGen gen(0x04C0FFEEu);

    // (path, build-valid, build-invalid) cases for the configured rule paths.
    struct Case
    {
        const char *path;
        ::drogon::HttpMethod method;
    };

    const std::vector<Case> cases = {
      {"/oauth2/authorize", ::drogon::Get},
      {"/oauth2/token", ::drogon::Post},
      {"/oauth2/login", ::drogon::Post},
      {"/api/register", ::drogon::Post},
    };

    constexpr int kRounds = 24;
    for (int round = 0; round < kRounds; ++round)
    {
        const Case &c = cases[gen.pick(cases.size())];

        // Build a VALID request for the chosen path.
        auto ok = ::drogon::HttpRequest::newHttpRequest();
        ok->setMethod(c.method);
        ok->setPath(c.path);
        std::string missingField;
        if (std::string(c.path) == "/oauth2/authorize")
        {
            ok->setParameter("client_id", gen.clientId());
            ok->setParameter("redirect_uri", "http://localhost:5173/callback");
            ok->setParameter("response_type", "code");
            missingField = "client_id";
        }
        else if (std::string(c.path) == "/oauth2/token")
        {
            ok->setParameter("grant_type", gen.grantType());
            missingField = "grant_type";
        }
        else if (std::string(c.path) == "/oauth2/login")
        {
            ok->setParameter("username", "user" + std::to_string(gen.intInRange(1, 999)));
            ok->setParameter("password", "password123");
            missingField = "password";  // remove -> too short / missing
        }
        else  // /api/register
        {
            ok->setParameter("username", "user" + std::to_string(gen.intInRange(1, 999)));
            ok->setParameter("password", "password123");
            missingField = "password";
        }

        CHECK(runValidation(ok) == FilterOutcome::Passed);

        // Build an INVALID variant (omit a required field) -> must be REJECTED.
        auto bad = ::drogon::HttpRequest::newHttpRequest();
        bad->setMethod(c.method);
        bad->setPath(c.path);
        if (std::string(c.path) == "/oauth2/authorize")
        {
            bad->setParameter("redirect_uri", "http://localhost:5173/callback");
            bad->setParameter("response_type", "code");
        }
        else if (std::string(c.path) == "/oauth2/token")
        {
            // no grant_type
        }
        else if (std::string(c.path) == "/oauth2/login")
        {
            bad->setParameter("username", "user");
            bad->setParameter("password", "short");  // < 8 chars
        }
        else  // /api/register
        {
            bad->setParameter("username", "user");
            // no password
        }
        CHECK(runValidation(bad) == FilterOutcome::Rejected);
        (void)missingField;
    }
}

// 3.1 PRESERVATION (negative control): an unconfigured path has no rules and
// must pass through unconditionally — pinned so the fix does not accidentally
// attach rules to unconfigured paths.
DROGON_TEST(Integration_P1_ValidationRules_Property4_3_1_UnconfiguredPath_PassThrough_Baseline)
{
    PreservationInputGen gen(0x1234ABCDu);
    static const std::vector<std::string> unconfigured =
      {"/health", "/metrics", "/random/unmatched/path", "/static/app.js"};
    for (int i = 0; i < 8; ++i)
    {
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Get);
        req->setPath(unconfigured[gen.pick(unconfigured.size())]);
        CHECK(runValidation(req) == FilterOutcome::Passed);
    }
}
