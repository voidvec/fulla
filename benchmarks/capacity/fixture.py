"""Owned-fixture operations. SQL is confined to an explicitly leased S07 DB."""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import threading
import time
from pathlib import Path

from protocol import ProtocolError, Session, User, json_body
from support import Pacer, Redis, load_config


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


class Database:
    def __init__(self, fixture):
        self.fixture = fixture
        self.db = fixture["db"]
        self.guard = lambda: None
        if not re.fullmatch(r"fulla_s07_[a-z0-9_]+", self.db["database"]):
            raise ValueError("database must be an explicit fulla_s07_ fixture")
        if self.db.get("host") != "127.0.0.1":
            raise ValueError("fixture DB must be loopback")

    def sql(self, query):
        self.guard()
        env = os.environ.copy()
        env["PGPASSWORD"] = self.db["password"]
        command = [
            "psql",
            "-X",
            "-qAt",
            "-v",
            "ON_ERROR_STOP=1",
            "-w",
            "-h",
            self.db["host"],
            "-p",
            str(self.db["port"]),
            "-U",
            self.db["user"],
            "-d",
            self.db["database"],
        ]
        result = subprocess.run(
            command,
            input=query,
            text=True,
            capture_output=True,
            env=env,
            timeout=20,
            check=False,
        )
        if result.returncode:
            raise RuntimeError("fixture SQL failed (private details not exported)")
        return result.stdout.strip()

    def claim(self):
        count = int(
            self.sql(
                "SELECT count(*) FROM information_schema.tables WHERE table_schema='public';"
            )
        )
        if count == 0:
            self.sql(
                "CREATE TABLE _s07_owner (nonce text PRIMARY KEY); INSERT INTO _s07_owner VALUES ("
                + literal(self.fixture["owner_nonce"])
                + ");"
            )
        self.verify()

    def verify(self):
        if self.sql("SELECT nonce FROM _s07_owner;") != self.fixture["owner_nonce"]:
            raise ValueError("database fixture owner mismatch")

    def stats(self):
        self.verify()
        query = """SELECT json_build_object(
          'database_bytes',pg_database_size(current_database()),
          'at_rows',(SELECT count(*) FROM oauth2_access_tokens),
          'rt_rows',(SELECT count(*) FROM oauth2_refresh_tokens),
          'expired_at_rows',(SELECT count(*) FROM oauth2_access_tokens WHERE expires_at<extract(epoch from now())),
          'db_connections',(SELECT count(*) FROM pg_stat_activity WHERE datname=current_database()),
          'waiting_connections',(SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() AND wait_event_type='Lock'),
          'wal_bytes',(SELECT wal_bytes::text FROM pg_stat_wal),
          'wal_lsn',pg_current_wal_lsn()::text,
          'checkpoint_timed',(SELECT checkpoints_timed FROM pg_stat_bgwriter),
          'checkpoint_requested',(SELECT checkpoints_req FROM pg_stat_bgwriter),
          'checkpoint_write_ms',(SELECT checkpoint_write_time FROM pg_stat_bgwriter),
          'checkpoint_sync_ms',(SELECT checkpoint_sync_time FROM pg_stat_bgwriter),
          'checkpoint_timeout_seconds',extract(epoch from current_setting('checkpoint_timeout')::interval),
          'checkpoint_completion_target',current_setting('checkpoint_completion_target')::numeric,
          'min_wal_size_bytes',pg_size_bytes(current_setting('min_wal_size')),
          'max_wal_size_bytes',pg_size_bytes(current_setting('max_wal_size')),
          'shared_buffers_bytes',pg_size_bytes(current_setting('shared_buffers')),
          'max_connections',current_setting('max_connections')::integer,
          'archive_mode',current_setting('archive_mode'),
          'replication_slots',(SELECT count(*) FROM pg_replication_slots),
          'tables_bytes',(SELECT coalesce(sum(pg_total_relation_size(quote_ident(tablename))),0) FROM pg_tables WHERE schemaname='public'),
          'fsync',current_setting('fsync'),'synchronous_commit',current_setting('synchronous_commit'),
          'full_page_writes',current_setting('full_page_writes'),
          'pg_version',current_setting('server_version'));"""
        return json.loads(self.sql(query))

    def validate_issued_credentials(self, hashes):
        self.verify()
        if any(not re.fullmatch(r"[0-9A-F]{64}", h) for h in hashes):
            raise ValueError("invalid token digest audit input")
        unique = sorted(set(hashes))
        present = valid = 0
        client = self.fixture["confidential_client"]
        for offset in range(0, len(unique), 1000):
            values = ",".join(
                "(" + literal(h) + ")" for h in unique[offset : offset + 1000]
            )
            query = (
                "WITH wanted(token) AS (VALUES "
                + values
                + ") SELECT json_build_object('present',count(a.token),'valid',count(a.token) FILTER(WHERE a.client_id="
                + literal(client["id"])
                + " AND a.scope="
                + literal(client["scope"])
                + " AND NOT a.revoked AND a.expires_at>extract(epoch from now()))) FROM wanted w LEFT JOIN oauth2_access_tokens a ON a.token=w.token;"
            )
            result = json.loads(self.sql(query))
            present += result["present"]
            valid += result["valid"]
        if present != len(unique) or valid != len(unique):
            raise ValueError(
                "issued CC tokens are not persisted with expected client/scope/lifetime"
            )
        return {
            "issued_responses_audited": len(hashes),
            "unique_currently_valid_tokens": len(unique),
            "persisted_and_bound": valid,
        }

    def reset_tokens(self):
        self.verify()
        self.sql("TRUNCATE oauth2_refresh_tokens,oauth2_access_tokens,oauth2_codes;")

    def seed(self, repo):
        self.verify()
        for path in sorted((repo / "apps/server/seed").glob("*.sql")):
            self.sql(path.read_text())
        for account in self.fixture["accounts"]:
            password, salt = account["password"], os.urandom(16)
            hashed = (
                "$pbkdf2-sha256$310000$"
                + salt.hex()
                + "$"
                + hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 310000).hex()
            )
            self.sql(
                "INSERT INTO users(username,password_hash,salt,email,email_verified) VALUES ("
                + ",".join(
                    literal(v)
                    for v in (
                        account["username"],
                        hashed,
                        "",
                        account["username"] + "@s07.invalid",
                    )
                )
                + ",true) ON CONFLICT(username) DO NOTHING;"
            )
            self.sql(
                "INSERT INTO user_roles(user_id,role_id) SELECT u.id,r.id FROM users u,roles r WHERE u.username="
                + literal(account["username"])
                + " AND r.name='user' ON CONFLICT DO NOTHING;"
            )
            self.sql(
                "INSERT INTO oauth2_subject_mappings(subject,internal_user_id,provider) SELECT id::text,id,'local' FROM users WHERE username="
                + literal(account["username"])
                + " ON CONFLICT(provider,subject) DO NOTHING;"
            )
            account["expected_sub"] = self.sql(
                "SELECT public_sub::text FROM users WHERE username="
                + literal(account["username"])
                + ";"
            )
        actual = int(
            self.sql(
                "SELECT count(*) FROM users WHERE username LIKE 's07u%' AND password_hash LIKE '$pbkdf2-sha256$310000$%';"
            )
        )
        if actual != len(self.fixture["accounts"]):
            raise ValueError("PBKDF2 fixture cost or account count mismatch")


def cache_config(config):
    return next(p["config"] for p in config["plugins"] if p["name"] == "OAuth2Plugin")


class Server:
    def __init__(self, fixture, repo: Path, binary: Path, runtime: Path):
        self.fixture, self.repo, self.binary, self.runtime = (
            fixture,
            repo,
            binary.resolve(),
            runtime,
        )
        self.process = None
        self.log = None
        self.shutdown_events = []
        self.log_thread = None
        self.console_bytes = 0
        self.console_retained_bytes = 0

    def configuration(self, profile):
        config = load_config(self.repo / "apps/server/config/config.json")
        config["listeners"] = [
            {"address": "127.0.0.1", "port": self.fixture["port"], "https": False}
        ]
        config["db_clients"][0].update(
            host="127.0.0.1",
            port=self.fixture["db_proxy_port"],
            dbname=self.fixture["db"]["database"],
            user=self.fixture["db"]["user"],
            passwd=self.fixture["db"]["password"],
        )
        config["redis_clients"][0].update(
            host="127.0.0.1",
            port=self.fixture["redis_port"],
            passwd=self.fixture["redis_password"],
        )
        config["app"]["relaunch_on_error"] = False
        # Retention is bounded identically in both arms; DEBUG/WARN level differs explicitly.
        config["app"]["log"].update(
            log_path="logs", log_size_limit=1024 * 1024, max_files=2
        )
        if profile in ("tuned", "soak"):
            config["app"]["number_of_threads"] = 2
            config["app"]["log"]["log_level"] = "WARN"
            config["db_clients"][0]["number_of_connections"] = 16
            config["redis_clients"][0]["number_of_connections"] = 16
            cache_config(config)["cache"]["enabled"] = True
        if profile == "soak":
            prod = load_config(self.repo / "apps/server/config/config.prod.json")
            limiter = next(
                p for p in prod["plugins"] if p["name"] == "drogon::plugin::Hodor"
            )
            config["plugins"].append(limiter)
        config.setdefault("custom_config", {}).setdefault("auth", {}).setdefault(
            "bootstrap_admin", {}
        )["enabled"] = False
        return config

    def configure(self, profile):
        self.runtime.mkdir(parents=True, exist_ok=True)
        os.chmod(self.runtime, 0o700)
        for name in ("migrations", "seed"):
            target = self.runtime / name
            if not target.exists():
                target.symlink_to(
                    self.repo / "apps/server" / name, target_is_directory=True
                )
        config = self.configuration(profile)
        path = self.runtime / "config.json"
        path.write_text(json.dumps(config, indent=2))
        os.chmod(path, 0o600)
        self.fixture["runtime_directory"] = str(self.runtime.resolve())
        self.fixture["runtime_config_sha256"] = hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        self.fixture.update(
            profile=profile,
            cache_enabled=cache_config(config)["cache"]["enabled"],
            consent_url=config["custom_config"]
            .get("oauth2", {})
            .get("consent_url", "/consent"),
        )
        return config

    def migrate(self):
        self.configure("default")
        env = self.environment()
        with (self.runtime / "migration.log").open("wb") as log:
            self.process = subprocess.Popen(
                [str(self.binary), "--migrate-only"],
                cwd=self.runtime,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                code = self.process.wait(timeout=120)
            except BaseException:
                self.process.kill()
                self.process.wait(timeout=5)
                raise
            finally:
                self.process = None
        if code:
            raise RuntimeError("migration did not complete; see private runtime log")

    def environment(self):
        env = os.environ.copy()
        # Do not inherit another task's FULLA_DB/REDIS overrides.
        for key in list(env):
            if key.startswith("FULLA_"):
                del env[key]
        env.update(
            FULLA_DB_HOST="127.0.0.1",
            FULLA_DB_PORT=str(self.fixture["db_proxy_port"]),
            FULLA_DB_USER=self.fixture["db"]["user"],
            FULLA_DB_PASSWORD=self.fixture["db"]["password"],
            FULLA_DB_NAME=self.fixture["db"]["database"],
            FULLA_REDIS_HOST="127.0.0.1",
            FULLA_REDIS_PORT=str(self.fixture["redis_port"]),
            FULLA_REDIS_PASSWORD=self.fixture["redis_password"],
        )
        return env

    def start(self, profile, pacer=None):
        if self.process:
            raise ValueError("server already owned/running")
        config = self.configure(profile)
        self.log = (self.runtime / "server-private.log").open("wb")
        self.process = subprocess.Popen(
            [str(self.binary)],
            cwd=self.runtime,
            env=self.environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

        def retain_console():
            tail = bytearray()
            cap = 2 * 1024 * 1024
            try:
                while True:
                    chunk = self.process.stdout.read(65536)
                    if not chunk:
                        break
                    self.console_bytes += len(chunk)
                    if self.console_retained_bytes < cap:
                        part = chunk[: cap - self.console_retained_bytes]
                        self.log.write(part)
                        self.console_retained_bytes += len(part)
                    tail.extend(chunk)
                    if len(tail) > 65536:
                        del tail[:-65536]
                if self.console_bytes > cap:
                    self.log.write(b"\n[S07 console truncated; tail follows]\n" + tail)
                    self.console_retained_bytes += len(tail)
            finally:
                self.log.flush()

        self.log_thread = threading.Thread(
            target=retain_console, daemon=True, name="s07-private-console"
        )
        self.log_thread.start()
        self.fixture["server_pid"] = self.process.pid
        self.fixture["server_executable"] = str(self.binary)
        self.fixture["server_start_ticks"] = (
            Path("/proc", str(self.process.pid), "stat")
            .read_text()
            .rsplit(")", 1)[1]
            .split()[19]
        )
        deadline = time.monotonic() + 30
        startup_pacer = pacer or Pacer(profile == "soak")
        session = Session(self.fixture["target"], 2, pace=startup_pacer)
        try:
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError("candidate exited during readiness")
                try:
                    response = session.request("readiness", "GET", "/health/ready")
                    value = json_body(response, "readiness")
                    if value.get("database") == value.get("redis") == "connected":
                        self.fixture["startup_pacing"] = (
                            {k: list(v) for k, v in startup_pacer.queues.items()}
                            if profile == "soak"
                            else {}
                        )
                        return config
                except ProtocolError:
                    pass
                time.sleep(0.2)
            raise RuntimeError("real DB/Redis readiness not reached")
        finally:
            session.close()

    def stop(self):
        if self.process:
            self.process.terminate()
            try:
                code = self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                code = self.process.wait(timeout=3)
            if self.log_thread:
                self.log_thread.join(timeout=5)
                if self.log_thread.is_alive():
                    raise RuntimeError("console collector did not finish")
            self.shutdown_events.append(
                {
                    "exit_code": code,
                    "time_ns": time.time_ns(),
                    "console_bytes": self.console_bytes,
                    "console_retained_bytes": self.console_retained_bytes,
                }
            )
            self.process = None
        if self.log:
            self.log.close()
            self.log = None


def create_organizations(fixture, save=lambda: None, guard=lambda: None):
    for account in fixture["accounts"]:
        if account.get("org_ready"):
            continue
        session = Session(fixture["target"])
        session.before_send = guard
        user = User(session, account, fixture)
        try:
            user.accept_pair(user.authorize_consent())
            user.subject = user.userinfo()["sub"]
            slug = "s07-" + account["username"]
            response = session.request(
                "org_setup",
                "POST",
                "/api/me/organizations",
                bearer=user.access,
                data={"slug": slug, "name": "S07 isolated org"},
                expected=(201, 409),
            )
            listed = session.request(
                "org_list_setup", "GET", "/api/me/organizations", bearer=user.access
            )
            organizations = json_body(listed, "org_list_setup").get("organizations", [])
            matches = [
                o
                for o in organizations
                if o.get("slug") == slug and o.get("role") == "owner"
            ]
            if len(matches) != 1 or matches[0].get("id") is None:
                raise ProtocolError("org_setup_shape", "org_setup", 201)
            org_id = matches[0]["id"]
            if not account.get("org_client"):
                response = session.request(
                    "app_setup",
                    "POST",
                    "/api/me/applications",
                    bearer=user.access,
                    data={
                        "name": "S07 app " + account["username"],
                        "client_type": "CONFIDENTIAL",
                        "redirect_uris": [fixture["public_client"]["redirect_uri"]],
                        "scopes": ["openid", "profile", "org"],
                        "allowed_grant_types": ["authorization_code", "refresh_token"],
                    },
                    expected=(201,),
                )
                app = json_body(response, "app_setup")
                app = app.get("data", app)
                if not app.get("client_id") or not app.get("client_secret"):
                    raise ProtocolError("app_setup_shape", "app_setup", 201)
                account.update(
                    org=slug,
                    org_id=org_id,
                    org_client={
                        "id": app["client_id"],
                        "secret": app["client_secret"],
                        "redirect_uri": fixture["public_client"]["redirect_uri"],
                        "scope": "openid profile org",
                    },
                )
                save()
            session.request(
                "app_transfer",
                "POST",
                "/api/me/applications/" + account["org_client"]["id"] + "/transfer",
                bearer=user.access,
                data={"org_slug": slug},
                expected=(200, 204),
            )
            account.update(org_ready=True)
            save()
        finally:
            session.close()


def verify_candidate(fixture, pacer=None):
    """Bind observations and credentials to the leased DB/Redis and owning listener."""
    db = Database(fixture)
    db.verify()
    stats = db.stats()
    if any(
        stats.get(k) != "on"
        for k in ("fsync", "synchronous_commit", "full_page_writes")
    ):
        raise ValueError("durable PostgreSQL settings are required")
    redis = Redis(fixture["redis_port"], fixture["redis_password"])
    try:
        if redis.command("GET", "fulla:s07:owner") != fixture["owner_nonce"]:
            raise ValueError("Redis ownership mismatch")
        values = redis.command("CONFIG", "GET", "save", "appendonly", "appendfsync")
        persistence = dict(zip(values[::2], values[1::2]))
        if not persistence.get("save") and persistence.get("appendonly") != "yes":
            raise ValueError("Redis persistence must not be silently disabled")
        stats["redis_persistence"] = persistence
    finally:
        redis.close()
    pid = int(fixture["server_pid"])
    proc = Path("/proc") / str(pid)
    ticks = (proc / "stat").read_text().rsplit(")", 1)[1].split()[19]
    if ticks != str(fixture["server_start_ticks"]):
        raise ValueError("candidate PID was reused")
    if (
        Path(os.readlink(proc / "cwd")).resolve()
        != Path(fixture["runtime_directory"]).resolve()
    ):
        raise ValueError("candidate runtime directory mismatch")
    runtime_config = Path(fixture["runtime_directory"]) / "config.json"
    if (
        hashlib.sha256(runtime_config.read_bytes()).hexdigest()
        != fixture["runtime_config_sha256"]
    ):
        raise ValueError("candidate runtime configuration changed")
    config = load_config(runtime_config)
    if bool(cache_config(config)["cache"]["enabled"]) != bool(fixture["cache_enabled"]):
        raise ValueError("effective cache profile mismatch")
    executable = Path(os.readlink(proc / "exe")).resolve()
    if executable != Path(fixture["server_executable"]).resolve():
        raise ValueError("candidate executable mismatch")
    if (
        hashlib.sha256(executable.read_bytes()).hexdigest()
        != fixture["build"]["binary_sha256"]
    ):
        raise ValueError("candidate executable hash changed")
    inodes = set()
    for p in (proc / "fd").iterdir():
        try:
            link = os.readlink(p)
            if link.startswith("socket:["):
                inodes.add(link[8:-1])
        except OSError:
            pass
    port = int(fixture["port"])
    owns = False
    for p in ("tcp", "tcp6"):
        for line in (proc / "net" / p).read_text().splitlines()[1:]:
            fields = line.split()
            if (
                len(fields) > 9
                and fields[9] in inodes
                and fields[3] == "0A"
                and int(fields[1].split(":")[1], 16) == port
            ):
                owns = True
    if not owns:
        raise ValueError("candidate PID does not own the target listener")
    session = Session(fixture["target"], pace=pacer)
    try:
        ready = json_body(
            session.request("readiness", "GET", "/health/ready"), "readiness"
        )
        if ready.get("database") != "connected" or ready.get("redis") != "connected":
            raise ValueError("both real backends must be connected")
    finally:
        session.close()
    return stats
