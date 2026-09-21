#pragma once

#include <drogon/drogon.h>

#include <cstdint>
#include <optional>
#include <string>

// v1.5.0 M1 (real-tenant design §2.1 item 4): bounded multi-slot store for
// the org context an authorize request selected, keyed by the request's
// `state` value. The consent round trip (authorize -> portal -> POST
// /oauth2/consent) and the login round trip cannot carry the org binding
// through client-controlled form fields without re-validating it, so the
// binding is stashed server-side on the session and re-read (and the gate
// re-run) at code issuance.
//
// Shape: one session key (`pending_org_context_slots`) holding a JSON array
// of {s: state, o: orgId, t: mint-epoch-seconds} entries, mirroring
// ConsentCsrfSlots (#144). Mint appends (pruning expired entries and
// evicting the oldest beyond kMaxSlots); consume() removes ONLY the
// matching entry. `state` is unique per authorize request and already
// mandatory (P0-4), which makes it a sound correlation key.
//
// Thread safety: same process-wide mutex across the full
// read-modify-write as ConsentCsrfSlots (Drogon Session serializes single
// get/insert calls, not sequences). Cross-instance deployments sharing
// session storage are covered only with sticky routing (same caveat as the
// rest of the session surface).
namespace fulla::drogon::utils
{

class OrgContextSlots
{
  public:
    /// At most this many concurrent org-context-bearing authorize flows per
    /// session; the oldest slot is evicted on overflow (an evicted flow
    /// degrades to a no-org-context issuance, never a wrong-org one).
    static constexpr std::size_t kMaxSlots = 5;

    /// Slot lifetime, seconds. Matches ConsentCsrfSlots' 600s window; the
    /// authorization code TTL (10 min) bounds the useful lifetime anyway.
    static constexpr int64_t kTtlSeconds = 600;

    static constexpr const char *kSessionKey = "pending_org_context_slots";

    /// Record that the authorize request with `state` selected `orgId`.
    static void mint(
      const ::drogon::SessionPtr &session,
      const std::string &state,
      int32_t orgId,
      int64_t nowSeconds
    );

    /// Read and remove the org id stashed for `state` (one-shot: the
    /// issuance consumes the binding). Returns nullopt when the session
    /// has no slots, the state is unknown, or the slot expired.
    static std::optional<int32_t> consume(
      const ::drogon::SessionPtr &session,
      const std::string &state,
      int64_t nowSeconds
    );
};

}  // namespace fulla::drogon::utils
