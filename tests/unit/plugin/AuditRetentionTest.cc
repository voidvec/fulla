// tests/unit/plugin/AuditRetentionTest.cc
//
// P1-6 (#283): unit tests for the audit-retention partition selection
// (OAuth2CleanupService::retentionDropCandidates — pure, mock-clock) and the
// CSV formula-injection hardening (AuditService::csvCell).
//
// Partition naming red line (review rev.1 F2): the REAL pattern from V025
// is audit_logs_pYYYY_MM — the draft's audit_logs_yYYYYmMM would have been
// a silent no-op. These tests pin the real pattern.

#include <drogon/drogon_test.h>
#include <fulla/drogon/admin/AuditService.h>
#include <fulla/drogon/plugin/OAuth2CleanupService.h>

#include <string>
#include <vector>

using fulla::drogon::OAuth2CleanupService;
using fulla::drogon::admin::AuditService;

namespace
{
// 2026-08-16T12:00:00Z
constexpr int64_t kAug16_2026 = 1786881600;

std::vector<std::string> dropOf(
  std::vector<std::string> names,
  int months,
  int64_t now
)
{
    return OAuth2CleanupService::retentionDropCandidates(names, months, now);
}
}  // namespace

// ---------------------------------------------------------------------------
// retentionDropCandidates
// ---------------------------------------------------------------------------

DROGON_TEST(Unit_P1_AuditRetention_RealNamingPattern_SelectsOldMonths)
{
    // Retention 6 months, now = 2026-08-16 -> cutoff month = 2026-02.
    // Partitions with month < 2026-02 drop; 2026-02 and later stay.
    std::vector<std::string> names = {
      "audit_logs_p2025_08",  // 12 months back — DROP
      "audit_logs_p2026_01",  // 7 months back — DROP
      "audit_logs_p2026_02",  // exactly the cutoff month — KEEP (may hold retained rows)
      "audit_logs_p2026_07",  // last month — KEEP
      "audit_logs_p2026_08",  // current month — KEEP
      "audit_logs_p2027_01",  // future — KEEP
    };
    auto drop = dropOf(names, 6, kAug16_2026);
    CHECK(drop == std::vector<std::string>({"audit_logs_p2025_08", "audit_logs_p2026_01"}));
}

DROGON_TEST(Unit_P1_AuditRetention_DefaultAndForeignNames_NeverSelected)
{
    std::vector<std::string> names = {
      "audit_logs_default",         // the DEFAULT partition — NEVER dropped
      "audit_logs_p2020_01",        // old but real pattern — DROP
      "audit_logs_y2020m01",        // the draft's wrong pattern — ignored
      "audit_logs_p20_01",          // malformed year — ignored
      "audit_logs_p2026_13",        // invalid month — ignored
      "audit_logs_p2026_00",        // invalid month — ignored
      "users",                      // foreign relation — ignored
      "audit_logs_p2026_1",         // non-2-digit month — ignored (length mismatch)
      "audit_logs_p2026_01; DROP TABLE users",  // injection attempt — ignored
    };
    auto drop = dropOf(names, 24, kAug16_2026);
    CHECK(drop == std::vector<std::string>({"audit_logs_p2020_01"}));
}

DROGON_TEST(Unit_P1_AuditRetention_ZeroOrNegativeRetention_KeepsEverything)
{
    std::vector<std::string> names = {"audit_logs_p2020_01", "audit_logs_p2026_08"};
    CHECK(dropOf(names, 0, kAug16_2026).empty());
    CHECK(dropOf(names, -3, kAug16_2026).empty());
}

DROGON_TEST(Unit_P1_AuditRetention_CutoffArithmeticAcrossYearBoundary)
{
    // now = 2026-03-10, retention 2 months -> cutoff month 2026-01.
    constexpr int64_t mar10_2026 = 1773100800;
    std::vector<std::string> names = {
      "audit_logs_p2025_11",  // < 2026-01 — DROP
      "audit_logs_p2025_12",  // < 2026-01 — DROP
      "audit_logs_p2026_01",  // == cutoff — KEEP
      "audit_logs_p2026_02",  // KEEP
    };
    auto drop = dropOf(names, 2, mar10_2026);
    CHECK(drop == std::vector<std::string>({"audit_logs_p2025_11", "audit_logs_p2025_12"}));
}

DROGON_TEST(Unit_P1_AuditRetention_TwelveMonthRetention_YearBack)
{
    // now = 2026-08-16, retention 12 -> cutoff month 2025-08: the 2025-08
    // partition equals the cutoff month and must be KEPT (its rows from the
    // end of Aug 2025 are still within 12 months of mid-Aug 2026).
    std::vector<std::string> names = {"audit_logs_p2025_07", "audit_logs_p2025_08", "audit_logs_p2025_09"};
    auto drop = dropOf(names, 12, kAug16_2026);
    CHECK(drop == std::vector<std::string>({"audit_logs_p2025_07"}));
}

// ---------------------------------------------------------------------------
// AuditService::csvCell — formula-injection + RFC 4180
// ---------------------------------------------------------------------------

DROGON_TEST(Unit_P1_AuditCsvCell_FormulaPrefixesNeutralized)
{
    // Every OWASP dangerous leading char gets the ' prefix.
    CHECK(AuditService::csvCell("=SUM(A1:A9)") == "'=SUM(A1:A9)");
    CHECK(AuditService::csvCell("+1|cmd") == "'+1|cmd");
    CHECK(AuditService::csvCell("-2+3") == "'-2+3");
    CHECK(AuditService::csvCell("@cmd") == "'@cmd");
    CHECK(AuditService::csvCell("\tDDE") == "'\tDDE");
    // CR forces RFC 4180 quoting as well as the formula prefix.
    CHECK(AuditService::csvCell("\rDDE") == "\"'\rDDE\"");
    // Benign leading chars are untouched.
    CHECK(AuditService::csvCell("normal cell") == "normal cell");
    CHECK(AuditService::csvCell("'already prefixed") == "'already prefixed");
    CHECK(AuditService::csvCell("") == "");
}

DROGON_TEST(Unit_P1_AuditCsvCell_Rfc4180Quoting)
{
    // Separator / quote / newline force quoting; embedded quotes double.
    CHECK(AuditService::csvCell("a,b") == "\"a,b\"");
    CHECK(AuditService::csvCell("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(AuditService::csvCell("line1\nline2") == "\"line1\nline2\"");
    // Formula + special chars: prefix first, then quoted as a whole.
    CHECK(AuditService::csvCell("=1,2") == "\"'=1,2\"");
}
