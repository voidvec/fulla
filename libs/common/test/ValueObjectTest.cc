// Task 13 (fulla-sdk-refactor, design.md §6): pure gtest unit tests for
// the common::model value objects. No DB/no Drogon.

#include <fulla/common/model/Subject.h>
#include <fulla/common/model/Scope.h>
#include <fulla/common/model/ClientId.h>
#include <fulla/common/model/RedirectUri.h>
#include <fulla/common/model/PkceChallenge.h>
#include <fulla/common/model/TokenValue.h>
#include <fulla/common/model/TenantId.h>

#include <gtest/gtest.h>

using namespace fulla::common::model;

TEST(SubjectTest, ConstructsFromNonEmptyString)
{
    Subject s("local:alice");
    EXPECT_EQ(s.value(), "local:alice");
}

TEST(SubjectTest, RejectsEmpty)
{
    EXPECT_THROW(Subject(""), std::invalid_argument);
}

TEST(SubjectTest, EqualityByValue)
{
    Subject a("local:alice");
    Subject b("local:alice");
    Subject c("local:bob");
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(ScopeTest, ConstructsFromValidToken)
{
    Scope s("openid");
    EXPECT_EQ(s.value(), "openid");
}

TEST(ScopeTest, RejectsEmpty)
{
    EXPECT_THROW(Scope(""), std::invalid_argument);
}

TEST(ScopeTest, RejectsSpace)
{
    EXPECT_THROW(Scope("open id"), std::invalid_argument);
}

TEST(ScopeTest, RejectsQuoteAndBackslash)
{
    EXPECT_THROW(Scope("a\"b"), std::invalid_argument);
    EXPECT_THROW(Scope("a\\b"), std::invalid_argument);
}

TEST(ScopeTest, OrderingForSortedContainers)
{
    Scope a("admin");
    Scope b("read");
    EXPECT_LT(a, b);
}

TEST(ClientIdTest, ConstructsAndRejectsEmpty)
{
    ClientId id("fulla-portal");
    EXPECT_EQ(id.value(), "fulla-portal");
    EXPECT_THROW(ClientId(""), std::invalid_argument);
}

TEST(RedirectUriTest, ConstructsAndRejectsEmpty)
{
    RedirectUri uri("http://localhost/cb");
    EXPECT_EQ(uri.value(), "http://localhost/cb");
    EXPECT_THROW(RedirectUri(""), std::invalid_argument);
}

TEST(PkceChallengeTest, ConstructsWithS256)
{
    PkceChallenge p("abc123", "S256");
    EXPECT_EQ(p.challenge(), "abc123");
    EXPECT_EQ(p.method(), "S256");
    EXPECT_TRUE(p.isS256());
}

TEST(PkceChallengeTest, ConstructsWithPlain)
{
    PkceChallenge p("verifierstring", "plain");
    EXPECT_FALSE(p.isS256());
}

TEST(PkceChallengeTest, RejectsUnsupportedMethod)
{
    EXPECT_THROW(PkceChallenge("abc", "MD5"), std::invalid_argument);
}

TEST(PkceChallengeTest, RejectsEmptyChallenge)
{
    EXPECT_THROW(PkceChallenge("", "S256"), std::invalid_argument);
}

TEST(TokenValueTest, ConstructsAndRejectsEmpty)
{
    TokenValue t("some-opaque-token");
    EXPECT_EQ(t.value(), "some-opaque-token");
    EXPECT_THROW(TokenValue(""), std::invalid_argument);
}

TEST(TenantIdTest, DefaultConstructsToNone)
{
    TenantId t;
    EXPECT_TRUE(t.isNone());
    EXPECT_EQ(t.value(), "");
}

TEST(TenantIdTest, NoneFactoryMatchesDefault)
{
    EXPECT_EQ(TenantId::none(), TenantId());
}

TEST(TenantIdTest, NonEmptyValueIsNotNone)
{
    TenantId t("acme-corp");
    EXPECT_FALSE(t.isNone());
    EXPECT_EQ(t.value(), "acme-corp");
}
