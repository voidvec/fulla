#!/usr/bin/env bash
# setup-database.sh - Create database, run migrations and seed data (Linux/macOS)
set -euo pipefail

source "$(dirname "$0")/env_common.sh"

DB_USER="${FULLA_DB_USER:-fulla_user}"
DB_NAME="${FULLA_DB_NAME:-fulla_db}"
DB_PASSWORD="${FULLA_DB_PASSWORD:-123456}"
DB_HOST="${FULLA_DB_HOST:-localhost}"
DB_PORT="${FULLA_DB_PORT:-5432}"

MIGRATIONS_DIR="$FULLA_SERVER_ABS_DIR/$SQL_MIGRATIONS_REL_DIR"
SEED_DIR="$FULLA_SERVER_ABS_DIR/$SQL_SEED_REL_DIR"

# Check for psql
if ! command -v psql &>/dev/null; then
    echo "[Error] psql not found in PATH. Install PostgreSQL or add its bin"
    echo "        dir (e.g. /usr/lib/postgresql/17/bin) to PATH."
    exit 1
fi

# Force stable, parseable psql output: English messages (the probe's
# error classification below matches them; localized psql gettext output
# on Windows is code-page GBK and unmatchable) and UTF-8 client encoding
# (parity with setup_database.bat).
export PGCLIENTENCODING=UTF8

echo "Setting up $DB_NAME database..."

export PGPASSWORD="$DB_PASSWORD"

# ---------------------------------------------------------------------------
# Reachability + login probe. Classification is driven by EXIT CODES and SQL
# results only -- never by psql message text, which Windows installs
# localize (GBK) and LC_MESSAGES cannot override. set -e is suspended: a
# failed probe is DATA here, not a script abort.
# ---------------------------------------------------------------------------
# Step 1: is a PostgreSQL listening at all? (no credentials involved)
if ! pg_isready -q -h "$DB_HOST" -p "$DB_PORT" >/dev/null 2>&1; then
    echo "[Error] No PostgreSQL answering at $DB_HOST:$DB_PORT (pg_isready failed)." >&2
    echo "        Fix: start PostgreSQL, or point FULLA_DB_HOST/FULLA_DB_PORT at it." >&2
    exit 1
fi

# Step 2: does the role exist and does the password work? One parameterized
# query answers both (psql -c does not interpolate :variables, so the SQL
# goes through stdin; :'role' stays a bound parameter).
echo "Probing login as \"$DB_USER\"..."
set +e
PROBE_ERR=$(printf "SELECT 1 FROM pg_roles WHERE rolname = :'role';\n" | \
    psql -U "$DB_USER" -h "$DB_HOST" -p "$DB_PORT" -d postgres \
        -v role="$DB_USER" -tAf - 2>&1)
PROBE_RC=$?
set -e
if [ "$PROBE_RC" != "0" ]; then
    echo "[Error] Cannot log into PostgreSQL as \"$DB_USER\"@$DB_HOST:$DB_PORT:" >&2
    echo "        $PROBE_ERR" >&2
    # The server deliberately answers a wrong password and a missing role
    # with the SAME error (anti-enumeration) -- on a fresh install the role
    # is usually missing, so the fix text covers both and leads with it.
    echo "        Fix (check in order):" >&2
    echo "        a. role missing (fresh install) -> create it once from a superuser shell (use the" >&2
    echo "           same password you set in FULLA_DB_PASSWORD):" >&2
    echo "             sudo -u postgres psql -c \"CREATE ROLE $DB_USER LOGIN PASSWORD '<choose-a-password>';\"" >&2
    echo "           (docker: docker exec <pg-container> psql -U postgres -c \"CREATE ROLE $DB_USER LOGIN PASSWORD '<choose-a-password>';\" )" >&2
    exit 1
fi

echo "Dropping existing database..."
psql -U "$DB_USER" -h "$DB_HOST" -p "$DB_PORT" -d postgres \
    -c "DROP DATABASE IF EXISTS $DB_NAME;" 2>/dev/null || true

echo "Creating new database..."
# Do NOT swallow stderr: any failure must surface with root-cause specific
# guidance (parity with the .bat).
CREATE_ERR_FILE=$(mktemp)
if ! psql -U "$DB_USER" -h "$DB_HOST" -p "$DB_PORT" -d postgres     -c "CREATE DATABASE $DB_NAME;" 2>"$CREATE_ERR_FILE"; then
    rm -f "$CREATE_ERR_FILE"

    # Root cause A: already exists -- the DROP above was silently swallowed
    # (its stderr goes to /dev/null); almost always an open session (e.g. a
    # running fulla-server) holds the database. Detected by QUERY, not by
    # message text (localized installs report it in the local code page).
    DB_STILL_THERE=$(printf "SELECT 1 FROM pg_database WHERE datname = :'db';\n" | \
        psql -U "$DB_USER" -h "$DB_HOST" -p "$DB_PORT" -d postgres \
            -v db="$DB_NAME" -tAf - 2>/dev/null || true)
    if [ "$DB_STILL_THERE" = "1" ]; then
        echo "[Error] Database \"$DB_NAME\" already exists and could not be dropped" >&2
        echo "        (the earlier DROP's error was suppressed; two possible causes:" >&2
        echo "        an active connection holds the database -- e.g. a running" >&2
        echo "        fulla-server -- or the database's owner is NOT \"$DB_USER\")." >&2
        echo "        Fix: stop whatever is connected, then re-run -- or drop manually" >&2
        echo "        from a superuser shell:" >&2
        echo "          psql -U postgres -h $DB_HOST -p $DB_PORT -c \"DROP DATABASE $DB_NAME WITH (FORCE);\"" >&2
        exit 1
    fi

    # Root cause B: role lacks CREATEDB (the probe proved role+password are
    # fine; DROP needs only ownership, CREATE needs the attribute). Try to
    # self-heal once via the local 'postgres' superuser over peer auth
    # (typical on Linux/WSL), else print the exact fix.
    echo "[Warn] CREATE DATABASE failed -- checking CREATEDB on \"$DB_USER\"..."
    HAS_DB=$(psql -U "$DB_USER" -h "$DB_HOST" -p "$DB_PORT" -d postgres -tAc         "SELECT 1 FROM pg_roles WHERE rolname = '$DB_USER' AND rolcreatedb;" 2>/dev/null || true)
    CREATED_OK=0
    if [ "$HAS_DB" != "1" ] && command -v sudo >/dev/null 2>&1 &&        sudo -n -u postgres psql -tAc            "SELECT 1 FROM pg_roles WHERE rolname = '$DB_USER';" 2>/dev/null | grep -q 1; then
        echo "        Granting CREATEDB to \"$DB_USER\" via the local postgres superuser..."
        if sudo -n -u postgres psql -c "ALTER ROLE \"$DB_USER\" CREATEDB;" >/dev/null 2>&1; then
            if psql -U "$DB_USER" -h "$DB_HOST" -p "$DB_PORT" -d postgres                 -c "CREATE DATABASE $DB_NAME;"; then
                echo "        Self-healed: role granted CREATEDB, database created."
                CREATED_OK=1
            fi
        fi
    fi
    if [ "$CREATED_OK" != "1" ]; then
        echo "[Error] Failed to create database \"$DB_NAME\" as role \"$DB_USER\"." >&2
        echo "        Cause: the role lacks CREATEDB (typical for a hand-created role)." >&2
        echo "        Fix once from a superuser shell:" >&2
        echo "          sudo -u postgres psql -c 'ALTER ROLE $DB_USER CREATEDB;'" >&2
        echo "        (docker: docker exec <pg-container> psql -U postgres -c 'ALTER ROLE $DB_USER CREATEDB;')" >&2
        echo "        then re-run this script." >&2
        exit 1
    fi
fi
rm -f "$CREATE_ERR_FILE"

# Apply migrations
if [ -d "$MIGRATIONS_DIR" ]; then
    echo "Applying migrations from $MIGRATIONS_DIR..."
    for f in "$MIGRATIONS_DIR"/V*.sql; do
        [ -f "$f" ] || continue
        echo "  Applying $(basename "$f")..."
        psql -U "$DB_USER" -h "$DB_HOST" -p "$DB_PORT" -d "$DB_NAME" -f "$f"
    done
else
    echo "[Error] Migrations directory not found: $MIGRATIONS_DIR"
    exit 1
fi

# Apply seed data (explicit list: benchmark-only seeds live in
# benchmarks/fulla/seed and must never land in a dev/test database)
if [ -d "$SEED_DIR" ]; then
    echo "Applying seed data from $SEED_DIR..."
    for f in dev_admin_user.sql dev_admin_console_client.sql dev_portal_client.sql dev_backend_client.sql; do
        [ -f "$SEED_DIR/$f" ] || continue
        echo "  Applying $f..."
        psql -U "$DB_USER" -h "$DB_HOST" -p "$DB_PORT" -d "$DB_NAME" -f "$SEED_DIR/$f"
    done
fi

unset PGPASSWORD
echo "Database setup complete!"
