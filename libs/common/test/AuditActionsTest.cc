// AuditActionsTest.cc
//
// P2-2 batch A gates for the audit-event closed set:
//   1. Every constant in kAllEvents is distinct (a duplicate would make
//      webhook event filters ambiguous).
//   2. The array is sorted by value (isKnownEvent's linear scan is the
//      baseline; keeping the order documented lets a later binary search
//      drop in without re-verifying the set).
//   3. Every name matches the lower_snake_case grammar (plus the single
//      documented SOCIAL_LOGIN_TOKEN_ISSUED outlier) -- a drifted name
//      silently fragments the filter vocabulary.
//   4. Spot pins: representative values are the exact strings the v1.6.0
//      audit trail already contains (renames would break stored-data
//      queries; the constants exist to STOP drift, not to rename).

#include <fulla/common/observability/AuditActions.h>

#include <algorithm>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

namespace ae = fulla::common::observability::audit_events;

std::vector<std::string_view> asVector()
{
    return std::vector<std::string_view>(ae::kAllEvents.begin(), ae::kAllEvents.end());
}

}  // namespace

TEST(AuditActionsTest, AllEventsAreDistinct)
{
    auto events = asVector();
    std::sort(events.begin(), events.end());
    const auto duplicate = std::adjacent_find(events.begin(), events.end());
    EXPECT_EQ(duplicate, events.end()) << "duplicate audit event name in kAllEvents";
}

TEST(AuditActionsTest, AllEventsAreSortedByValue)
{
    auto events = asVector();
    EXPECT_TRUE(std::is_sorted(events.begin(), events.end()))
        << "kAllEvents must stay sorted by value (header contract)";
}

TEST(AuditActionsTest, NamesMatchSnakeCaseGrammar)
{
    for (auto event : ae::kAllEvents)
    {
        if (event == ae::kSocialLoginTokenIssued)
        {
            continue;  // documented historical-casing outlier
        }
        EXPECT_FALSE(event.empty()) << "empty audit event name";
        EXPECT_FALSE(std::isupper(static_cast<unsigned char>(event.front())))
            << event << ": must start lowercase";
        EXPECT_EQ(event.find("__"), std::string_view::npos) << event << ": double underscore";
        EXPECT_NE(event.back(), '_') << event << ": trailing underscore";
        for (char c : event)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            EXPECT_TRUE(ok) << event << ": illegal char '" << c << "'";
        }
    }
}

TEST(AuditActionsTest, SpotPinsMatchTheStoredV1Trail)
{
    // Exact strings the audit_logs.action column already contains; a
    // changed constant here breaks existing queries (v1.x keeps them).
    EXPECT_STREQ(ae::kLoginSuccess, "login_success");
    EXPECT_STREQ(ae::kTokenIssued, "token_issued");
    EXPECT_STREQ(ae::kTokenRefreshed, "token_refreshed");
    EXPECT_STREQ(ae::kRefreshTokenReuseDetected, "refresh_token_reuse_detected");
    EXPECT_STREQ(ae::kSocialLoginTokenIssued, "SOCIAL_LOGIN_TOKEN_ISSUED");
    EXPECT_STREQ(ae::kWebauthnCloneDetected, "webauthn_clone_detected");
    EXPECT_STREQ(ae::kOrgOwnershipTransferred, "org_ownership_transferred");
    EXPECT_STREQ(ae::kOrgUpdated, "org_updated");
    EXPECT_STREQ(ae::kApplicationSecretRotated, "application_secret_rotated");
}

TEST(AuditActionsTest, IsKnownEventAnswersMembership)
{
    EXPECT_TRUE(ae::isKnownEvent("login_success"));
    EXPECT_TRUE(ae::isKnownEvent(ae::kSocialLoginTokenIssued));
    EXPECT_FALSE(ae::isKnownEvent("login_succes"));   // typo'd
    EXPECT_FALSE(ae::isKnownEvent("Login_Success"));  // cased
    EXPECT_FALSE(ae::isKnownEvent(""));
    EXPECT_FALSE(ae::isKnownEvent("logout"));  // not an emitted action
}
