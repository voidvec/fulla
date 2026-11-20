#include "../src/controllers/TokenSuccessPayload.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace
{
using ::fulla::drogon::controllers::detail::buildClientCredentialsTokenPayload;
using ::fulla::drogon::controllers::detail::buildDeviceTokenPayload;

Json::Value parseJson(const std::string &text)
{
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value json;
    std::string errors;
    if (!reader->parse(text.data(), text.data() + text.size(), &json, &errors))
        return {};
    return json;
}

Json::Value serializeAndRead(const Json::Value &json)
{
    Json::StreamWriterBuilder builder;
    return parseJson(Json::writeString(builder, json));
}
}  // namespace

TEST(TokenSuccessPayloadTest, ClientCredentialsHasExactlyItsFourFields)
{
    const auto json = buildClientCredentialsTokenPayload("at", 3600, "tokens:read");
    const auto expected = parseJson(
      R"({"access_token":"at","token_type":"Bearer","expires_in":3600,"scope":"tokens:read"})"
    );
    ASSERT_TRUE(expected.isObject());
    EXPECT_EQ(json, expected);
    EXPECT_EQ(serializeAndRead(json), expected);
}

TEST(TokenSuccessPayloadTest, ClientCredentialsKeepsEmptyAndUnnormalizedScope)
{
    const auto empty = buildClientCredentialsTokenPayload("at", 60, "");
    ASSERT_TRUE(empty.isMember("scope"));
    EXPECT_TRUE(empty["scope"].isString());
    EXPECT_EQ(empty["scope"].asString(), "");
    EXPECT_EQ(empty.size(), 4U);

    const std::string scope = " profile  openid ";
    const auto spaced = buildClientCredentialsTokenPayload("at", 60, scope);
    EXPECT_EQ(spaced["scope"].asString(), scope);
    EXPECT_EQ(spaced.size(), 4U);
    EXPECT_FALSE(spaced.isMember("id_token"));
    EXPECT_FALSE(spaced.isMember("refresh_token"));
}

TEST(TokenSuccessPayloadTest, DeviceOmitsOnlyEmptyScope)
{
    const auto empty = buildDeviceTokenPayload("at", 60, "rt", "");
    const auto scoped = buildDeviceTokenPayload("at", 60, "rt", "profile");
    const auto expectedEmpty = parseJson(
      R"({"access_token":"at","token_type":"Bearer","expires_in":60,"refresh_token":"rt"})"
    );
    const auto expectedScoped = parseJson(
      R"({"access_token":"at","token_type":"Bearer","expires_in":60,"refresh_token":"rt","scope":"profile"})"
    );
    ASSERT_TRUE(expectedEmpty.isObject());
    ASSERT_TRUE(expectedScoped.isObject());
    EXPECT_EQ(empty, expectedEmpty);
    EXPECT_EQ(scoped, expectedScoped);
    EXPECT_EQ(serializeAndRead(empty), expectedEmpty);
    EXPECT_EQ(serializeAndRead(scoped), expectedScoped);
}

TEST(TokenSuccessPayloadTest, DeviceKeepsEmptyRefreshTokenAndLeavesSigningToCaller)
{
    const auto json = buildDeviceTokenPayload("at", 60, "", "openid profile");
    ASSERT_TRUE(json.isMember("refresh_token"));
    EXPECT_TRUE(json["refresh_token"].isString());
    EXPECT_TRUE(json["refresh_token"].asString().empty());
    EXPECT_EQ(json["scope"].asString(), "openid profile");
    EXPECT_FALSE(json.isMember("id_token"));
    EXPECT_FALSE(json.isMember("roles"));
    EXPECT_EQ(json.size(), 5U);
}

TEST(TokenSuccessPayloadTest, ExpiresInPreservesSignedInt64WithoutPolicyChanges)
{
    const std::array<int64_t, 5> values{
      0, -1, int64_t{1} << 40, 9007199254740993LL, std::numeric_limits<int64_t>::max()
    };
    for (const auto value : values)
    {
        const std::array<Json::Value, 2> payloads{
          buildClientCredentialsTokenPayload("at", value, "scope"),
          buildDeviceTokenPayload("at", value, "rt", "scope")
        };
        for (const auto &json : payloads)
        {
            EXPECT_TRUE(json["expires_in"].isInt64());
            EXPECT_EQ(json["expires_in"].asInt64(), value);
            const auto parsed = serializeAndRead(json);
            ASSERT_TRUE(parsed.isObject());
            EXPECT_TRUE(parsed["expires_in"].isInt64());
            EXPECT_EQ(parsed["expires_in"].asInt64(), value);
            EXPECT_EQ(parsed, json);
        }
    }
}

TEST(TokenSuccessPayloadTest, StringsSurviveJsonEscapingAndUtf8RoundTrip)
{
    const std::string access = "a\"\\\n\t";
    const std::string refresh = "r\r";
    const std::string scope = "scope-\xE6\xB5\x8B\xE8\xAF\x95 ";
    const auto cc = serializeAndRead(buildClientCredentialsTokenPayload(access, 60, scope));
    const auto device = serializeAndRead(buildDeviceTokenPayload(access, 60, refresh, scope));
    for (const auto &json : {cc, device})
    {
        ASSERT_TRUE(json.isObject());
        EXPECT_EQ(json["access_token"].asString(), access);
        EXPECT_EQ(json["scope"].asString(), scope);
        EXPECT_EQ(json["token_type"].asString(), "Bearer");
    }
    EXPECT_EQ(device["refresh_token"].asString(), refresh);
}

TEST(TokenSuccessPayloadTest, ResultsAreIndependentAndAllowTheOriginalIdTokenAppend)
{
    auto first = buildDeviceTokenPayload("at1", 60, "rt1", "openid");
    const auto second = buildClientCredentialsTokenPayload("at2", 120, "");
    const auto third = buildDeviceTokenPayload("at3", 180, "rt3", "");
    first["id_token"] = "signed-by-controller";
    EXPECT_EQ(first["access_token"].asString(), "at1");
    EXPECT_EQ(first["refresh_token"].asString(), "rt1");
    EXPECT_EQ(first["id_token"].asString(), "signed-by-controller");
    EXPECT_FALSE(second.isMember("id_token"));
    EXPECT_FALSE(second.isMember("refresh_token"));
    EXPECT_EQ(second["scope"].asString(), "");
    EXPECT_FALSE(third.isMember("id_token"));
    EXPECT_FALSE(third.isMember("scope"));
    EXPECT_EQ(third["refresh_token"].asString(), "rt3");
}
