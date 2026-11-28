#include "../src/controllers/AuthorizationCodeResponse.h"

#include <gtest/gtest.h>
#include <json/json.h>

#include <array>
#include <string>
#include <utility>

namespace
{
using ::fulla::drogon::controllers::detail::buildAuthorizationCodeResponse;

void expectResponse(
  const ::drogon::HttpResponsePtr &response,
  const std::string &code,
  const std::string &location,
  bool jsonMode
)
{
    ASSERT_NE(response, nullptr);
    EXPECT_TRUE(response->getHeader("cache-control").empty());
    EXPECT_TRUE(response->getHeader("pragma").empty());
    if (jsonMode)
    {
        EXPECT_EQ(response->getStatusCode(), ::drogon::k200OK);
        EXPECT_EQ(response->getContentType(), ::drogon::CT_APPLICATION_JSON);
        EXPECT_TRUE(response->getHeader("location").empty());
        const auto body = response->getJsonObject();
        ASSERT_NE(body, nullptr);
        ASSERT_TRUE(body->isObject());
        EXPECT_EQ(body->size(), 2U);
        EXPECT_TRUE((*body)["code"].isString());
        EXPECT_TRUE((*body)["location"].isString());
        EXPECT_EQ((*body)["code"].asString(), code);
        EXPECT_EQ((*body)["location"].asString(), location);
        return;
    }
    EXPECT_EQ(response->getStatusCode(), ::drogon::k302Found);
    EXPECT_EQ(response->getHeader("location"), location);
}
}  // namespace

TEST(AuthorizationCodeResponseTest, JsonPreservesExactEnvelopeAndRawCode)
{
    expectResponse(
      buildAuthorizationCodeResponse("https://rp.example/cb", "raw code", "s", true),
      "raw code",
      "https://rp.example/cb?code=raw+code&state=s",
      true
    );
}

TEST(AuthorizationCodeResponseTest, RedirectPreservesExactLocation)
{
    expectResponse(
      buildAuthorizationCodeResponse("https://rp.example/cb", "c", "s", false),
      "c",
      "https://rp.example/cb?code=c&state=s",
      false
    );
}

TEST(AuthorizationCodeResponseTest, UriSeparatorsRetainExistingSuffixBehavior)
{
    const std::array<std::pair<const char *, const char *>, 4> cases{{
      {"https://rp.example/cb", "https://rp.example/cb?code=c&state=s"},
      {"https://rp.example/cb?tenant=alpha%2Bbeta",
       "https://rp.example/cb?tenant=alpha%2Bbeta&code=c&state=s"},
      {"https://rp.example/cb?", "https://rp.example/cb?&code=c&state=s"},
      {"https://rp.example/cb?a=b&", "https://rp.example/cb?a=b&&code=c&state=s"},
    }};
    for (const auto &item : cases)
    {
        SCOPED_TRACE(item.first);
        for (bool jsonMode : {false, true})
            expectResponse(
              buildAuthorizationCodeResponse(item.first, "c", "s", jsonMode),
              "c", item.second, jsonMode
            );
    }
}

TEST(AuthorizationCodeResponseTest, EmptyAndShortStateRetainExistingShape)
{
    const std::array<std::pair<const char *, const char *>, 2> cases{{
      {"", "https://rp.example/cb?code=c"},
      {"s", "https://rp.example/cb?code=c&state=s"},
    }};
    for (const auto &item : cases)
        for (bool jsonMode : {false, true})
            expectResponse(
              buildAuthorizationCodeResponse("https://rp.example/cb", "c", item.first, jsonMode),
              "c", item.second, jsonMode
            );
}

TEST(AuthorizationCodeResponseTest, OpaqueComponentsAreEncodedOnce)
{
    const std::string code = "code +&=/%?#\"'!~*()\r\n\xE4\xB8\xAD";
    const std::string state = "state +&=%?#/\"'\r\n\xE4\xB8\xAD!~*()";
    const std::string location =
      "https://rp.example/cb?code=code+%2B%26%3D%2F%25%3F%23%22%27!~*()%0D%0A%E4%B8%AD"
      "&state=state+%2B%26%3D%25%3F%23%2F%22%27%0D%0A%E4%B8%AD!~*()";
    for (bool jsonMode : {false, true})
    {
        const auto response =
          buildAuthorizationCodeResponse("https://rp.example/cb", code, state, jsonMode);
        expectResponse(response, code, location, jsonMode);
        EXPECT_EQ(response->getHeader("location").find('\r'), std::string::npos);
        EXPECT_EQ(response->getHeader("location").find('\n'), std::string::npos);
    }
}

TEST(AuthorizationCodeResponseTest, EmptyValuesAreNotRevalidated)
{
    for (bool jsonMode : {false, true})
        expectResponse(
          buildAuthorizationCodeResponse("", "", "", jsonMode), "", "?code=", jsonMode
        );
}
