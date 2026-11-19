#include <drogon/drogon_test.h>

#include "../../../libs/storage-postgres/src/AccessTokenTimestamps.h"

#include <array>
#include <chrono>
#include <cstdint>

namespace
{
using AccessToken = ::fulla::oauth2::model::OAuth2AccessToken;
using Row = ::drogon_model::fulla_db::Oauth2AccessTokens;
using ::fulla::storage::postgres::detail::assignAccessTokenTimestamps;
}  // namespace

DROGON_TEST(Unit_P0_Storage_AccessTokenTimestamps_DefaultsFloorFractionalSeconds)
{
    constexpr int64_t seconds = 1700000000;
    for (const int fraction : {250, 750})
    {
        Row row;
        AccessToken token{};
        const std::chrono::system_clock::time_point now{
          std::chrono::seconds{seconds} + std::chrono::milliseconds{fraction}
        };
        assignAccessTokenTimestamps(row, token, now);
        REQUIRE(row.getIssuedAt() != nullptr);
        REQUIRE(row.getNotBefore() != nullptr);
        CHECK(row.getValueOfIssuedAt() == seconds);
        CHECK(row.getValueOfNotBefore() == seconds);
    }
}

DROGON_TEST(Unit_P0_Storage_AccessTokenTimestamps_PreservesExplicitAndIndependentValues)
{
    constexpr int64_t seconds = 1700000000;
    const std::chrono::system_clock::time_point now{
      std::chrono::seconds{seconds} + std::chrono::milliseconds{750}
    };
    const std::array<std::array<int64_t, 4>, 4> cases{{
      {{seconds - 100, seconds + 100, seconds - 100, seconds + 100}},
      {{seconds - 100, 0, seconds - 100, seconds}},
      {{0, seconds + 100, seconds, seconds + 100}},
      {{-7, -3, -7, -3}},
    }};
    for (const auto &entry : cases)
    {
        Row row;
        AccessToken token{};
        token.issuedAt = entry[0];
        token.notBefore = entry[1];
        assignAccessTokenTimestamps(row, token, now);
        REQUIRE(row.getIssuedAt() != nullptr);
        REQUIRE(row.getNotBefore() != nullptr);
        CHECK(row.getValueOfIssuedAt() == entry[2]);
        CHECK(row.getValueOfNotBefore() == entry[3]);
    }
}

DROGON_TEST(Unit_P0_Storage_AccessTokenTimestamps_ZeroResolvedValuesStillWriteColumns)
{
    Row row;
    AccessToken token{};
    const std::chrono::system_clock::time_point now{std::chrono::milliseconds{750}};
    assignAccessTokenTimestamps(row, token, now);
    REQUIRE(row.getIssuedAt() != nullptr);
    REQUIRE(row.getNotBefore() != nullptr);
    CHECK(row.getValueOfIssuedAt() == 0);
    CHECK(row.getValueOfNotBefore() == 0);
}
