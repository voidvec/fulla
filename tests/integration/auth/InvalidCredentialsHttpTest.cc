#include <drogon/drogon_test.h>

#include "HttpTestClient.h"

DROGON_TEST(Integration_P1_Auth_InvalidCredentials_DoNotCrashServer)
{
    using fulla::test::http::serverReachable;
    REQUIRE(serverReachable());

    const std::string username = "__missing_" + ::drogon::utils::getUuid();
    auto response = fulla::test::http::sendPostForm(
      "/oauth2/login?json=true",
      "username=" + username +
        "&password=definitely-invalid"
        "&client_id=fulla-portal"
        "&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback"
        "&scope=openid+profile"
        "&state=memory-login-regression"
    );
    REQUIRE(response != nullptr);
    CHECK(response->getStatusCode() == ::drogon::k401Unauthorized);

    auto health = fulla::test::http::sendGet("/health");
    REQUIRE(health != nullptr);
    CHECK(health->getStatusCode() == ::drogon::k200OK);
}
