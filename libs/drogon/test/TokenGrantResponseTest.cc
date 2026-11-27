#include "../src/controllers/TokenGrantResponse.h"

#include <fulla/common/testing/FakeMetrics.h>
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <utility>

namespace
{
using ::fulla::common::ports::MetricLabels;
using ::fulla::common::testing::FakeMetrics;
using ::fulla::drogon::controllers::detail::buildTokenGrantResponse;
using ::fulla::drogon::controllers::detail::TokenGrantResponseKind;

constexpr std::array<TokenGrantResponseKind, 2>
  kKinds{TokenGrantResponseKind::AuthorizationCode, TokenGrantResponseKind::RefreshToken};

Json::Value successResult()
{
    Json::Value result;
    result["access_token"] = "access";
    result["refresh_token"] = "refresh";
    result["id_token"] = "id";
    result["token_type"] = "Bearer";
    result["expires_in"] = Json::Int64(3600);
    result["roles"].append("admin");
    result["extension"]["large_integer"] = Json::Int64(9007199254740993LL);
    result["extension"]["values"].append(true);
    result["extension"]["values"].append(Json::Value());
    result["extension"]["empty"] = "";
    return result;
}

void expectCounter(const FakeMetrics::Sample &sample, int status)
{
    EXPECT_EQ(sample.kind, FakeMetrics::Kind::Counter);
    EXPECT_EQ(sample.name, "oauth2_requests_total");
    EXPECT_EQ(
      sample.labels, (MetricLabels{{"endpoint", "token"}, {"status", std::to_string(status)}})
    );
    EXPECT_EQ(sample.value, 1.0);
}

void expectDirectError(const ::drogon::HttpResponsePtr &response, const Json::Value &result)
{
    ASSERT_NE(response, nullptr);
    ASSERT_NE(response->getJsonObject(), nullptr);
    EXPECT_EQ(*response->getJsonObject(), result);
    EXPECT_EQ(response->getContentType(), ::drogon::CT_APPLICATION_JSON);
    EXPECT_TRUE(response->getHeader("cache-control").empty());
    EXPECT_TRUE(response->getHeader("pragma").empty());
    EXPECT_TRUE(response->getHeader("www-authenticate").empty());
}
}  // namespace

TEST(TokenGrantResponseTest, SuccessPreservesJsonAndGrantSpecificMetricOrder)
{
    const auto result = successResult();
    for (const auto kind : kKinds)
    {
        auto metrics = std::make_shared<FakeMetrics>();
        auto response = buildTokenGrantResponse(result, kind, metrics);
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->getStatusCode(), ::drogon::k200OK);
        ASSERT_NE(response->getJsonObject(), nullptr);
        EXPECT_EQ(*response->getJsonObject(), result);
        EXPECT_EQ(response->getContentType(), ::drogon::CT_APPLICATION_JSON);
        EXPECT_EQ(response->getHeader("cache-control"), "no-store");
        EXPECT_EQ(response->getHeader("pragma"), "no-cache");
        EXPECT_TRUE(response->getHeader("www-authenticate").empty());

        const bool authorizationCode = kind == TokenGrantResponseKind::AuthorizationCode;
        ASSERT_EQ(metrics->count(), authorizationCode ? 2U : 1U);
        expectCounter(metrics->samples()[0], 200);
        if (authorizationCode)
        {
            const auto &gauge = metrics->samples()[1];
            EXPECT_EQ(gauge.kind, FakeMetrics::Kind::Gauge);
            EXPECT_EQ(gauge.name, "oauth2_active_tokens");
            EXPECT_TRUE(gauge.labels.empty());
            EXPECT_EQ(gauge.value, 1.0);
        }
    }
}

TEST(TokenGrantResponseTest, ErrorsPreserveBodyHeadersAndStatusLabeledCounter)
{
    const std::array<std::pair<const char *, int>, 8> cases{
      {{"invalid_grant", 400},
       {"invalid_request", 400},
       {"invalid_scope", 400},
       {"unauthorized_client", 400},
       {"invalid_client", 401},
       {"access_denied", 403},
       {"server_error", 500},
       {"temporarily_unavailable", 503}}
    };
    for (const auto kind : kKinds)
    {
        for (const auto &[error, status] : cases)
        {
            SCOPED_TRACE(error);
            Json::Value result;
            result["error"] = error;
            result["error_description"] = "original description";
            result["error_uri"] = "https://example.invalid/error";
            result["extension"]["retry"] = false;
            auto metrics = std::make_shared<FakeMetrics>();
            auto response = buildTokenGrantResponse(result, kind, metrics);
            expectDirectError(response, result);
            EXPECT_EQ(static_cast<int>(response->getStatusCode()), status);
            ASSERT_EQ(metrics->count(), 1U);
            expectCounter(metrics->samples()[0], status);
        }
    }
}

TEST(TokenGrantResponseTest, UnknownEmptyAndNullErrorMembersStayErrors)
{
    for (const auto kind : kKinds)
    {
        for (const auto &error : {Json::Value("unknown_error"), Json::Value(""), Json::Value()})
        {
            Json::Value result;
            result["error"] = error;
            result["extension"] = Json::Int64(9007199254740993LL);
            auto metrics = std::make_shared<FakeMetrics>();
            auto response = buildTokenGrantResponse(result, kind, metrics);
            expectDirectError(response, result);
            EXPECT_EQ(response->getStatusCode(), ::drogon::k400BadRequest);
            EXPECT_FALSE(response->getJsonObject()->isMember("error_description"));
            ASSERT_EQ(metrics->count(), 1U);
            expectCounter(metrics->samples()[0], 400);
        }
    }
}

TEST(TokenGrantResponseTest, MissingMetricsDoesNotChangeSuccessOrError)
{
    for (const auto kind : kKinds)
    {
        auto result = successResult();
        auto response = buildTokenGrantResponse(result, kind, nullptr);
        EXPECT_EQ(response->getStatusCode(), ::drogon::k200OK);
        EXPECT_EQ(*response->getJsonObject(), result);
        EXPECT_EQ(response->getHeader("cache-control"), "no-store");
        EXPECT_EQ(response->getHeader("pragma"), "no-cache");

        result = Json::Value(Json::objectValue);
        result["error"] = "invalid_client";
        response = buildTokenGrantResponse(result, kind, nullptr);
        expectDirectError(response, result);
        EXPECT_EQ(response->getStatusCode(), ::drogon::k401Unauthorized);
    }
}

TEST(TokenGrantResponseTest, OptionalSuccessFieldsAreNotSynthesized)
{
    Json::Value result;
    result["access_token"] = "access";
    result["token_type"] = "Bearer";
    result["expires_in"] = Json::Int64(60);
    for (const auto kind : kKinds)
    {
        auto response = buildTokenGrantResponse(result, kind, nullptr);
        EXPECT_EQ(response->getStatusCode(), ::drogon::k200OK);
        EXPECT_EQ(*response->getJsonObject(), result);
        EXPECT_FALSE(response->getJsonObject()->isMember("refresh_token"));
        EXPECT_FALSE(response->getJsonObject()->isMember("id_token"));
        EXPECT_FALSE(response->getJsonObject()->isMember("scope"));
    }
}

TEST(TokenGrantResponseTest, SerializationPreservesInt64AndNestedExtensions)
{
    const auto result = successResult();
    auto response =
      buildTokenGrantResponse(result, TokenGrantResponseKind::AuthorizationCode, nullptr);
    const std::string body(response->getBody());
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value parsed;
    std::string errors;
    ASSERT_TRUE(reader->parse(body.data(), body.data() + body.size(), &parsed, &errors));
    EXPECT_EQ(parsed, result);
    EXPECT_TRUE(parsed["extension"]["large_integer"].isInt64());
    EXPECT_EQ(parsed["extension"]["large_integer"].asInt64(), Json::Int64(9007199254740993LL));
}
