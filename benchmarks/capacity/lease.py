#!/usr/bin/env python3
"""Generate private S07 fixture credentials and CREATE-only SQL; never provision/delete."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import subprocess
from pathlib import Path


def create(args):
    repo = Path(__file__).resolve().parents[2]
    if not re.fullmatch(r"[0-9a-f]{40}", args.product_commit):
        raise ValueError("record the exact product commit")
    # Tool/docs-only commits may follow the product build, but product inputs
    # must still match that commit. No global safe.directory mutation.
    subprocess.run(
        [
            "git",
            "-c",
            "safe.directory=" + str(repo),
            "diff",
            "--exit-code",
            args.product_commit,
            "--",
            "libs",
            "apps/server",
            "cmake",
            "CMakeLists.txt",
            "conanfile.py",
            "conan.lock",
        ],
        cwd=repo,
        check=True,
        capture_output=True,
        timeout=15,
    )
    cache = {}
    for line in args.build_cache.read_text().splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    if (
        cache.get("CMAKE_BUILD_TYPE") != "Release"
        or "-DNDEBUG" not in cache.get("CMAKE_CXX_FLAGS_RELEASE", "")
        or cache.get("FULLA_WERROR") != "ON"
    ):
        raise ValueError("use the verified Release/NDEBUG/WERROR build cache")
    flags = {
        k: v
        for k, v in cache.items()
        if "FLAGS" in k or k in ("FULLA_SANITIZER", "FULLA_TEST_COVERAGE")
    }
    if cache.get("FULLA_SANITIZER", "none").lower() not in (
        "",
        "none",
        "off",
    ) or cache.get("FULLA_TEST_COVERAGE", "OFF").upper() not in (
        "",
        "OFF",
        "FALSE",
        "0",
    ):
        raise ValueError(
            "diagnostic sanitizer/coverage builds cannot produce capacity evidence"
        )
    if re.search(
        r"-fsanitize|/fsanitize|--coverage|-fprofile|-ftest-coverage",
        " ".join(flags.values()),
    ):
        raise ValueError("instrumented compiler/linker flags are not a capacity build")
    ports = (args.db_port, args.redis_port, args.app_port, args.db_proxy_port)
    if len(set(ports)) != len(ports) or not all(1024 < p < 65536 for p in ports):
        raise ValueError("choose four distinct unprivileged loopback ports")
    if not re.fullmatch(r"fulla-s07-[a-z0-9-]+", args.redis_container):
        raise ValueError("Redis container must use the dedicated fulla-s07- prefix")
    if args.output.exists() or args.sql_output.exists():
        raise FileExistsError("refuse to replace an existing fixture or SQL file")
    nonce = secrets.token_hex(16)
    name = "fulla_s07_" + nonce[:10]
    value = {
        "owner": "fulla-capacity-s07",
        "owner_nonce": nonce,
        "port": args.app_port,
        "target": "http://127.0.0.1:" + str(args.app_port),
        "db_proxy_port": args.db_proxy_port,
        "db": {
            "host": "127.0.0.1",
            "port": args.db_port,
            "database": name,
            "user": name,
            "password": secrets.token_urlsafe(24),
        },
        "redis_port": args.redis_port,
        "redis_password": secrets.token_urlsafe(24),
        "public_client": {
            "id": "fulla-portal",
            "redirect_uri": "http://127.0.0.1:5173/callback",
            "scope": "openid profile",
        },
        "confidential_client": {
            "id": "backend-svc",
            "secret": "test-secret",
            "scope": "tokens:read",
        },
        "accounts": [
            {
                "username": "s07u" + nonce[:8] + str(i),
                "password": "S07-" + secrets.token_urlsafe(18) + "!",
            }
            for i in range(4)
        ],
        "build": {
            "git_commit": args.product_commit,
            "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            "cmake_release_flags": cache["CMAKE_CXX_FLAGS_RELEASE"],
            "cmake_flags_and_instrumentation": flags,
            "compiler_path": cache.get("CMAKE_CXX_COMPILER"),
            "lto_cache_values": {
                k: v
                for k, v in cache.items()
                if "LTO" in k or k == "CMAKE_INTERPROCEDURAL_OPTIMIZATION"
            },
        },
        "infrastructure": {
            "redis_container": args.redis_container,
            "redis_volume": "fulla-s07-redis-" + nonce[:10],
        },
        "profile": "default",
        "cache_enabled": False,
    }
    sql = (
        'CREATE ROLE "'
        + name
        + '" LOGIN PASSWORD '
        + "'"
        + value["db"]["password"]
        + "';\nCREATE DATABASE \""
        + name
        + '" OWNER "'
        + name
        + '";\n'
    )
    for path, content in (
        (args.output, json.dumps(value, indent=2) + "\n"),
        (args.sql_output, sql),
    ):
        path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as stream:
            stream.write(content)
    return {
        "status": "generated_only",
        "accounts": 4,
        "files_private": True,
        "external_resources_modified": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--build-cache", type=Path, required=True)
    parser.add_argument("--product-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sql-output", type=Path, required=True)
    parser.add_argument("--db-port", type=int, default=25433)
    parser.add_argument("--redis-port", type=int, default=16380)
    parser.add_argument("--app-port", type=int, default=15555)
    parser.add_argument("--db-proxy-port", type=int, default=25432)
    parser.add_argument("--redis-container", default="fulla-s07-capacity-redis")
    print(json.dumps(create(parser.parse_args())))


if __name__ == "__main__":
    main()
