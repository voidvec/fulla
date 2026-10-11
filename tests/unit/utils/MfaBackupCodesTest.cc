// P0-3 (#257): unit coverage for the backup-code matching/consumption
// helpers in MfaBackupCodes.h. The two production write paths hash backup
// codes with different hex case (enable: lowercase sha256Hex; regenerate:
// uppercase hashToken) — these pin the case-insensitive match contract, the
// one-entry removal + count round-trip, and the fail-closed behavior on
// unparsable/empty storage.

#include <drogon/drogon_test.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/drogon/utils/MfaBackupCodes.h>

#include <string>

using namespace fulla::drogon::utils;

namespace
{

std::string shaLower(const std::string &plain)
{
    return detail::cryptoProvider().sha256Hex(plain);
}

std::string shaUpper(const std::string &plain)
{
    return hashToken(plain);
}

std::string jsonArray(const std::vector<std::string> &entries)
{
    Json::Value arr(Json::arrayValue);
    for (const auto &e : entries)
        arr.append(e);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, arr);
}

}  // namespace

DROGON_TEST(Unit_P0_MfaBackup_Match_EnablePathLowercaseHash)
{
    const std::string stored = jsonArray({shaLower("ABCD1234"), shaLower("EFGH5678")});
    CHECK(mfa_backup::matchBackupCode(stored, "ABCD1234") == 0);
    CHECK(mfa_backup::matchBackupCode(stored, "EFGH5678") == 1);
}

DROGON_TEST(Unit_P0_MfaBackup_Match_RegeneratePathUppercaseHash)
{
    const std::string stored = jsonArray({shaUpper("ABCD1234")});
    CHECK(mfa_backup::matchBackupCode(stored, "ABCD1234") == 0);
}

DROGON_TEST(Unit_P0_MfaBackup_Match_WrongCodeOrGarbage_ReturnsMinusOne)
{
    const std::string stored = jsonArray({shaLower("ABCD1234")});
    CHECK(mfa_backup::matchBackupCode(stored, "WRONG9999") == -1);
    CHECK(mfa_backup::matchBackupCode(stored, "") == -1);
    CHECK(mfa_backup::matchBackupCode("", "ABCD1234") == -1);
    CHECK(mfa_backup::matchBackupCode("not-json", "ABCD1234") == -1);
    CHECK(mfa_backup::matchBackupCode(R"({"a":"b"})", "ABCD1234") == -1);
}

DROGON_TEST(Unit_P0_MfaBackup_RemoveEntry_AndCount_RoundTrip)
{
    const std::string stored = jsonArray({shaLower("AAAA1111"), shaLower("BBBB2222"), shaLower("CCCC3333")});
    const std::string afterFirst = mfa_backup::removeEntry(stored, 0);
    CHECK(mfa_backup::countEntries(afterFirst) == 2);
    CHECK(mfa_backup::matchBackupCode(afterFirst, "AAAA1111") == -1);
    CHECK(mfa_backup::matchBackupCode(afterFirst, "BBBB2222") == 0);
    CHECK(mfa_backup::matchBackupCode(afterFirst, "CCCC3333") == 1);

    const std::string afterLast = mfa_backup::removeEntry(afterFirst, 0);
    CHECK(mfa_backup::countEntries(afterLast) == 1);
    CHECK(mfa_backup::matchBackupCode(afterLast, "BBBB2222") == -1);
    CHECK(mfa_backup::matchBackupCode(afterLast, "CCCC3333") == 0);

    // Removing the final entry yields an empty array, not "" — consuming the
    // last code is a valid state (remaining = 0 forces a TOTP re-bind).
    const std::string afterAll = mfa_backup::removeEntry(afterLast, 0);
    CHECK(mfa_backup::countEntries(afterAll) == 0);
    CHECK(!afterAll.empty());
}

DROGON_TEST(Unit_P0_MfaBackup_RemoveEntry_InvalidInput_ReturnsEmpty)
{
    const std::string stored = jsonArray({shaLower("AAAA1111")});
    CHECK(mfa_backup::removeEntry(stored, -1).empty());
    CHECK(mfa_backup::removeEntry(stored, 5).empty());
    CHECK(mfa_backup::removeEntry("garbage", 0).empty());
    CHECK(mfa_backup::removeEntry("", 0).empty());
    CHECK(mfa_backup::countEntries("garbage") == 0);
}
