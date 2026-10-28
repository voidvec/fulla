#include <drogon/drogon_test.h>
#include <fulla/common/error/ErrorCatalog.h>

#include <array>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace fulla::common::error;

// Feature: error-code-message-standardization, Property 5: Error_Catalog 完整性与唯一性
//
// 对任意 Error_Catalog 条目，其 code 为非空字符串、numeric_code 为整数且落在其
// Error_Category 对应的段位区间内（Network 1000-1099、Database 2000-2099、
// Validation 3000-3099、Authentication 4000-4099、Authorization 5000-5099、
// Internal 6000-6099）、category 属于枚举集合、httpStatus 在 100..599 之间、
// 默认 Client_Safe_Message 非空、说明长度 1..200；并且在整个目录中 code 唯一、
// numeric_code 唯一；同时 RFC 6749/7009/8628 允许集合中的每个协议错误码在目录中
// 都登记了恰好一条含 HTTP 状态码与非空默认 error_description 的条目。
//
// Validates: Requirements 3.1, 3.2, 3.3, 3.8, 2.6, 11.6
//
// The catalog is a finite static table, so the per-entry invariants are first
// asserted by an exhaustive sweep over allEntries()/allOAuthEntries(). A
// randomized sampling loop (>=100 iterations, fixed mt19937 seed for
// reproducibility) then re-checks random entries to follow the PBT convention;
// on failure it prints the offending entry/index and the seed.

namespace
{

// trantor's LogStream has no overload for iostream manipulators: streaming
// std::hex would convert the function address to bool and print `1`
// (AppleClang -Wpointer-bool-conversion). Pre-format the seed instead.
std::string seedHex(unsigned int seed)
{
    std::ostringstream oss;
    oss << std::hex << seed;
    return oss.str();
}

// Category enum set membership (Error_Category): the 7 stable enum names.
bool isKnownCategory(ErrorCategory category)
{
    switch (category)
    {
        case ErrorCategory::NETWORK:
        case ErrorCategory::DATABASE:
        case ErrorCategory::VALIDATION:
        case ErrorCategory::AUTHENTICATION:
        case ErrorCategory::AUTHORIZATION:
        case ErrorCategory::INTERNAL:
        case ErrorCategory::UNKNOWN:
            return true;
    }
    return false;
}

const char *categoryName(ErrorCategory category)
{
    switch (category)
    {
        case ErrorCategory::NETWORK:
            return "NETWORK";
        case ErrorCategory::DATABASE:
            return "DATABASE";
        case ErrorCategory::VALIDATION:
            return "VALIDATION";
        case ErrorCategory::AUTHENTICATION:
            return "AUTHENTICATION";
        case ErrorCategory::AUTHORIZATION:
            return "AUTHORIZATION";
        case ErrorCategory::INTERNAL:
            return "INTERNAL";
        case ErrorCategory::UNKNOWN:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

// Asserts every Property 5 per-entry invariant for a single Application entry.
// Returns false (and logs the offending entry) on the first violation so the
// randomized loop can print the reproduction seed.
bool checkApplicationEntryInvariants(const CatalogEntry &e)
{
    const std::string codeStr(e.code);

    // code: non-empty string (Requirement 3.1).
    if (e.code.empty())
    {
        LOG_ERROR << "Property 5 violation: empty code";
        return false;
    }

    // category: member of the Error_Category enum set (Requirement 3.1).
    if (!isKnownCategory(e.category))
    {
        LOG_ERROR << "Property 5 violation: unknown category for code '" << codeStr << "'";
        return false;
    }

    // numeric_code: integer inside the owning category segment (Requirements 3.1, 3.2).
    const NumericSegment seg = ErrorCatalog::segmentFor(e.category);
    if (e.numericCode < seg.min || e.numericCode > seg.max)
    {
        LOG_ERROR << "Property 5 violation: numeric_code " << e.numericCode << " for code '"
                  << codeStr << "' out of " << categoryName(e.category) << " segment [" << seg.min
                  << "," << seg.max << "]";
        return false;
    }

    // httpStatus in [100, 599] (Requirement 3.1).
    if (e.httpStatus < 100 || e.httpStatus > 599)
    {
        LOG_ERROR << "Property 5 violation: httpStatus " << e.httpStatus << " for code '" << codeStr
                  << "' out of [100,599]";
        return false;
    }

    // default Client_Safe_Message: non-empty (Requirement 3.1).
    if (e.defaultMessage.empty())
    {
        LOG_ERROR << "Property 5 violation: empty default message for code '" << codeStr << "'";
        return false;
    }

    // description: length 1..200 (Requirement 3.1).
    if (e.description.empty() || e.description.size() > 200)
    {
        LOG_ERROR << "Property 5 violation: description length " << e.description.size()
                  << " out of [1,200] for code '" << codeStr << "'";
        return false;
    }

    return true;
}

// RFC 6749 §5.2 base set plus RFC 7009/8628 codes that must each be registered
// exactly once (Requirement 2.6).
const std::array<std::string_view, 13> &requiredOAuthCodes()
{
    static const std::array<std::string_view, 13> kCodes = {{
      "invalid_request",
      "invalid_client",
      "invalid_grant",
      "unauthorized_client",
      "unsupported_grant_type",
      "invalid_scope",
      "server_error",
      "temporarily_unavailable",
      "access_denied",
      "unsupported_token_type",
      "authorization_pending",
      "slow_down",
      "expired_token",
    }};
    return kCodes;
}

}  // namespace

// --- Per-entry invariants: exhaustive sweep over every Application entry. ------
DROGON_TEST(Unit_P0_ErrorCatalog_Property5_EveryEntrySatisfiesInvariants)
{
    const auto &entries = ErrorCatalog::allEntries();
    REQUIRE(!entries.empty());

    for (const auto &e : entries)
    {
        // Each Property 5 per-entry invariant (code/category/numeric/http/message/desc).
        CHECK(checkApplicationEntryInvariants(e));
    }
}

// --- Catalog-wide uniqueness: code unique and numeric_code unique. ------------
DROGON_TEST(Unit_P0_ErrorCatalog_Property5_CodeAndNumericCodeUnique)
{
    const auto &entries = ErrorCatalog::allEntries();

    std::unordered_set<std::string> seenCodes;
    std::unordered_set<int> seenNumeric;

    for (const auto &e : entries)
    {
        const std::string codeStr(e.code);
        // code unique across the whole catalog (Requirement 3.3).
        CHECK(seenCodes.insert(codeStr).second);
        // numeric_code unique across the whole catalog (Requirement 3.3).
        CHECK(seenNumeric.insert(e.numericCode).second);
    }

    // Set sizes equal the entry count iff there were no duplicates.
    CHECK(seenCodes.size() == entries.size());
    CHECK(seenNumeric.size() == entries.size());
}

// --- find()/findByNumeric() agree with allEntries() (single source of truth). -
DROGON_TEST(Unit_P0_ErrorCatalog_Property5_LookupsResolveToSameEntry)
{
    const auto &entries = ErrorCatalog::allEntries();

    for (const auto &e : entries)
    {
        const CatalogEntry *byCode = ErrorCatalog::find(e.code);
        REQUIRE(byCode != nullptr);
        CHECK(byCode->numericCode == e.numericCode);

        const CatalogEntry *byNumeric = ErrorCatalog::findByNumeric(e.numericCode);
        REQUIRE(byNumeric != nullptr);
        // numeric_code uniqueness implies the reverse lookup returns the same code.
        CHECK(std::string(byNumeric->code) == std::string(e.code));
    }
}

// --- OAuth2 protocol coverage: each allowed code appears exactly once with a ---
//     valid HTTP status and a non-empty default error_description.
DROGON_TEST(Unit_P0_ErrorCatalog_Property5_OAuthAllowedSetCoveredExactlyOnce)
{
    const auto &oauthEntries = ErrorCatalog::allOAuthEntries();
    REQUIRE(!oauthEntries.empty());

    // Count occurrences of every registered protocol code.
    std::unordered_map<std::string, int> counts;
    for (const auto &o : oauthEntries)
    {
        counts[std::string(o.error)]++;

        // Every registered protocol entry must itself be well-formed (Requirement 2.6).
        CHECK(!o.error.empty());
        CHECK(o.httpStatus >= 100);
        CHECK(o.httpStatus <= 599);
        CHECK(!o.defaultErrorDesc.empty());
    }

    // Each allowed protocol code from the RFC set appears exactly once, resolves
    // via findOAuth(), and carries a valid HTTP status + non-empty description.
    for (const auto &code : requiredOAuthCodes())
    {
        const std::string codeStr(code);
        const auto it = counts.find(codeStr);
        const int count = (it == counts.end()) ? 0 : it->second;
        CHECK(count == 1);

        const OAuthCatalogEntry *entry = ErrorCatalog::findOAuth(code);
        REQUIRE(entry != nullptr);
        CHECK(entry->httpStatus >= 100);
        CHECK(entry->httpStatus <= 599);
        CHECK(!entry->defaultErrorDesc.empty());
    }
}

// --- Randomized sampling loop (PBT convention): >=100 iterations, fixed seed. --
DROGON_TEST(Unit_P0_ErrorCatalog_Property5_RandomizedSamplingHoldsInvariants)
{
    // Fixed, printable seed so any failure is reproducible.
    constexpr unsigned int kSeed = 0xCA7A106U;
    std::mt19937 gen(kSeed);

    const auto &entries = ErrorCatalog::allEntries();
    const auto &oauthEntries = ErrorCatalog::allOAuthEntries();
    REQUIRE(!entries.empty());
    REQUIRE(!oauthEntries.empty());

    std::uniform_int_distribution<size_t> appDist(0, entries.size() - 1);
    std::uniform_int_distribution<size_t> oauthDist(0, oauthEntries.size() - 1);

    constexpr int kIterations = 200;  // >= 100 per the PBT convention.
    for (int i = 0; i < kIterations; ++i)
    {
        // Sample a random Application entry and re-check the per-entry invariants.
        const size_t appIdx = appDist(gen);
        const CatalogEntry &appEntry = entries[appIdx];
        if (!checkApplicationEntryInvariants(appEntry))
        {
            // Print the reproduction context: seed, iteration and offending index.
            LOG_ERROR << "Property 5 failed: seed=0x" << seedHex(kSeed)
                      << " iteration=" << i << " appEntryIndex=" << appIdx
                      << " code=" << std::string(appEntry.code);
            FAULT(
              "Property5 randomized sampling found an invalid Application entry; "
              "seed=0xCA7A106 (see log for offending entry)"
            );
        }

        // Sample a random OAuth2 entry and re-check its well-formedness.
        const size_t oauthIdx = oauthDist(gen);
        const OAuthCatalogEntry &oauthEntry = oauthEntries[oauthIdx];
        const bool oauthOk = !oauthEntry.error.empty() && oauthEntry.httpStatus >= 100 &&
                             oauthEntry.httpStatus <= 599 && !oauthEntry.defaultErrorDesc.empty();
        if (!oauthOk)
        {
            LOG_ERROR << "Property 5 failed: seed=0x" << seedHex(kSeed)
                      << " iteration=" << i << " oauthEntryIndex=" << oauthIdx
                      << " error=" << std::string(oauthEntry.error);
            FAULT(
              "Property5 randomized sampling found an invalid OAuth2 entry; "
              "seed=0xCA7A106 (see log for offending entry)"
            );
        }
    }
}

// Feature: error-code-message-standardization, Property 4: HTTP 状态码一致性
//
// 对任意 Error_Code，经统一入口产生的响应的运行时 HTTP 状态码等于该 code 在
// Error_Catalog 中登记的 HTTP_Status_Code，且该登记值满足类别映射规则：
// VALIDATION->400、AUTHENTICATION->401、AUTHORIZATION->403、DATABASE->500、
// INTERNAL->500、UNKNOWN->500、NETWORK 且 Numeric_Error_Code 为 1002->504、
// NETWORK 且其他->502；进而属于同一 Error_Category 的全部 code 返回相同状态码
// （NETWORK 类别按数值码区分 504/502 除外），同一 code 在任意一次调用下状态码保持相同。
//
// 放宽（方案 A / 需求 11.4，保留迁移前状态码）：少数面向资源语义的 VALIDATION 条目
// 通过「条目级 httpStatus 显式覆盖」保留其迁移前的 HTTP 状态码，而非沿用 VALIDATION
// 类别默认的 400：
//   - VALIDATION_RESOURCE_NOT_FOUND -> 404
//   - VALIDATION_RESOURCE_CONFLICT  -> 409
// 因此本属性的「类别默认映射」与「同类别一致」两条子断言改为只对*未显式覆盖*的条目
// 成立；被覆盖的条目则断言其登记值等于该条目登记的覆盖状态码。核心子断言
// （运行时状态码 == Catalog 登记值，需求 4.7）对所有条目（含被覆盖者）依然成立，
// 仍能捕获真实的不一致。
//
// Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.6, 4.7, 4.8, 4.9, 2.7, 7.4, 12.2, 11.4
//
// The catalog is a finite static table, so the rules are first asserted by an
// exhaustive sweep over allEntries(). A randomized sampling loop (>=100
// iterations, fixed mt19937 seed for reproducibility) then re-checks random
// entries to follow the PBT convention; on failure it prints the offending
// code/index and the seed so the failure is reproducible.

namespace
{

// The TIMEOUT numeric code that splits the NETWORK category into 504 vs 502
// (Requirements 4.5/4.6). Registered as NET_TIMEOUT in the catalog.
constexpr int kTimeoutNumericCode = 1002;

// Entries whose HTTP status is *explicitly overridden* in the catalog to preserve
// a pre-migration status that differs from their Error_Category default (方案 A /
// Requirement 11.4). For these, the category-default mapping does NOT apply; the
// override value below is the oracle instead.
//
// Returns the overridden HTTP status for an entry, or 0 if the entry uses the
// plain category/numeric mapping. Keep this in sync with the explicit
// httpStatusOverride values registered in ErrorCatalog.cc.
int httpStatusOverrideFor(const CatalogEntry &e)
{
    if (e.code == "VALIDATION_RESOURCE_NOT_FOUND")
    {
        return 404;
    }
    if (
      e.code == "VALIDATION_RESOURCE_CONFLICT" || e.code == "VALIDATION_USERNAME_TAKEN" ||
      e.code == "VALIDATION_EMAIL_TAKEN" || e.code == "VALIDATION_CREDENTIAL_ALREADY_REGISTERED"
    )
    {
        return 409;
    }
    if (e.code == "VALIDATION_RATE_LIMITED")
    {
        return 429;
    }
    // #78: AUTHENTICATION defaults to 401, but a rejected id_token_hint is a
    // request-parameter validation failure -> explicit 400.
    if (e.code == "AUTH_INVALID_ID_TOKEN_HINT")
    {
        return 400;
    }
    // #145: AUTHENTICATION defaults to 401, but a must_change_password gate on
    // an AUTHENTICATED session is an authorization refusal -> explicit 403.
    if (e.code == "AUTH_PASSWORD_CHANGE_REQUIRED")
    {
        return 403;
    }
    // P1-2 (#278): email-template admin codes with explicit overrides
    // (NOT_FOUND keeps the 404 resource precedent; RECIPIENT_DENIED is an
    // authorization refusal -> 403; QUOTA_EXCEEDED follows the RATE_LIMITED
    // 429 precedent).
    if (e.code == "EMAIL_TEMPLATE_NOT_FOUND")
    {
        return 404;
    }
    if (e.code == "EMAIL_TEST_RECIPIENT_DENIED")
    {
        return 403;
    }
    if (e.code == "EMAIL_TEST_QUOTA_EXCEEDED")
    {
        return 429;
    }
    return 0;
}

// True iff the entry carries an explicit per-entry httpStatus override.
bool isOverridden(const CatalogEntry &e)
{
    return httpStatusOverrideFor(e) != 0;
}

// Expected HTTP status for a catalog entry. For overridden entries it is the
// explicit override (方案 A / Requirement 11.4); otherwise it is derived purely
// from the category/numeric mapping rules of Requirement 4. This is the
// independent oracle the registered httpStatus is checked against.
int expectedHttpStatusFor(const CatalogEntry &e)
{
    if (const int overrideStatus = httpStatusOverrideFor(e); overrideStatus != 0)
    {
        return overrideStatus;
    }

    switch (e.category)
    {
        case ErrorCategory::VALIDATION:
            return 400;
        case ErrorCategory::AUTHENTICATION:
            return 401;
        case ErrorCategory::AUTHORIZATION:
            return 403;
        case ErrorCategory::DATABASE:
        case ErrorCategory::INTERNAL:
        case ErrorCategory::UNKNOWN:
            return 500;
        case ErrorCategory::NETWORK:
            return e.numericCode == kTimeoutNumericCode ? 504 : 502;
    }
    return 500;
}

}  // namespace

// --- Runtime status equals the catalog-registered status (Req 4.7, 2.7, 7.4). -
//     For every registered Error_Code, the runtime HTTP status produced by
//     Error::fromCode(code, ...).toHttpStatusCode() equals the catalog httpStatus.
DROGON_TEST(Unit_P0_HttpStatus_Property4_RuntimeEqualsCatalogRegisteredValue)
{
    const auto &entries = ErrorCatalog::allEntries();
    REQUIRE(!entries.empty());

    for (const auto &e : entries)
    {
        const Error err = Error::fromCode(std::string(e.code), "req_property4");
        const int runtimeStatus = err.toHttpStatusCode();
        if (runtimeStatus != e.httpStatus)
        {
            LOG_ERROR << "Property 4 violation: code '" << std::string(e.code)
                      << "' runtime status " << runtimeStatus << " != catalog httpStatus "
                      << e.httpStatus;
        }
        CHECK(runtimeStatus == e.httpStatus);
    }
}

// --- Catalog httpStatus satisfies the category/numeric mapping rules (Req 4.1- -
//     4.4, 4.6, 4.8 and 4.5 for NET_TIMEOUT->504). For entries with an explicit
//     per-entry override (方案 A / Requirement 11.4) the oracle is the declared
//     override value instead of the category default, so 404/409 resource codes
//     are accepted while every other code is still pinned to its category rule.
DROGON_TEST(Unit_P0_HttpStatus_Property4_CatalogValueMatchesCategoryMapping)
{
    const auto &entries = ErrorCatalog::allEntries();
    REQUIRE(!entries.empty());

    for (const auto &e : entries)
    {
        const int expected = expectedHttpStatusFor(e);
        if (e.httpStatus != expected)
        {
            LOG_ERROR << "Property 4 violation: code '" << std::string(e.code) << "' ("
                      << categoryName(e.category) << ", numeric " << e.numericCode
                      << ") httpStatus " << e.httpStatus << " != expected " << expected;
        }
        CHECK(e.httpStatus == expected);
    }
}

// --- Same-category consistency (Requirement 4.9). All *non-overridden* codes of -
//     a category share one status, EXCEPT NETWORK which splits 504 (numeric 1002)
//     vs 502. Entries carrying an explicit per-entry httpStatus override (方案 A /
//     Requirement 11.4, e.g. VALIDATION_RESOURCE_NOT_FOUND->404,
//     VALIDATION_RESOURCE_CONFLICT->409) are exempt from the single-status rule;
//     they are instead asserted to equal their registered override value so the
//     test still catches a genuinely wrong override.
DROGON_TEST(Unit_P0_HttpStatus_Property4_SameCategoryConsistency)
{
    const auto &entries = ErrorCatalog::allEntries();
    REQUIRE(!entries.empty());

    // Non-NETWORK categories: the first status seen pins the whole category.
    std::unordered_map<int, int> categoryStatus;  // category ordinal -> status
    // NETWORK splits by the TIMEOUT classification (is numeric == 1002).
    std::unordered_map<bool, int> networkStatus;  // isTimeout -> status

    for (const auto &e : entries)
    {
        // Overridden entries are intentionally exempt from the same-category
        // single-status invariant; verify they match their declared override
        // (which also equals the catalog httpStatus) and move on.
        if (isOverridden(e))
        {
            const bool overrideConsistent = e.httpStatus == httpStatusOverrideFor(e);
            if (!overrideConsistent)
            {
                LOG_ERROR << "Property 4 violation: overridden code '" << std::string(e.code)
                          << "' status " << e.httpStatus << " != declared override "
                          << httpStatusOverrideFor(e);
            }
            CHECK(overrideConsistent);
            continue;
        }

        if (e.category == ErrorCategory::NETWORK)
        {
            const bool isTimeout = e.numericCode == kTimeoutNumericCode;
            const auto [it, inserted] = networkStatus.emplace(isTimeout, e.httpStatus);
            // Precompute the boolean: drogon_test's expression decomposition only
            // captures the left operand of `||`, so the condition must be a single
            // value when passed to CHECK.
            const bool consistent = inserted || it->second == e.httpStatus;
            if (!consistent)
            {
                LOG_ERROR << "Property 4 violation: NETWORK code '" << std::string(e.code)
                          << "' (isTimeout=" << isTimeout << ") status " << e.httpStatus
                          << " != prior " << it->second;
            }
            CHECK(consistent);
            continue;
        }

        const int ordinal = static_cast<int>(e.category);
        const auto [it, inserted] = categoryStatus.emplace(ordinal, e.httpStatus);
        const bool consistent = inserted || it->second == e.httpStatus;
        if (!consistent)
        {
            LOG_ERROR << "Property 4 violation: " << categoryName(e.category) << " code '"
                      << std::string(e.code) << "' status " << e.httpStatus << " != prior "
                      << it->second;
        }
        CHECK(consistent);
    }
}

// --- Determinism (Requirement 4.9, second clause). toHttpStatusCode() returns ---
//     the same value across repeated calls for the same code.
DROGON_TEST(Unit_P0_HttpStatus_Property4_DeterministicAcrossCalls)
{
    const auto &entries = ErrorCatalog::allEntries();
    REQUIRE(!entries.empty());

    for (const auto &e : entries)
    {
        const Error err = Error::fromCode(std::string(e.code), "req_property4_determinism");
        const int first = err.toHttpStatusCode();
        const int second = err.toHttpStatusCode();
        CHECK(first == second);
    }
}

// --- Randomized sampling loop (PBT convention): >=100 iterations, fixed seed. --
//     Re-checks runtime==catalog, catalog==mapping and determinism on random codes.
DROGON_TEST(Unit_P0_HttpStatus_Property4_RandomizedSamplingHoldsInvariants)
{
    // Fixed, printable seed so any failure is reproducible.
    constexpr unsigned int kSeed = 0x4040C0DEU;
    std::mt19937 gen(kSeed);

    const auto &entries = ErrorCatalog::allEntries();
    REQUIRE(!entries.empty());

    std::uniform_int_distribution<size_t> dist(0, entries.size() - 1);

    constexpr int kIterations = 200;  // >= 100 per the PBT convention.
    for (int i = 0; i < kIterations; ++i)
    {
        const size_t idx = dist(gen);
        const CatalogEntry &e = entries[idx];

        const Error err = Error::fromCode(std::string(e.code), "req_property4_rand");
        const int runtimeStatus = err.toHttpStatusCode();
        const int expected = expectedHttpStatusFor(e);

        // Runtime == catalog == category-mapping, and repeated calls are stable.
        const bool ok = runtimeStatus == e.httpStatus && e.httpStatus == expected &&
                        runtimeStatus == err.toHttpStatusCode();
        if (!ok)
        {
            LOG_ERROR << "Property 4 failed: seed=0x" << seedHex(kSeed)
                      << " iteration=" << i << " entryIndex=" << idx
                      << " code=" << std::string(e.code) << " (" << categoryName(e.category)
                      << ", numeric " << e.numericCode << ") runtimeStatus=" << runtimeStatus
                      << " catalogHttpStatus=" << e.httpStatus << " expectedMapping=" << expected;
            FAULT(
              "Property4 randomized sampling found an inconsistent HTTP status; "
              "seed=0x4040C0DE (see log for offending entry)"
            );
        }
    }
}
