# Category B (Data-Race Freedom) — TSan Reproduction Findings

Spec: `concurrency-lifetime-safety-audit` — Task 2.
Property 2: **Bug Condition — Data-Race Freedom** (TSan). Covers defects **1.4** and **1.5**.

> Methodology note: these tests preserve the original Category B reproductions.
> Defect 1.5 was observed on the unfixed code under Linux TSan and is now fixed;
> the same historical test remains enabled and passes. Defect 1.4 remains an
> exploratory reproduction and has not yet been observed under TSan here.

## Files added

| File | Defect | Exercises |
|---|---|---|
| `ConcurrencyRaceSupport.h` | (shared) | TSan detection (`kTsanEnabled`), `SpinBarrier` (simultaneous release), seeded `InterleavingGenerator` (scoped-PBT interleavings). |
| `CategoryB_AuthorizationFilterRaceTest.cc` | **1.4** | Concurrent first-time `AuthorizationFilter::doFilter() → loadConfig()`. |
| `CategoryB_JwkManagerRaceTest.cc` | **1.5** | Historical one-writer reproduction plus concurrent multi-writer/reader snapshot consistency. |

These land under `tests/integration/concurrency/`, auto-collected by
`GLOB_RECURSE INTEGRATION_TESTS` in `tests/CMakeLists.txt` and compiled
into the `fulla-tests` target (ctest name `OAuth2Tests`).

## The exact unsynchronized accesses under audit

### 1.4 — `AuthorizationFilter::loadConfig()` (check-then-act, no lock)

`libs/drogon/src/filters/AuthorizationFilter.cc`:

```cpp
void AuthorizationFilter::loadConfig() {
    if (initialized_) return;                     // (R) racy read of initialized_
    ...
    rules_.push_back(rule);                        // (W) concurrent mutation, same std::vector
    ...
    publicPaths_.push_back(std::regex(...));       // (W) concurrent mutation, same std::vector
    initialized_ = true;                           // (W) racy write of initialized_
}
```

`loadConfig()` is the first statement of the public `doFilter()`. Multiple IO
threads entering `doFilter()` for the first time all read `initialized_ == false`,
fall through the unguarded check, and concurrently `push_back` into the same
`rules_` / `publicPaths_` vectors while writing `initialized_`. The test drives
this through a **token-less request**, which runs `loadConfig()` then returns 401
*before* touching the `OAuth2Plugin` — isolating the reproduction to
`rules_ / publicPaths_ / initialized_`.

### 1.5 — `JwkManager::init()` write vs `signJwt()`/`getJwks()` read (historical race)

Before the fix, `libs/oauth2/src/jwk/JwkManager.cc` had `init()` write
`rsaKey_` (`void*`), `kid_` (`std::string`), and `initialized_` (`bool`), while
`signJwt() const` and `getJwks() const` read them without synchronization. The
historical reproduction used exactly **one** `init()` writer (avoiding a second
`EVP_PKEY` allocation into `rsaKey_`) racing **N** readers. The current
implementation builds a pending `KeySet` under the initialization mutex and
atomically publishes an immutable shared snapshot; readers only load that
snapshot.

## How the tests reliably trip TSan

- A fresh instance per round guarantees the object is in its pre-init state, so
  every worker takes the unsynchronized init/transition path (the C(X) window).
- `SpinBarrier` releases all workers simultaneously to maximize collision inside
  the unsynchronized window.
- `InterleavingGenerator` (fixed seed) produces many distinct, reproducible
  concurrent configurations ("scoped PBT" without a 3rd-party property library,
  none of which is vendored in this repo).

## Explicitly NOT covered (per task scope)

- **`drogon::CacheMap` cross-loop access is NOT a Category B race case.**
  `CacheMap` carries its own internal mutex, so concurrent cross-thread access
  does not produce a data race. Expecting a TSan race there would self-refute
  under the "cannot-reproduce ⇒ root cause refuted" rule. Its real problem
  (members destroyed with the host object → UAF) is covered by **Task 3
  (Category C / ASan)**.
- **1.7 (`Metrics`)** currently only does `LOG_INFO` with no shared
  counter, so there is **no reproducible race**. No reproduction case is written
  for 1.7; it is handled as a guardrail in fix task 7.3.

## Expected TSan counterexamples (to be captured on a TSan-capable build)

When built and run with ThreadSanitizer, TSan is expected to emit
`WARNING: ThreadSanitizer: data race` reports of the following shape (exact
addresses/TIDs/line numbers will vary by build):

**1.4** — racing writes/reads on the filter's members:

```
WARNING: ThreadSanitizer: data race (pid=...)
  Write of size 8 at 0x... by thread T2:
    #0 std::vector<...>::push_back ...
    #1 oauth2::filters::AuthorizationFilter::loadConfig() AuthorizationFilter.cc:35
    #2 oauth2::filters::AuthorizationFilter::doFilter(...) AuthorizationFilter.cc:60
  Previous write of size 8 at 0x... by thread T1:
    #0 std::vector<...>::push_back ...
    #1 oauth2::filters::AuthorizationFilter::loadConfig() AuthorizationFilter.cc:35
  ... (and a race on the bool `initialized_` at AuthorizationFilter.cc:18/50)
```

**1.5** — racing write (init) vs read (sign/getJwks) on `initialized_`/`kid_`/`rsaKey_`:

```
WARNING: ThreadSanitizer: data race (pid=...)
  Read of size 1 at 0x... by thread T3:
    #0 oauth2::JwkManager::signJwt(...) const JwkManager.cc:160
  Previous write of size 1 at 0x... by thread T1:
    #0 oauth2::JwkManager::init(...) JwkManager.cc:38
  ... (and races on std::string `kid_` and void* `rsaKey_`)
```

## Build / run commands for a TSan run

Linux/macOS with a GCC/Clang toolchain (TSan and ASan are mutually exclusive —
this is the TSan build):

```bash
# From the repo root, after configuring the linux-release-tsan Conan preset:
export PATH="$(python3 tools/sanitizers/verify_tsan_dependencies.py \
  build/linux-release-tsan/conan-tsan-graph.json --print-drogon-bin):$PATH"
cmake --build --preset linux-release-tsan

# Run every registered CTest target under TSan (OAuth2Tests is only the
# aggregate Drogon Test binary, not the complete gate):
TZ=UTC ctest --test-dir build/linux-release-tsan --output-on-failure \
  --no-tests=error --timeout 900

# Or run the Category B cases directly from the test executable directory:
cd build/linux-release-tsan/tests
./fulla-tests -r \
  Integration_P1_Concurrency_1_4_AuthorizationFilter_LoadConfig_DataRace_Repro
./fulla-tests -r \
  Integration_P1_Concurrency_1_5_JwkManager_InitVsSign_DataRace_Repro
./fulla-tests -r \
  Integration_P1_Concurrency_1_5_JwkManager_ConcurrentInitAndReaders_AreConsistent
./fulla-tests -r \
  Integration_P1_Concurrency_1_5_JwkManager_ConcurrentAssignmentAndInit_AreSafe
```

CMake plumbing: `-DFULLA_SANITIZER=thread` →
`cmake/Sanitizers.cmake::oauth2_apply_sanitizer()` appends
`-fsanitize=thread -g -fno-omit-frame-pointer` to compile options and
`-fsanitize=thread` to link options for instrumented first-party targets.
Non-Debug builds emit a warning but still apply the flags.

Optional, to make the run fail hard on the first race (recommended for CI gating):

```bash
export TZ=UTC
export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1:suppressions=$PWD/tools/sanitizers/tsan.supp"
```

## Verification status (2026-11-25)

- **Defect 1.5 red:** on the unfixed `linux-release-tsan` build, the historical
  `Integration_P1_Concurrency_1_5_JwkManager_InitVsSign_DataRace_Repro` exited 66.
  TSan reported a write in `std::vector<KeyEntry>::push_back` from
  `JwkManager::init()` racing a read in `findEntry()`/`activeEntry()` from
  `signJwt()` (summary at `vector.tcc:521`).
- **Defect 1.5 green:** after serializing initialization and atomically publishing
  immutable key snapshots, the unchanged historical test passed under TSan with
  16 assertions in 1 test case and no TSan warning. The positive
  `ConcurrentInitAndReaders_AreConsistent` case also passed under TSan with 16
  assertions in 1 test case.
- **Normal-build controls:** Windows Release passed the OAuth2 GTest suite
  (199/199), the historical case (55 assertions), and the positive case
  (16 assertions).
- **Defect 1.4:** still pending a dedicated TSan-capable observation. No sanitizer
  output is claimed for it here.
- **Environment note:** MSVC itself has no TSan runtime. The observations above
  were made in WSL Ubuntu with GCC 13 and the `linux-release-tsan` preset, using
  `setarch "$(uname -m)" -R` for WSL's high-address mapping constraint.
- **Assignment red:** the concurrent copy/move-assignment versus init/read regression
  exposed a second unsynchronized access before the fix. TSan exited 66 with a
  read in `JwkManager::log()` racing the write to the logger pointer in
  `JwkManager::operator=`. The regression alternates copy assignment with
  reinitialization while another thread continuously calls init/sign/JWKS readers.
  After atomic logger publication and two-way initialization-mutex serialization for
  assignment, the same regression passed under TSan with 5 assertions.
- **Compatibility and coverage:** `JwkManager` copy/move operations share or
  transfer the immutable `KeySet` snapshot, preserving the previous public
  copy/move surface without duplicating `EVP_PKEY` ownership. The common testing
  support archive is now TSan-instrumented and verified; its 43 tests passed.
- **Out-of-process controls:** `SocialLoginState_OutOfProcess` now consumes the
  build-tree config. The endpoint wrapper uses an isolated runtime config with
  daemon/relaunch disabled, avoiding unsupported TSan post-fork thread creation;
  both endpoint tests passed in the full run.
- **Full gate (2026-11-26):** the final WSL GCC TSan run passed **701/701**
  registered tests in **575.7 seconds**, with `TZ=UTC`, `halt_on_error=1`, and an
  isolated PostgreSQL/Redis pair. Both the outer CTest JUnit and the SDK smoke
  JUnit passed the no-empty/no-skip checker. Dependency verification covered
  nine first-party targets plus the full-stack SDK smoke, including framework
  archive instrumentation and selected TSan link paths.
- **Configuration/test isolation:** canonical portal aliases win over legacy
  aliases, empty aliases fall back without erasing the JSON baseline, and the
  serial-only `.env` test seam snapshots/restores the loaded table. Regression
  probes covered `.env` shadowing and restoration. The endpoint wrapper resolves
  symlinks without GNU `readlink -e`, rejects directories/cycles, and preserves
  the target executable's physical directory.
