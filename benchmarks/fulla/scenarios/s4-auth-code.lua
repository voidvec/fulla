-- s4-auth-code.lua
-- S4: authorization_code flow with PKCE — the heaviest single-request path.
--
-- This is a TWO-STEP flow:
--   Step 1 (login):  POST /oauth2/login → get authorization code
--   Step 2 (token):  POST /oauth2/token (grant=authorization_code) → get tokens
--
-- wrk's request() returns one HTTP request per call. To model the full
-- auth_code flow, we ALTERNATE between step 1 and step 2 on each request()
-- invocation, carrying the `code` from step 1's response into step 2's body.
-- wrk counts both as separate requests — this is correct: we're measuring
-- the combined throughput of the auth_code flow across both endpoints.
--
-- ⚠️ CRITICAL CONSTRAINT: This scenario MUST be run with -c == -t (one
-- connection per thread). wrk shares file-scope state across all connections
-- within a thread, so the inter-request dependency (code from step 1's
-- response feeds step 2's body) would be corrupted if multiple connections
-- interleave within a thread. Use the --threads-equals-conns flag in
-- run-scenario.sh (or manually pass -t == -c), e.g.:
--   bash run-scenario.sh scenarios/s4-auth-code.lua 2 4 8
--   (run-scenario.sh automatically uses -t == -c for this scenario)
--
-- Key constraints (verified against codebase):
--   * PKCE is MANDATORY (F-011 / RFC 9700 §2.1.1): require_pkce_for_public=true
--     in all shipped configs. The /login step MUST send code_challenge (S256),
--     and the /token step MUST send the matching code_verifier.
--   * json=true makes /login return {"code":"...","location":"..."} instead of
--     a 302 redirect (SessionController.cc:748). We parse the JSON to extract
--     the code. It's read via getParameter which covers form body, so we pass
--     it in the POST body.
--   * Each VU must use a DIFFERENT bench_user_NNNN (progressive lockout at
--     5/10/15/20 failed logins — AuthService.cc:149-157). We distribute users
--     across threads.
--   * PKCE pairs are pre-generated (wrk Lua has no SHA256). Each thread cycles
--     through its slice of pkce_pairs.txt.
--
-- Usage:
--   WRK_LIB_DIR=/path/to/lib wrk -t<T> -c<C> -d30s --latency -s s4-auth-code.lua <URL>

local LIB_DIR = os.getenv("WRK_LIB_DIR") or "benchmarks/fulla/lib"
local USER_FILE = LIB_DIR .. "/generated/bench_users.txt"
local PKCE_FILE = LIB_DIR .. "/generated/pkce_pairs.txt"

-- Per-thread state. In wrk, each worker thread re-executes the script and
-- runs init(); setup() only runs in the main thread. So data must be loaded
-- in init(), NOT at module scope.
local users, pkce_pairs
local user_start, user_end, user_idx
local pkce_start, pkce_end, pkce_idx

-- Step state per thread: 0 = next request is login, 1 = next request is token
local step = 0
local pending_code = nil
local pending_verifier = nil
local req_counter = 0

local threads = {}

-- Simple JSON value extractor: finds "code":"value" in the response body.
local function extract_code(body)
    if not body then return nil end
    return body:match('"code"%s*:%s*"([^"]+)"')
end

-- Load a file as a 1-indexed table of lines (stripped).
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
    -- Ensure it has a value (fallback to 0 for single-threaded runs).
    if not tid then tid = 0 end

    -- Load data in THIS worker thread's Lua state.
    users = load_lines(USER_FILE)
    pkce_pairs = load_lines(PKCE_FILE)

    local nthreads = tonumber(os.getenv("WRK_NTHREADS") or "1")
    if not nthreads or nthreads < 1 then nthreads = 1 end

    -- User slice
    local u_per = math.floor(#users / nthreads)
    local u_rem = #users % nthreads
    user_start = tid * u_per + math.min(tid, u_rem) + 1
    local u_extra = (tid < u_rem) and 1 or 0
    user_end = user_start + u_per + u_extra - 1
    user_idx = user_start

    -- PKCE slice
    local p_per = math.floor(#pkce_pairs / nthreads)
    local p_rem = #pkce_pairs % nthreads
    pkce_start = tid * p_per + math.min(tid, p_rem) + 1
    local p_extra = (tid < p_rem) and 1 or 0
    pkce_end = pkce_start + p_per + p_extra - 1
    pkce_idx = pkce_start

    -- Reset per-thread state
    step = 0
    pending_code = nil
    pending_verifier = nil
    req_counter = 0
end

-- Advance user index with wrap (reusable within thread's slice)
local function next_user()
    local u = users[user_idx]
    user_idx = user_idx + 1
    if user_idx > user_end then
        user_idx = user_start
    end
    return u
end

-- Advance PKCE pair index with wrap
local function next_pkce()
    local pair = pkce_pairs[pkce_idx]
    pkce_idx = pkce_idx + 1
    if pkce_idx > pkce_end then
        pkce_idx = pkce_start
    end
    -- pair is "verifier,challenge"
    local verifier, challenge = pair:match("^([^,]+),(.+)$")
    return verifier, challenge
end

request = function()
    req_counter = req_counter + 1

    if step == 0 then
        -- Step 1: login → get code
        local username = next_user()
        local verifier, challenge = next_pkce()

        -- Save verifier for the token exchange step
        pending_verifier = verifier

        wrk.method = "POST"
        wrk.path = "/oauth2/login"
        wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
        wrk.headers["Authorization"] = nil
        -- json=true in the body (getParameter covers form body, SessionController.cc:748)
        wrk.body = string.format(
            "username=%s&password=admin&client_id=fulla-portal"
                .. "&redirect_uri=http://127.0.0.1:5173/callback"
                .. "&scope=openid+profile"
                .. "&state=t%d-r%d"
                .. "&code_challenge=%s&code_challenge_method=S256"
                .. "&json=true",
            username, tid, req_counter, challenge
        )

        -- Advance to step 1 (token exchange on next request() call)
        step = 1
    else
        -- Step 2: token exchange with the code from the previous login.
        -- If login failed (no code), send a token request that will 400 —
        -- this shows up in error_rate, which is the correct signal.
        local code = pending_code or ""
        local verifier = pending_verifier or ""

        wrk.method = "POST"
        wrk.path = "/oauth2/token"
        wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
        wrk.headers["Authorization"] = nil
        wrk.body = string.format(
            "grant_type=authorization_code&code=%s"
                .. "&redirect_uri=http://127.0.0.1:5173/callback"
                .. "&client_id=fulla-portal"
                .. "&code_verifier=%s",
            code, verifier
        )

        -- Back to step 0 (login) for the next cycle
        step = 0
        pending_code = nil
        pending_verifier = nil
    end

    return wrk.format()
end

-- response() captures the code from the login step's JSON response.
-- Called by wrk after each request completes.
response = function(status, headers, body)
    if step == 1 and status == 200 then
        -- We just sent the login request (step was set to 1 in request()).
        -- This response is for that login — extract the code for the next
        -- request (token exchange).
        local code = extract_code(body)
        if code then
            pending_code = code
        end
    end
end
