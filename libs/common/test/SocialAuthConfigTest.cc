#include <fulla/common/utils/SocialAuthConfig.h>
#include <gtest/gtest.h>
using namespace fulla::common::utils;
TEST(SocialAuthConfigTest, CanonicalCallbackIdentity)
{
    EXPECT_EQ(socialOriginAndPath("https://PORTAL.test:443/callback/shared?provider=github"),
              socialOriginAndPath("https://portal.test/callback/shared/?provider=google"));
    EXPECT_NE(socialOriginAndPath("https://portal.test/callback/github"),
              socialOriginAndPath("https://portal.test/callback/google"));
    for (const auto *uri : {"https://user@portal.test/cb", "https://portal.test/cb#fragment",
                           "https://portal.test/a/../cb", "https://portal.test/%63b",
                           "https://portal.test:99999/cb", "https://portal.test/cb\\bad",
                           "https://portal.test/cb bad", "javascript:alert(1)"})
        EXPECT_THROW(socialOriginAndPath(uri), std::invalid_argument);
}
TEST(SocialAuthConfigTest, RedirectResolutionAndPlaceholders)
{
    Json::Value config;
    config["frontend"]["url"] = "https://portal.test/";
    EXPECT_EQ(socialRedirectUri(config, "google"), "https://portal.test/callback/google");
    config["external_auth"]["google"]["redirect_uri"] = "https://other.test/callback/google";
    EXPECT_EQ(socialRedirectUri(config, "google"), "https://other.test/callback/google");
    EXPECT_FALSE(socialCredentialConfigured("YOUR_SECRET"));
    EXPECT_FALSE(socialCredentialConfigured("OVERRIDE_SECRET"));
    EXPECT_FALSE(socialCredentialConfigured(""));
    EXPECT_TRUE(socialCredentialConfigured("configured-secret"));
}
