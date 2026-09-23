#pragma once

// v1.5.0 M3 (real-tenant design §1.3 item 2, ruling R-M3-4): the owner
// soft-delete hook. Both account-deletion paths (self-service
// deleteAccount and the admin deleteUser) must, BEFORE the soft delete
// lands, auto-effect any pending succession nomination the deleting
// owner holds -- otherwise the seat freezes and only the #228 admin
// override can recover it. Orgs with NO pending nomination fall to the
// admin-takeover state on purpose (the #228 endpoint is the documented
// fallback; nothing to do here).
//
// Each effect runs the SAME single-transaction seat swap the accept
// endpoint uses (OrgSuccessionRepository::effectSuccession); a soft-
// deleted nominee cannot take the seat, so such nominations are skipped
// (that org falls to the admin-takeover state too). Any failure fails
// the whole guard so the caller can abort the deletion (fail visibly --
// a half-effected succession followed by a soft delete is the exact
// deadlock this milestone closes).

#include <drogon/HttpRequest.h>
#include <drogon/orm/DbClient.h>

#include <functional>
#include <string>

namespace fulla::drogon::utils
{

/// Called by the delete paths with the deleting user's internal id and
/// the request (the auto-effect audit event org_successor_auto_effected
/// rides the request's actor/IP context). `done(ok, detail)` fires
/// once: ok=false aborts the deletion (detail names the failing org for
/// the log line).
void effectPendingSuccessionsForUser(
  const ::drogon::orm::DbClientPtr &db,
  int32_t userId,
  const ::drogon::HttpRequestPtr &req,
  std::function<void(bool, const std::string &)> &&done
);

}  // namespace fulla::drogon::utils
