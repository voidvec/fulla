-- s5-refresh-token.lua
-- S5: refresh_token — POST /oauth2/token with grant_type=refresh_token.
--
--   POST /oauth2/token
--   Content-Type: application/x-www-form-urlencoded
--   body: grant_type=refresh_token&refresh_token=<RT>&client_id=fulla-portal
--
-- Measures refresh-token rotation (V008 family logic) + new token signing.
-- The handler does an atomic CAS: UPDATE ... WHERE revoked=false RETURNING *,
-- generates a new AT + RT (same family_id), and persists the pair.
--
-- ⚠️ CRITICAL: Each RT is used EXACTLY ONCE. V008 migration introduces family
-- rotation — reusing a revoked RT triggers cascade-revocation of the entire
-- family (TokenService.cc:446-458). So each thread advances linearly through
-- its RT slice and does NOT wrap. When the slice is exhausted, the thread
-- returns nil (wrk closes that connection — this is expected and shows up
-- as socket errors in the summary; use --reseed to refresh the pool between
-- concurrency levels).
--
-- The RTs are seeded with client_id=fulla-portal (which has refresh_token in
-- its allowed_grant_types). fulla-portal is PUBLIC (token_endpoint_auth_method=
-- none), so no client_secret is needed — just client_id in the body.
--
-- Usage:
--   WRK_LIB_DIR=/path/to/lib wrk -t<T> -c<C> -d30s --latency -s s5-refresh-token.lua <URL>
--   (use run-scenario.sh --reseed lib/generated/bench_refresh_tokens.sql for
--    fresh-pool-per-level; see run-scenario.sh for details)

local LIB_DIR = os.getenv("WRK_LIB_DIR") or "benchmarks/fulla/lib"
local TOKEN_FILE = LIB_DIR .. "/generated/refresh_tokens.txt"

-- Per-thread state (loaded in init, NOT at module scope).
local tokens, tok_start, tok_end, tok_idx
local threads = {}

local function load_lines(filepath)
    local lines = {}
    local f = io.open(filepath, "r")
    if not f then error("cannot open " .. filepath) end
    for line in f:lines() do
        line = line:gsub("%s+", "")
        if line ~= "" then
            table.insert(lines, line)
        end
    end
    f:close()
    return lines
end

setup = function(thread)
    table.insert(threads, thread)
    thread:set("tid", #threads - 1)
end

init = function(args)
    -- tid is a global set by thread:set("tid", ...) in setup().
    if not tid then tid = 0 end

    tokens = load_lines(TOKEN_FILE)
    local n = #tokens

    local nthreads = tonumber(os.getenv("WRK_NTHREADS") or "1")
    if not nthreads or nthreads < 1 then nthreads = 1 end

    local per = math.floor(n / nthreads)
    local remainder = n % nthreads
    tok_start = tid * per + math.min(tid, remainder) + 1
    local extra = (tid < remainder) and 1 or 0
    tok_end = tok_start + per + extra - 1
    -- ONE-SHOT: advance linearly, no wrap. When exhausted, return nil.
    tok_idx = tok_start
end

request = function()
    -- ONE-SHOT pool: each RT used exactly once (V008 family rotation).
    -- When this thread's slice is exhausted, return nil to stop this connection.
    if tok_idx > tok_end then
        return nil
    end
    local token = tokens[tok_idx]
    tok_idx = tok_idx + 1

    wrk.method = "POST"
    wrk.path = "/oauth2/token"
    wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
    wrk.headers["Authorization"] = nil
    -- fulla-portal is PUBLIC (token_endpoint_auth_method=none) — no secret needed.
    wrk.body = "grant_type=refresh_token&refresh_token=" .. token .. "&client_id=fulla-portal"
    return wrk.format()
end
