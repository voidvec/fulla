// tests/integration/oidc/DiscoveryReconciliationHttpTest.cc
//
// P0-5 (#259): discovery metadata is reconciled against the implementation.
// The 2026-10 gap analysis found both directions of drift (scopes_supported
// omitted org + the resource-scope family; code_challenge_methods advertised
// plain; claims_supported listed claims that were never emitted). These
// checks freeze the reconciled contract on BOTH metadata endpoints
// (openid-configuration and RFC 8414 oauth-authorization-server) so any
// future drift fails CI instead of shipping to RPs.
//
// Same guard as the other HTTP batches: requires Postgres (seed catalog) +
// the live listener on 127.0.0.1:5555; skips cleanly otherwise.

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define DISCOVERY_RECON_SKIP_GUARD                             \
    do                                                         \
    {                                                          \
        if (!postgresAvailable() || !serverReachable())        \
        {                                                      \
            CHECK(true);                                       \
            return;                                            \
        }                                                      \
    } while (0)

namespace
{

std::set<std::string> stringSet(const Json::Value &arr)
{
    std::set<std::string> out;
    if (arr.isArray())
    {
        for (const auto &v : arr)
            if (v.isString())
                out.insert(v.asString());
    }
    return out;
}

Json::Value fetchMetadata(const std::string &path)
{
    auto resp = sendGet(path, "");
    if (!resp || resp->getStatusCode() != ::drogon::k200OK)
        return {};
    Json::Value body;
    parseJsonBody(resp, body);
    return body;
}

}  // namespace

// Both endpoints: grant_types == the token endpoint's dispatch set;
// token_endpoint_auth_methods == the implemented client-auth paths;
// code_challenge_methods == S256 only (P0-5: plain rejected at authorize).
DROGON_TEST(Integration_P0_Discovery_GrantsMethods_AcrossBothEndpoints)
{
    DISCOVERY_RECON_SKIP_GUARD;

    const std::set<std::string> kGrants = {
      "authorization_code",
      "refresh_token",
      "client_credentials",
      "urn:ietf:params:oauth:grant-type:device_code",
    };
    const std::set<std::string> kAuthMethods = {"client_secret_basic", "client_secret_post"};
    const std::set<std::string> kPkce = {"S256"};

    for (const std::string &path :
         {"/.well-known/openid-configuration", "/.well-known/oauth-authorization-server"})
    {
        Json::Value doc = fetchMetadata(path);
        REQUIRE(!doc.isNull());
        CHECK(stringSet(doc["grant_types_supported"]) == kGrants);
        CHECK(stringSet(doc["token_endpoint_auth_methods_supported"]) == kAuthMethods);
        CHECK(stringSet(doc["code_challenge_methods_supported"]) == kPkce);
    }
}

// scopes_supported comes from the DB catalog (P0-5): it must cover the
// seeded core + org + the V023 resource-scope family, and must NOT carry
// the inert legacy read/write rows.
DROGON_TEST(Integration_P0_Discovery_Scopes_CatalogBacked)
{
    DISCOVERY_RECON_SKIP_GUARD;

    Json::Value doc = fetchMetadata("/.well-known/openid-configuration");
    REQUIRE(!doc.isNull());
    auto scopes = stringSet(doc["scopes_supported"]);

    for (const std::string &must :
         {"openid", "profile", "email", "admin", "org", "users:read", "clients:read", "audit:read"})
    {
        CHECK(scopes.count(must) == 1);
    }
    CHECK(scopes.count("read") == 0);
    CHECK(scopes.count("write") == 0);

    auto asMeta = fetchMetadata("/.well-known/oauth-authorization-server");
    REQUIRE(!asMeta.isNull());
    CHECK(stringSet(asMeta["scopes_supported"]) == scopes);
}

// claims_supported == the set actually emitted across the id_token and
// userinfo (P0-2 scope-gated claims included; org_ctx and roles included).
DROGON_TEST(Integration_P0_Discovery_Claims_MatchEmittedSet)
{
    DISCOVERY_RECON_SKIP_GUARD;

    Json::Value doc = fetchMetadata("/.well-known/openid-configuration");
    REQUIRE(!doc.isNull());

    const std::set<std::string> kClaims = {
      "sub",       "iss",       "aud",       "exp",
      "iat",       "nonce",     "auth_time", "acr",
      "amr",       "org_ctx",   "name",      "preferred_username",
      "picture",   "email",     "email_verified",
      "roles",
    };
    CHECK(stringSet(doc["claims_supported"]) == kClaims);
}
