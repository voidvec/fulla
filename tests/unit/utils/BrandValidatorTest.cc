// tests/unit/utils/BrandValidatorTest.cc
//
// P1-1 (#279): unit tests for the brand-field validators. The brand-URI
// rule is deliberately NOT the redirect-registration rule: no registration
// matching, no allow_http_redirect_uri inheritance — https-only with a
// loopback exemption for dev, no userinfo, no fragment, bounded length.

#include <drogon/drogon_test.h>
#include <fulla/drogon/utils/BrandValidator.h>

#include <string>

using fulla::drogon::utils::validateBrandUri;
using fulla::drogon::utils::validatePrimaryColor;

// ---------------------------------------------------------------------------
// validateBrandUri
// ---------------------------------------------------------------------------

DROGON_TEST(Unit_P1_BrandValidator_AcceptsHttpsAndEmpty)
{
    CHECK(validateBrandUri("") == "");
    CHECK(validateBrandUri("https://brand.example.com/logo.svg") == "");
    CHECK(validateBrandUri("https://cdn.example.com/a/b/logo.png?ver=1") == "");
    CHECK(validateBrandUri("https://xn--example.com/logo.svg") == "");
}

DROGON_TEST(Unit_P1_BrandValidator_LoopbackHttpExemption)
{
    CHECK(validateBrandUri("http://localhost:5173/logo.svg") == "");
    CHECK(validateBrandUri("http://127.0.0.1:8080/logo.png") == "");
    CHECK(validateBrandUri("http://[::1]:9000/logo.svg") == "");
    // The exemption is loopback-ONLY — and it must NOT inherit
    // allow_http_redirect_uri (which would widen it to arbitrary hosts).
    CHECK(!validateBrandUri("http://brand.example.com/logo.svg").empty());
    CHECK(!validateBrandUri("http://evil.example.com:443/logo.svg").empty());
}

DROGON_TEST(Unit_P1_BrandValidator_RejectsSmugglingShapes)
{
    // Userinfo would smuggle a confusing authority past a naive host check.
    CHECK(!validateBrandUri("https://brand.example.com@evil.io/logo.svg").empty());
    // Fragments never belong in a brand asset URL.
    CHECK(!validateBrandUri("https://brand.example.com/logo.svg#x").empty());
    // Non-http(s) schemes.
    CHECK(!validateBrandUri("javascript:alert(1)").empty());
    CHECK(!validateBrandUri("data:image/svg+xml;base64,AAAA").empty());
    CHECK(!validateBrandUri("ftp://brand.example.com/logo.svg").empty());
    // Missing host / not absolute.
    CHECK(!validateBrandUri("https:///logo.svg").empty());
    CHECK(!validateBrandUri("brand.example.com/logo.svg").empty());
    CHECK(!validateBrandUri("/static/logo.svg").empty());
    // Length bound mirrors the VARCHAR(512) column.
    CHECK(!validateBrandUri("https://example.com/" + std::string(600, 'a')).empty());
}

// ---------------------------------------------------------------------------
// validatePrimaryColor
// ---------------------------------------------------------------------------

DROGON_TEST(Unit_P1_BrandValidator_PrimaryColorTightenedRegex)
{
    CHECK(validatePrimaryColor("") == "");
    CHECK(validatePrimaryColor("#fff") == "");
    CHECK(validatePrimaryColor("#1A2b3C") == "");
    CHECK(validatePrimaryColor("#4f81c4") == "");
    // The {3,8} draft form contradicted the VARCHAR(7) column: 8-hex
    // (#12345678) and short (#12) forms are rejected.
    CHECK(!validatePrimaryColor("#12345678").empty());
    CHECK(!validatePrimaryColor("#12").empty());
    CHECK(!validatePrimaryColor("fff").empty());     // missing '#'
    CHECK(!validatePrimaryColor("#ff").empty());
    CHECK(!validatePrimaryColor("#gggggg").empty());  // non-hex
    CHECK(!validatePrimaryColor("#ffffff ").empty()); // trailing space
}
