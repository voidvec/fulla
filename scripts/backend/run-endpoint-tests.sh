#!/usr/bin/env bash
# CMake/ctest wrapper for the out-of-process endpoint tests (方案 A).
#
# Starts the fulla server as a background process, waits for health
# readiness, runs the OAuth2 + Admin endpoint test scripts against it, then
# stops the server. Returns non-zero if any endpoint test failed.
#
# Registered as a single ctest entry (EndpointTests_OutOfProcess) so that
# `ctest` alone exercises the full out-of-process HTTP stack -- the tests
# formerly reachable only via full-test.sh Step 6-7.
#
# Prerequisites (NOT managed by this script -- must be satisfied by the
# environment, same as full-test.sh):
#   - PostgreSQL + Redis running
#   - Database migrated + seeded (setup-database.sh)
#   - jq installed (for JSON parsing in the endpoint test scripts)
#   - Server binary built
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER_EXE=""
BASE_URL="http://127.0.0.1:5555"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --server-exe) SERVER_EXE="$2"; shift 2 ;;
        --base-url)   BASE_URL="$2";   shift 2 ;;
        *) shift ;;
    esac
done

if [ -z "$SERVER_EXE" ]; then
    echo "ERROR: --server-exe is required"
    exit 2
fi
# BSD readlink has no -e; follow links explicitly and resolve the parent physically.
RESOLVED_EXE="$SERVER_EXE"
for _ in $(seq 1 40); do
    [ -L "$RESOLVED_EXE" ] || break
    LINK_TARGET="$(readlink "$RESOLVED_EXE")" || exit 2
    case "$LINK_TARGET" in
        /*) RESOLVED_EXE="$LINK_TARGET" ;;
        *)  RESOLVED_EXE="$(dirname "$RESOLVED_EXE")/$LINK_TARGET" ;;
    esac
done
if [ -L "$RESOLVED_EXE" ] || [ ! -f "$RESOLVED_EXE" ] || [ ! -x "$RESOLVED_EXE" ]; then
    echo "ERROR: server executable is not an executable file: $SERVER_EXE"
    exit 2
fi
SERVER_DIR="$(cd "$(dirname "$RESOLVED_EXE")" && pwd -P)" || exit 2
SERVER_EXE="$SERVER_DIR/$(basename "$RESOLVED_EXE")"

RESULT=0

# Kill any stale server instance on the test port. NOTE: do NOT use
# `pkill -f fulla-server` -- this script's own argv contains the
# server binary path (--server-exe /path/fulla-server), so pkill -f
# would match and kill THIS script. fuser targets the port occupant
# instead, which is always the server process, never the wrapper.
fuser -k 5555/tcp 2>/dev/null || true
sleep 1

RUN_DIR="$(mktemp -d "$SERVER_DIR/.endpoint-tests.XXXXXX")" || exit 2
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        echo "[endpoint-wrapper] Stopping server (PID $SERVER_PID)"
        kill "$SERVER_PID" 2>/dev/null || true
        # Grace period for clean shutdown, then force-kill + reap. Do NOT use
        # bare `wait` before SIGKILL -- it hangs if the server ignores SIGTERM.
        for _ in 1 2 3 4 5; do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 1
        done
        kill -9 "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true  # reap zombie (safe after SIGKILL)
    fi
    rm -rf -- "$RUN_DIR"
}
trap cleanup EXIT

# Drogon relaunches a failed daemon; TSan cannot create threads after that fork.
# Keep runtime files beside the normal server resources while preserving the
# parent-directory .env lookup.
if ! jq --arg server_dir "$SERVER_DIR" \
    '.app.relaunch_on_error = false
     | .app.run_as_daemon = false
     | .app.document_root = $server_dir
     | .app.upload_path = ($server_dir + "/uploads")
     | .app.dynamic_views_path = [($server_dir + "/views")]
     | .app.log.log_path = ($server_dir + "/logs")' \
    "$SERVER_DIR/config.json" >"$RUN_DIR/config.json"; then
    echo "[endpoint-wrapper] Failed to create isolated runtime config"
    exit 2
fi
echo "[endpoint-wrapper] Starting server: $SERVER_EXE"
echo "[endpoint-wrapper] Working dir:     $RUN_DIR"

cd "$RUN_DIR" || exit 2
# Redirect server stdout/stderr to a log file so it does NOT inherit the
# wrapper's stdout pipe -- otherwise ctest waits for EOF on the pipe even
# after the wrapper exits (the server holds the write end open).
"$SERVER_EXE" >/tmp/fulla-ep-test.log 2>&1 &
SERVER_PID=$!
disown "$SERVER_PID" 2>/dev/null || true  # detach so bash won't wait on exit

# Wait for health readiness (up to 30s).
ready=false
for i in $(seq 1 30); do
    if curl -s "$BASE_URL/health/live" 2>/dev/null | grep -q '"ok"'; then
        ready=true; break
    fi
    sleep 1
done
if [ "$ready" != true ]; then
    echo "[endpoint-wrapper] Server did not become ready within 30s (PostgreSQL/Redis/DB seed may be unavailable)"
    exit 1
fi
echo "[endpoint-wrapper] Server ready (PID $SERVER_PID)"

# Run the endpoint test scripts.
bash "$SCRIPT_DIR/test-oauth2-endpoints.sh" || RESULT=1
bash "$SCRIPT_DIR/test-admin-endpoints.sh" || RESULT=1

if [ "$RESULT" -eq 0 ]; then
    echo "[endpoint-wrapper] ALL endpoint tests PASSED"
else
    echo "[endpoint-wrapper] Some endpoint tests FAILED"
fi
exit $RESULT
