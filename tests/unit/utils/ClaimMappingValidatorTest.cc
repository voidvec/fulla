// tests/unit/utils/ClaimMappingValidatorTest.cc
//
// P1-3 (#280): write-time red lines for claims mapping + the custom
// attributes document policy. These are the security-closure unit tests
// the adversarial review demanded (H1: the draft's account-takeover chain
// is closed by the reserved-word blacklist + source whitelist + the
// openid-scope ban).

#include <drogon/drogon_test.h>
#include <fulla/drogon/utils/ClaimMappingValidator.h>

#include <string>

using fulla::drogon::utils::resolveClaimSourceValue;
using fulla::drogon::utils::validateCustomAttributes;
using fulla::drogon::utils::validateClaimMapping;

// ---------------------------------------------------------------------------
// Reserved-claim blacklist: no mapping may shadow a standard/published claim.
// ---------------------------------------------------------------------------
DROGON_TEST(Unit_P1_ClaimMapping_ReservedClaimBlacklist)
{
    for (const char *reserved :
         {"sub", "iss", "aud", "exp", "iat", "nonce", "auth_time", "acr", "amr",
          "org_ctx", "email", "email_verified", "name", "preferred_username",
          "family_name", "given_name", "nickname", "picture", "website",
          "phone_number", "phone_number_verified", "username", "roles"})
    {
        CHECK(!validateClaimMapping("profile", reserved, "custom_attributes.tenant", "both").empty());
    }
    // A non-reserved name passes (with a valid source).
    CHECK(validateClaimMapping("profile", "tenant_id", "custom_attributes.tenant", "both").empty());
}

// openid trigger scope is banned (always granted = zero-authorization).
DROGON_TEST(Unit_P1_ClaimMapping_OpenIdScopeBanned)
{
    CHECK(!validateClaimMapping("openid", "tenant_id", "custom_attributes.tenant", "both").empty());
}

// Source whitelist: custom_attributes.<key> + user.display_name ONLY.
DROGON_TEST(Unit_P1_ClaimMapping_SourceWhitelist)
{
    CHECK(validateClaimMapping("profile", "tenant_id", "custom_attributes.tenant", "both").empty());
    CHECK(validateClaimMapping("profile", "display", "user.display_name", "both").empty());
    // org.role is CUT (cross-context leak — the review's H1 companion).
    CHECK(!validateClaimMapping("profile", "org_role", "org.role", "both").empty());
    // Path traversal / other built-ins / injections rejected.
    CHECK(!validateClaimMapping("profile", "x", "users.email", "both").empty());
    CHECK(!validateClaimMapping("profile", "x", "custom_attributes..", "both").empty());
    CHECK(!validateClaimMapping("profile", "x", "custom_attributes.", "both").empty());
    CHECK(!validateClaimMapping("profile", "x", "custom_attributes.Bad_Key", "both").empty());
}

DROGON_TEST(Unit_P1_ClaimMapping_ShapeRules)
{
    CHECK(!validateClaimMapping("profile", "x", "custom_attributes.k", "sometimes").empty());
    CHECK(!validateClaimMapping("profile", "X", "custom_attributes.k", "both").empty());
    CHECK(!validateClaimMapping("", "x", "custom_attributes.k", "both").empty());
}

// ---------------------------------------------------------------------------
// Custom attributes document policy.
// ---------------------------------------------------------------------------
DROGON_TEST(Unit_P1_CustomAttributes_Policy)
{
    CHECK(validateCustomAttributes("{\"tenant\":\"acme\"}").empty());
    CHECK(validateCustomAttributes("{\"prefs\":{\"theme\":\"dark\"}}").empty());
    // Depth > 2 rejected.
    CHECK(!validateCustomAttributes("{\"a\":{\"b\":{\"c\":1}}}").empty());
    // Prototype-pollution trio rejected (regex excludes; explicit check guards).
    CHECK(!validateCustomAttributes("{\"__proto__\":{\"x\":1}}").empty());
    CHECK(!validateCustomAttributes("{\"constructor\":\"1\"}").empty());
    CHECK(!validateCustomAttributes("{\"a\":{\"prototype\":1}}").empty());
    // Key grammar: uppercase / leading underscore / too long rejected.
    CHECK(!validateCustomAttributes("{\"BadKey\":1}").empty());
    CHECK(!validateCustomAttributes("{\"_private\":1}").empty());
    CHECK(!validateCustomAttributes("{\"a2345678901234567890123456789012345678901234567890123456789012345678\":1}").empty());
    // Non-object rejected.
    CHECK(!validateCustomAttributes("[1,2]").empty());
    CHECK(!validateCustomAttributes("\"x\"").empty());
    // Size cap 8 KiB.
    std::string big = "{\"k\":\"";
    big += std::string(9000, 'a');
    big += "\"}";
    CHECK(!validateCustomAttributes(big).empty());
}

// ---------------------------------------------------------------------------
// Scalar source resolution.
// ---------------------------------------------------------------------------
DROGON_TEST(Unit_P1_ClaimSource_ScalarResolution)
{
    const std::string profile =
      "{\"display_name\":\"Ada\",\"custom_attributes\":{\"tenant\":\"acme\",\"n\":7,\"b\":true,"
      "\"obj\":{\"x\":1},\"arr\":[1]}}";
    std::string out;
    CHECK(resolveClaimSourceValue("user.display_name", profile, out));
    CHECK(out == "Ada");
    CHECK(resolveClaimSourceValue("custom_attributes.tenant", profile, out));
    CHECK(out == "acme");
    CHECK(resolveClaimSourceValue("custom_attributes.n", profile, out));
    CHECK(out == "7");
    CHECK(resolveClaimSourceValue("custom_attributes.b", profile, out));
    CHECK(out == "true");
    // Objects/arrays never publish.
    CHECK(!resolveClaimSourceValue("custom_attributes.obj", profile, out));
    CHECK(!resolveClaimSourceValue("custom_attributes.arr", profile, out));
    // Absent keys / non-whitelisted roots.
    CHECK(!resolveClaimSourceValue("custom_attributes.missing", profile, out));
    CHECK(!resolveClaimSourceValue("users.email", profile, out));
}
