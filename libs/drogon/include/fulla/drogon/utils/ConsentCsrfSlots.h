#pragma once

#include <drogon/drogon.h>

#include <cstdint>
#include <string>

// #144: bounded multi-slot store for consent CSRF nonces, replacing the
// single `pending_consent_csrf`(+`_ts`) session keys shipped with PR #141.
//
// Problem being fixed: the single slot meant two concurrent authorize flows
// on one session (multi-tab) overwrote each other's nonce, so the first
// consent page could no longer be submitted (hard 400 on Gate 3).
//
// Shape: one session key (`pending_consent_csrf_slots`) holding a JSON array
// of {n: nonce, t: mint-epoch-seconds} entries. Mint appends (pruning
// expired entries and evicting the oldest beyond kMaxSlots); consume
// verifies a submitted nonce against the live entries and removes ONLY the
// matching entry, so two concurrent flows each keep a valid nonce.
//
// Thread safety: both mint and consume hold the same process-wide mutex
// across their full read-modify-write of the session value. Drogon Session
// serializes individual get/insert calls but NOT read-modify-write
// sequences, so a narrower lock would let a concurrent mint (a) lose a
// just-appended entry or (b) write back a pre-consumption snapshot,
// resurrecting an already-consumed one-shot nonce. Cross-instance
// deployments sharing session storage are covered only with sticky routing
// (same caveat as the PR #141 single-slot implementation).
namespace fulla::drogon::utils
{

class ConsentCsrfSlots
{
  public:
    /// At most this many concurrent consent flows per session; the oldest
    /// slot is evicted on overflow (a consent page whose nonce was evicted
    /// fails Gate 3 with the standard expired/mismatched 400).
    static constexpr std::size_t kMaxSlots = 5;

    /// One-shot nonce lifetime, seconds (unchanged from the PR #141 gate).
    static constexpr int64_t kTtlSeconds = 600;

    static constexpr const char *kSessionKey = "pending_consent_csrf_slots";

    /// Append a fresh nonce slot, first dropping expired entries and, when
    /// already at kMaxSlots, the oldest live entry.
    static void mint(
      const ::drogon::SessionPtr &session,
      const std::string &nonce,
      int64_t nowSeconds
    );

    /// Verify and consume a submitted nonce. Returns true when it matches a
    /// live (non-expired) slot; only that slot is erased. Returns false when
    /// the session has no slots, the nonce is unknown/expired, or was
    /// already consumed (replay).
    static bool consume(
      const ::drogon::SessionPtr &session,
      const std::string &nonce,
      int64_t nowSeconds
    );

    /// Non-destructive validity check (v1.5.0 M1b): same lifetime rules as
    /// consume(), but the slot is left in place. Sole purpose: GET
    /// /oauth2/consent/context rendering the consent screen BEFORE the
    /// submit; the one-shot consume stays with POST /oauth2/consent.
    static bool peek(
      const ::drogon::SessionPtr &session,
      const std::string &nonce,
      int64_t nowSeconds
    );
};

}  // namespace fulla::drogon::utils
