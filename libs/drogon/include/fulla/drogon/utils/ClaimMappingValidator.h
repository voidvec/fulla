#pragma once

// P1-3 (#280): write-time validation for claim mappings + the self-service
// custom-attributes document. Pure functions — unit-tested directly.
//
// Security red lines (design §P1-3 rev.1 — the adversarial review found
// an account-takeover chain in the draft; these are the closures):
//   * claim_name may NEVER shadow a standard or already-published claim
//     (reserved-word blacklist — a mapping onto `email` or `sub` would
//     let an admin rewrite the identity assertions RPs trust);
//   * source_path whitelist: `custom_attributes.<key>` (key = the
//     self-service key grammar) + exactly one built-in `user.display_name`;
//     the draft's `org.role` source is CUT (cross-context leakage);
//   * scope_name may not be `openid` (always granted — mapping on it
//     would be zero-authorization publication).

#include <string>

namespace fulla::drogon::utils
{

/// Returns "" when acceptable, otherwise the rejection reason.
std::string validateClaimMapping(
  const std::string &scopeName,
  const std::string &claimName,
  const std::string &sourcePath,
  const std::string &includeIn
);

/// Returns "" when acceptable. Rules: object node; depth <= 2; serialized
/// <= 8 KiB; every key matches ^[a-z][a-z0-9_]{0,63}$; and the prototype-
/// pollution trio (__proto__/constructor/prototype) is rejected outright
/// (the regex already excludes them; the explicit check documents the
/// intent and guards regex regressions).
std::string validateCustomAttributes(const std::string &jsonDocument);

/// Extracts the value at sourcePath from a user's attributes document for
/// claim publication. `userJson` is the users row's profile JSON
/// ({display_name?, custom_attributes?}). Returns false when the path is
/// absent or resolves to a non-scalar (objects/arrays never publish).
bool resolveClaimSourceValue(
  const std::string &sourcePath,
  const std::string &userJson,
  std::string &outValue
);

}  // namespace fulla::drogon::utils
