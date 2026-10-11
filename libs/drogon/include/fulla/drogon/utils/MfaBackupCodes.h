#pragma once

// P0-3 (#257): pure helpers for MFA backup-code matching and consumption.
//
// Storage contract (users.mfa_backup_codes): a JSON array *string* in a text
// column, one SHA-256 hex hash per backup code. Two write paths hash with
// different hex case (enable: CryptoUtils detail::cryptoProvider().sha256Hex,
// lowercase; regenerate: utils::hashToken, uppercase), so matching is
// case-insensitive over the hex representation — see
// deliverables/p0-fix-design-2026-10.md §P0-3 and issue #257.
//
// Deliberately header-only pure functions: the consumption decision runs in
// the drogon layer (MfaController) because identity's IMfaRepository must not
// grow interface methods in a minor release (api-diff gate), and these pure
// pieces are unit-testable without a database.

#include <json/json.h>

#include <cctype>
#include <sstream>
#include <string>

#include <fulla/drogon/utils/CryptoUtils.h>

namespace fulla::drogon::utils::mfa_backup
{

namespace detail
{

inline std::string toLowerHex(std::string s)
{
    for (char &c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline bool parseArray(const std::string &storedJsonArray, Json::Value &out)
{
    if (storedJsonArray.empty())
        return false;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream in(storedJsonArray);
    return Json::parseFromStream(builder, in, &out, &errs) && out.isArray();
}

}  // namespace detail

// Returns the index of the entry matching sha256(plainCode), compared
// case-insensitively; -1 = no match / unparsable / empty input.
inline int matchBackupCode(const std::string &storedJsonArray, const std::string &plainCode)
{
    if (plainCode.empty())
        return -1;
    Json::Value arr;
    if (!detail::parseArray(storedJsonArray, arr))
        return -1;
    const std::string expected =
      detail::toLowerHex(::fulla::drogon::utils::detail::cryptoProvider().sha256Hex(plainCode));
    for (Json::ArrayIndex i = 0; i < arr.size(); ++i)
    {
        if (arr[i].isString() && detail::toLowerHex(arr[i].asString()) == expected)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Re-serializes the array without entry `index`. Returns "" on parse failure
// (callers treat that as consumption failure, never as "remove everything").
inline std::string removeEntry(const std::string &storedJsonArray, int index)
{
    if (index < 0)
        return "";
    Json::Value arr;
    if (!detail::parseArray(storedJsonArray, arr) || index >= static_cast<int>(arr.size()))
        return "";
    Json::Value kept(Json::arrayValue);
    for (Json::ArrayIndex i = 0; i < arr.size(); ++i)
    {
        if (static_cast<int>(i) != index)
            kept.append(arr[i]);
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, kept);
}

// Number of stored hashes; 0 on unparsable/empty input.
inline int countEntries(const std::string &storedJsonArray)
{
    Json::Value arr;
    if (!detail::parseArray(storedJsonArray, arr))
        return 0;
    return static_cast<int>(arr.size());
}

}  // namespace fulla::drogon::utils::mfa_backup
