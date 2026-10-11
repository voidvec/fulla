// tests/integration/oidc/AuthorizeContextHttpTest.cc
//
// P0-6 (#260): public application-name discovery. The login/consent screens
// consume {client_name} so the user sees WHICH app is asking before typing
// credentials. Pins: known active client -> 200 + non-empty name; unknown
// client -> 404; missing client_id -> 400.

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <json/json.h>

#include "HttpTestClient.h"

#include <string>

using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define AUTHORIZE_CONTEXT_SKIP_GUARD                            \
    do                                                          \
    {                                                           \
        if (!postgresAvailable() || !serverReachable())         \
        {                                                       \
            CHECK(true);                                        \
            return;                                             \
        }                                                       \
    } while (0)

DROGON_TEST(Integration_P0_AuthorizeContext_KnownClient_ReturnsDisplayName)
{
    AUTHORIZE_CONTEXT_SKIP_GUARD;

    auto resp = sendGet("/oauth2/authorize-context?client_id=fulla-portal", "");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, ::drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body.isMember("client_name"));
    CHECK(!body["client_name"].asString().empty());
}

DROGON_TEST(Integration_P0_AuthorizeContext_UnknownClient_Returns404)
{
    AUTHORIZE_CONTEXT_SKIP_GUARD;

    auto resp = sendGet("/oauth2/authorize-context?client_id=no-such-client", "");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, ::drogon::k404NotFound));
}

DROGON_TEST(Integration_P0_AuthorizeContext_MissingClientId_Returns400)
{
    AUTHORIZE_CONTEXT_SKIP_GUARD;

    auto resp = sendGet("/oauth2/authorize-context", "");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, ::drogon::k400BadRequest));
}
