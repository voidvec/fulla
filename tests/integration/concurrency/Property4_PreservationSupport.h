// tests/integration/concurrency/Property4_PreservationSupport.h
//
// Spec: concurrency-lifetime-safety-audit — Task 4 (Property 4: Preservation).
// Behavioral Equivalence on ¬C(X) — observation-first BASELINE capture on the
// UNFIXED code (F). These baselines MUST PASS on the current code; they lock in
// the normal-path behavior that the fixes (tasks 6/7/8) must not change, and
// are re-run for F-vs-F' comparison at 6.4 / 7.4 / 8.5.
//
// ─────────────────────────────────────────────────────────────────────────
// WHY A SEPARATE TASK-4 SUPPORT HEADER (composition, not modification)
// ─────────────────────────────────────────────────────────────────────────
// The shared race scaffolding (`ConcurrencyRaceSupport.h`) and the Category C
// storage double (`CategoryC_DeferredStorageSupport.h`) are owned by Tasks 2/3
// and reused here verbatim (SpinBarrier, InterleavingGenerator, PendingCallbacks,
// DeferringStorage, makeLiveAccessToken/makeLiveClient/farFutureExpiry). To keep
// those stable, this header adds the Task-4-only "scoped PBT" input generators
// on TOP of them rather than editing them:
//
//   * `PreservationInputGen` — a fixed-seed std::mt19937 (same reproducible,
//     "scoped PBT without a 3rd-party property lib" approach used in Tasks 2/3)
//     that generates random-but-reproducible NORMAL inputs: random client ids,
//     scope combos, grant types, token names, request paths, and cache
//     hit/miss sequences. The fixed seed makes every recorded baseline
//     reproducible across runs and across F / F'.
//
// The consistency relaxation from the task is honored by the TESTS that use
// this header: look-aside concurrent misses may trigger N DB back-fills
// (cache stampede) — a benign, non-linearizable performance characteristic —
// so the tests compare the FINAL CONVERGED value, not strict linearizability.
//
// _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7_

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "ConcurrencyRaceSupport.h"

namespace fulla::test::concurrency
{
// A cache access in a generated hit/miss sequence.
enum class CacheOp
{
    Miss,  // key not yet seen this round -> look-aside resolves from the DB
    Hit    // key already resolved this round -> served from L1 synchronously
};

// Deterministic, seeded generator of NORMAL (¬C(X)) inputs for the Preservation
// baselines. Mirrors the Tasks 2/3 "scoped PBT" style (a fixed-seed mt19937),
// so the recorded baseline is reproducible run-to-run and comparable F vs F'.
class PreservationInputGen
{
  public:
    explicit PreservationInputGen(std::uint32_t seed) : rng_(seed)
    {
    }

    int intInRange(int lo, int hi)
    {
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(rng_);
    }

    bool boolean()
    {
        return intInRange(0, 1) == 1;
    }

    // Pick one element index in [0, n).
    std::size_t pick(std::size_t n)
    {
        if (n == 0)
            return 0;
        std::uniform_int_distribution<std::size_t> dist(0, n - 1);
        return dist(rng_);
    }

    // A reproducible unique-ish token/key string: "<prefix>-<n>".
    std::string token(const std::string &prefix)
    {
        return prefix + "-" + std::to_string(intInRange(0, 1000000));
    }

    // Random client id from a small fixed pool (random client combos).
    std::string clientId()
    {
        static const std::vector<std::string> kClients =
          {"test-client", "fulla-portal", "svc-client", "mobile-client"};
        return kClients[pick(kClients.size())];
    }

    // Random scope combo (random scope combos). Always OIDC-valid subsets.
    std::string scope()
    {
        static const std::vector<std::string> kScopes =
          {"openid", "openid profile", "openid email", "openid profile email", "profile"};
        return kScopes[pick(kScopes.size())];
    }

    // Random grant type (random grant combos).
    std::string grantType()
    {
        static const std::vector<std::string> kGrants =
          {"authorization_code", "refresh_token", "client_credentials"};
        return kGrants[pick(kGrants.size())];
    }

    // Random request path used by the RBAC / public-path baselines (3.7).
    std::string requestPath()
    {
        static const std::vector<std::string> kPaths =
          {"/api/admin/users",
           "/api/admin/settings/reset",
           "/api/user/profile",
           "/api/user/orders/42",
           "/api/public/health",
           "/oauth2/token",
           "/random/unmatched/path"};
        return kPaths[pick(kPaths.size())];
    }

    // A reproducible cache hit/miss access SEQUENCE over `distinctKeys` keys
    // and `length` accesses. The FIRST access to any key is forced to Miss
    // (cold L1), subsequent accesses to an already-seen key are Hits. This
    // models the normal look-aside read pattern whose semantics 3.4 pins.
    std::vector<std::pair<int, CacheOp>> cacheSequence(int distinctKeys, int length)
    {
        std::vector<std::pair<int, CacheOp>> seq;
        std::vector<bool> seen(static_cast<std::size_t>(distinctKeys), false);
        seq.reserve(static_cast<std::size_t>(length));
        for (int i = 0; i < length; ++i)
        {
            int key = intInRange(0, distinctKeys - 1);
            CacheOp op = seen[static_cast<std::size_t>(key)] ? CacheOp::Hit : CacheOp::Miss;
            seen[static_cast<std::size_t>(key)] = true;
            seq.emplace_back(key, op);
        }
        return seq;
    }

  private:
    std::mt19937 rng_;
};
}  // namespace fulla::test::concurrency
