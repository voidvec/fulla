#pragma once

// P1-1 (#279): brand-field validators for org/client branding surfaces
// (logo_uri, primary_color). Pure functions — unit-tested directly.
//
// validateBrandUri is deliberately NOT validateRedirectUri: a brand image
// is not a redirect target, so there is no registration matching and NO
// inheritance of the auth.allow_http_redirect_uri exemption (design §P1-1
// rev.1). Contract: https-only, loopback hosts exempted for dev, no
// userinfo, no fragment, bounded length.

#include <string>

namespace fulla::drogon::utils
{

/// Returns "" when the value is acceptable, otherwise a human-readable
/// rejection reason. An EMPTY value is acceptable (the fields are
/// optional; absence means "no branding").
std::string validateBrandUri(const std::string &uri);

/// Returns "" when acceptable, otherwise a rejection reason. Empty is
/// acceptable. Tightened to ^#[0-9a-fA-F]{3,6}$ — the draft's {3,8} form
/// contradicted the VARCHAR(7) column width (review rev.1).
std::string validatePrimaryColor(const std::string &color);

}  // namespace fulla::drogon::utils
