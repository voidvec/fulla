#!/usr/bin/env python3
"""Client SDK regeneration + drift gate for clients/python, clients/go and clients/ts.

Regenerates the committed generated code under clients/ from the single
OpenAPI source of truth (apps/server/openapi.yaml) and, with --check,
compares the fresh output against what is committed (design:
docs/productization-evolution/in-progress/client-sdk-facility-design.md
sections D9 + 11.5; TS client added by P1-5 #282).

Generators are pinned (upgrades go through a dedicated PR so the resulting
whole-tree diff stays reviewable):

  openapi-python-client 0.29.0   -> clients/python/src/fulla/generated/
                                    (package-internal relative imports make the
                                    generated package relocatable; the generator
                                    also writes project-level files next to the
                                    package which are deliberately NOT copied)
  oapi-codegen v2.8.0            -> clients/go/generated/client.gen.go
  @hey-api/openapi-ts 0.99.0     -> clients/ts/src/generated/
                                    (invoked via the local node_modules install
                                    in clients/ts -- `npm install` there first;
                                    emitted import extensions follow
                                    clients/ts/tsconfig.json, so that file is
                                    part of the determinism contract)

Also asserts the version linkage clients/python/pyproject.toml ==
clients/ts/package.json == cmake/Version.cmake so a forgotten bump can
never publish a wrong PyPI/npm version (release.yml's sdk-python job runs
--version-only as a backstop).

Exit codes:
  0  regeneration done / no drift / versions in sync
  1  drift detected, or version linkage mismatch
  2  environment error (missing files/tools, failed generation)

Usage:
  regen_clients.py [--repo ROOT] [--check] [--python-only] [--go-only]
                   [--ts-only] [--version-only] [--selftest]

Local network note: the Go generator downloads modules on first use; on
networks where proxy.golang.org is unreachable set GOPROXY to a mirror
(e.g. https://goproxy.cn,direct). CI runners are unaffected. The TS
generator needs no network once `npm install` has run in clients/ts.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Optional, Tuple

HERE = Path(__file__).resolve().parent
REPO_DEFAULT = HERE.parent.parent

TAG = "[clients-regen]"

OPENAPI_YAML = Path("apps/server/openapi.yaml")
VERSION_CMAKE = Path("cmake/Version.cmake")
PY_CLIENT_DIR = Path("clients/python")
PY_GEN_CONFIG = PY_CLIENT_DIR / "openapi-python-client.yaml"
PY_GENERATED = PY_CLIENT_DIR / "src" / "fulla" / "generated"
PY_PROJECT = PY_CLIENT_DIR / "pyproject.toml"
PY_PACKAGE_NAME = "fulla"  # must match PY_GEN_CONFIG package_name_override
GO_CLIENT_DIR = Path("clients/go")
GO_GENERATED = GO_CLIENT_DIR / "generated"
GO_PACKAGE = "generated"
TS_CLIENT_DIR = Path("clients/ts")
TS_GENERATED = TS_CLIENT_DIR / "src" / "generated"
TS_PACKAGE = TS_CLIENT_DIR / "package.json"

OPENAPI_PYTHON_CLIENT_VERSION = "0.29.0"
OAPI_CODEGEN_MODULE = "github.com/oapi-codegen/oapi-codegen/v2/cmd/oapi-codegen"
OAPI_CODEGEN_VERSION = "v2.8.0"
HEY_API_VERSION = "0.99.0"

# Compared content is newline-normalized: the generators write LF, while a
# Windows checkout with core.autocrlf=true materializes CRLF. That conversion
# is a client-side artifact, not drift.
IGNORED_SUFFIXES = (".pyc",)
IGNORED_DIR_NAMES = {"__pycache__", ".ruff_cache", ".pytest_cache"}


def _die_env(msg: str) -> int:
    print(f"{TAG}[err] {msg}", file=sys.stderr)
    return 2


def _cmake_version(repo: Path) -> Optional[Tuple[int, int, int]]:
    text = (repo / VERSION_CMAKE).read_text(encoding="utf-8")
    parts = []
    for component in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"FULLA_PROJECT_VERSION_{component}\s+(\d+)", text)
        if not m:
            return None
        parts.append(int(m.group(1)))
    return (parts[0], parts[1], parts[2])


def _pyproject_version(repo: Path) -> Optional[str]:
    # Regex instead of tomllib: CI's ubuntu-22.04 python3 is 3.10 and this
    # tool must run there without new dependencies (same rationale as the
    # PyYAML-optional governance gate).
    text = (repo / PY_PROJECT).read_text(encoding="utf-8")
    m = re.search(r'^version\s*=\s*"([^"]+)"', text, re.MULTILINE)
    return m.group(1) if m else None


def _ts_package_version(repo: Path) -> Optional[str]:
    # First "version" key wins; clients/ts/package.json keeps it near the
    # top, above devDependencies, so an accidental second occurrence there
    # cannot shadow it.
    text = (repo / TS_PACKAGE).read_text(encoding="utf-8")
    m = re.search(r'"version"\s*:\s*"([^"]+)"', text)
    return m.group(1) if m else None


def check_version_sync(repo: Path) -> bool:
    cmake = _cmake_version(repo)
    pyproject = _pyproject_version(repo)
    ts_package = _ts_package_version(repo)
    if cmake is None or pyproject is None or ts_package is None:
        print(
            f"{TAG}[err] could not parse versions (cmake={cmake} pyproject={pyproject!r} ts={ts_package!r})"
        )
        return False
    expected = ".".join(str(c) for c in cmake)
    ok = True
    if pyproject != expected:
        print(f"{TAG}[drift] version mismatch: pyproject={pyproject} != cmake/Version.cmake={expected}")
        print(f"{TAG}        bump clients/python/pyproject.toml together with cmake/Version.cmake")
        ok = False
    if ts_package != expected:
        print(f"{TAG}[drift] version mismatch: clients/ts/package.json={ts_package} != cmake/Version.cmake={expected}")
        print(f"{TAG}        bump clients/ts/package.json together with cmake/Version.cmake")
        ok = False
    if ok:
        print(f"{TAG}[ok] version sync: pyproject == ts package.json == cmake == {expected}")
    return ok


def _collect_files(root: Path) -> List[Path]:
    out: List[Path] = []
    for path in sorted(root.rglob("*")):
        if path.is_dir():
            continue
        if path.suffix in IGNORED_SUFFIXES:
            continue
        if any(part in IGNORED_DIR_NAMES for part in path.parts):
            continue
        out.append(path)
    return out


def _normalized(path: Path) -> bytes:
    return path.read_bytes().replace(b"\r\n", b"\n")


def _compare_trees(fresh: Path, committed: Path, label: str) -> List[str]:
    """Returns a list of human-readable drift descriptions (empty == clean)."""
    drift: List[str] = []
    fresh_files = {p.relative_to(fresh): p for p in _collect_files(fresh)}
    committed_files = {p.relative_to(committed): p for p in _collect_files(committed)}
    for rel in sorted(set(fresh_files) - set(committed_files)):
        drift.append(f"{label}: missing from committed tree: {rel}")
    for rel in sorted(set(committed_files) - set(fresh_files)):
        drift.append(f"{label}: stale in committed tree (not generated anymore): {rel}")
    for rel in sorted(set(fresh_files) & set(committed_files)):
        if _normalized(fresh_files[rel]) != _normalized(committed_files[rel]):
            drift.append(f"{label}: content drift: {rel}")
    return drift


def _check_python_generator_version() -> Optional[str]:
    try:
        from importlib.metadata import version as _pkg_version
    except ImportError:  # pragma: no cover - py<3.8
        return None
    try:
        installed = _pkg_version("openapi-python-client")
    except Exception:
        return "openapi-python-client is not installed in the current environment"
    if installed != OPENAPI_PYTHON_CLIENT_VERSION:
        return (
            f"openapi-python-client {installed} != pinned {OPENAPI_PYTHON_CLIENT_VERSION}; "
            f"install with: pip install openapi-python-client=={OPENAPI_PYTHON_CLIENT_VERSION}"
        )
    return None


def regenerate_python(repo: Path, check: bool) -> int:
    version_error = _check_python_generator_version()
    if version_error:
        return _die_env(version_error)
    for required in (repo / OPENAPI_YAML, repo / PY_GEN_CONFIG):
        if not required.exists():
            return _die_env(f"missing {required}")

    with tempfile.TemporaryDirectory(prefix="af-regen-py-") as tmp:
        # Argument-vector invocation only (no shell); paths come from repo
        # constants, never from user input. --overwrite because the tempdir
        # pre-exists (openapi-python-client refuses existing dirs otherwise).
        proc = subprocess.run(
            [
                sys.executable,
                "-m",
                "openapi_python_client",
                "generate",
                "--path",
                str(OPENAPI_YAML),
                "--output-path",
                tmp,
                "--config",
                str(PY_GEN_CONFIG),
                "--fail-on-warning",
                "--overwrite",
            ],
            cwd=str(repo),
            capture_output=True,
            text=True,
            shell=False,
        )
        if proc.returncode != 0:
            print(proc.stdout)
            print(proc.stderr, file=sys.stderr)
            return _die_env("openapi-python-client generate failed")
        fresh_pkg = Path(tmp) / PY_PACKAGE_NAME
        if not fresh_pkg.is_dir():
            return _die_env(f"generator did not produce a '{PY_PACKAGE_NAME}/' package dir")

        if check:
            drift = _compare_trees(fresh_pkg, repo / PY_GENERATED, "python")
            if drift:
                for line in drift[:20]:
                    print(f"{TAG}[drift] {line}")
                if len(drift) > 20:
                    print(f"{TAG}[drift] ... and {len(drift) - 20} more")
                print(f"{TAG} regenerate with: python tools/clients/regen_clients.py")
                return 1
            count = len(_collect_files(repo / PY_GENERATED))
            print(f"{TAG}[ok] python: committed generated tree matches spec ({count} files)")
            return 0

        target = repo / PY_GENERATED
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(fresh_pkg, target)
        print(f"{TAG}[ok] python: regenerated {len(_collect_files(target))} files into {PY_GENERATED}")
    return 0


def regenerate_go(repo: Path, check: bool) -> int:
    if not (repo / OPENAPI_YAML).exists():
        return _die_env(f"missing {repo / OPENAPI_YAML}")
    with tempfile.TemporaryDirectory(prefix="af-regen-go-") as tmp:
        out_file = Path(tmp) / "client.gen.go"
        # Argument-vector invocation only (no shell); the module@version
        # literal is composed solely from pinned constants.
        proc = subprocess.run(
            [
                "go",
                "run",
                OAPI_CODEGEN_MODULE + "@" + OAPI_CODEGEN_VERSION,
                "-generate",
                "types,client",
                "-package",
                GO_PACKAGE,
                "-o",
                str(out_file),
                str(OPENAPI_YAML),
            ],
            cwd=str(repo),
            capture_output=True,
            text=True,
            shell=False,
        )
        if proc.returncode != 0:
            print(proc.stdout)
            print(proc.stderr, file=sys.stderr)
            return _die_env("oapi-codegen generate failed")
        fresh_dir = Path(tmp)
        target = repo / GO_GENERATED
        if check:
            drift = _compare_trees(fresh_dir, target, "go")
            if drift:
                for line in drift:
                    print(f"{TAG}[drift] {line}")
                print(f"{TAG} regenerate with: python tools/clients/regen_clients.py")
                return 1
            print(f"{TAG}[ok] go: committed generated tree matches spec")
            return 0

        target.mkdir(parents=True, exist_ok=True)
        for stale in target.iterdir():
            if stale.is_file():
                stale.unlink()
        shutil.copy2(out_file, target / "client.gen.go")
        print(f"{TAG}[ok] go: regenerated {GO_GENERATED / 'client.gen.go'}")
    return 0


def _check_ts_generator(repo: Path) -> Optional[str]:
    """Returns an error string when the pinned local TS generator is unusable."""
    node = shutil.which("node")
    if node is None:
        return "node (>= 18.17) is required for the TS generator and not on PATH"
    entry = repo / TS_CLIENT_DIR / "node_modules" / "@hey-api" / "openapi-ts" / "dist" / "run.mjs"
    if not entry.is_file():
        return "clients/ts/node_modules is missing the pinned generator; run `npm install` in clients/ts"
    manifest = repo / TS_CLIENT_DIR / "node_modules" / "@hey-api" / "openapi-ts" / "package.json"
    try:
        installed = json.loads(manifest.read_text(encoding="utf-8")).get("version")
    except (OSError, ValueError):
        installed = None
    if installed != HEY_API_VERSION:
        return (
            f"@hey-api/openapi-ts {installed} != pinned {HEY_API_VERSION}; "
            f"run `npm install` in clients/ts"
        )
    return None


def regenerate_ts(repo: Path, check: bool) -> int:
    version_error = _check_ts_generator(repo)
    if version_error:
        return _die_env(version_error)
    if not (repo / OPENAPI_YAML).exists():
        return _die_env(f"missing {repo / OPENAPI_YAML}")
    node = shutil.which("node")
    entry = repo / TS_CLIENT_DIR / "node_modules" / "@hey-api" / "openapi-ts" / "dist" / "run.mjs"
    with tempfile.TemporaryDirectory(prefix="af-regen-ts-") as tmp:
        out_dir = Path(tmp) / "gen"
        # Argument-vector invocation only (no shell); every path comes from
        # repo constants. Emitted import extensions follow clients/ts/
        # tsconfig.json (discovered from the generator's install location),
        # so the committed tsconfig is part of the drift contract.
        proc = subprocess.run(
            [node, str(entry), "-i", str(OPENAPI_YAML), "-o", str(out_dir)],
            cwd=str(repo),
            capture_output=True,
            text=True,
            shell=False,
        )
        if proc.returncode != 0 or not out_dir.is_dir():
            print(proc.stdout)
            print(proc.stderr, file=sys.stderr)
            return _die_env("@hey-api/openapi-ts generate failed")

        if check:
            drift = _compare_trees(out_dir, repo / TS_GENERATED, "ts")
            if drift:
                for line in drift[:20]:
                    print(f"{TAG}[drift] {line}")
                if len(drift) > 20:
                    print(f"{TAG}[drift] ... and {len(drift) - 20} more")
                print(f"{TAG} regenerate with: cd clients/ts && npm run generate (or tools/clients/regen_clients.py)")
                return 1
            count = len(_collect_files(repo / TS_GENERATED))
            print(f"{TAG}[ok] ts: committed generated tree matches spec ({count} files)")
            return 0

        target = repo / TS_GENERATED
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(out_dir, target)
        print(f"{TAG}[ok] ts: regenerated {len(_collect_files(target))} files into {TS_GENERATED}")
    return 0


# ---------------------------------------------------------------------------
# Selftest (fixture-driven, no network, no generators)
# ---------------------------------------------------------------------------

def _selftest() -> int:
    ok = True

    # 1) tree comparison catches modified / missing / stale files
    with tempfile.TemporaryDirectory(prefix="af-selftest-") as tmp:
        root = Path(tmp)
        fresh = root / "fresh"
        committed = root / "committed"
        for sub in (fresh, committed):
            (sub / "api").mkdir(parents=True)
            (sub / "api" / "a.py").write_bytes(b"x = 1\n")
            (sub / "b.py").write_bytes(b"y = 2\n")
        # normalize check: CRLF in committed == LF in fresh
        (committed / "b.py").write_bytes(b"y = 2\r\n")
        if _compare_trees(fresh, committed, "t"):
            print("[clients-regen][err] selftest: CRLF normalization not applied")
            ok = False
        # content drift
        (committed / "api" / "a.py").write_bytes(b"x = 3\n")
        d = _compare_trees(fresh, committed, "t")
        if len(d) != 1 or "content drift" not in d[0]:
            print(f"[clients-regen][err] selftest: content drift not caught: {d}")
            ok = False
        # missing + stale
        (committed / "api" / "a.py").unlink()
        (committed / "zz.py").write_bytes(b"z\n")
        d = _compare_trees(fresh, committed, "t")
        kinds = sorted(x.split(":")[1].strip().split()[0] for x in d)
        if kinds != ["missing", "stale"]:
            print(f"[clients-regen][err] selftest: missing/stale not caught: {d}")
            ok = False

    # 2) version parsing on the real repo files (parse-only, no mutation)
    repo = REPO_DEFAULT
    cmake = _cmake_version(repo)
    if not cmake or not all(isinstance(c, int) for c in cmake):
        print(f"[clients-regen][err] selftest: cmake version parse failed: {cmake}")
        ok = False
    if _pyproject_version(repo) is None:
        print("[clients-regen][err] selftest: pyproject version parse failed")
        ok = False
    if _ts_package_version(repo) is None:
        print("[clients-regen][err] selftest: ts package.json version parse failed")
        ok = False

    # 3) version-sync gate faults on mismatch (fixture pyproject + package.json)
    with tempfile.TemporaryDirectory(prefix="af-selftest-ver-") as tmp:
        root = Path(tmp)
        (root / "cmake").mkdir()
        (root / "cmake" / "Version.cmake").write_text(
            "set(FULLA_PROJECT_VERSION_MAJOR 1)\n"
            "set(FULLA_PROJECT_VERSION_MINOR 2)\n"
            "set(FULLA_PROJECT_VERSION_PATCH 0)\n"
        )
        client_py = root / "clients" / "python"
        client_py.mkdir(parents=True)
        (client_py / "pyproject.toml").write_text('[project]\nversion = "0.0.1"\n')
        client_ts = root / "clients" / "ts"
        client_ts.mkdir(parents=True)
        (client_ts / "package.json").write_text('{"name":"fulla-oauth2","version":"0.0.1"}\n')
        if check_version_sync(root):
            print("[clients-regen][err] selftest: version mismatch not caught")
            ok = False
        # Fixing only pyproject still fails on the stale ts package.json.
        (client_py / "pyproject.toml").write_text('[project]\nversion = "1.2.0"\n')
        if check_version_sync(root):
            print("[clients-regen][err] selftest: ts-only version mismatch not caught")
            ok = False
        (client_ts / "package.json").write_text('{"name":"fulla-oauth2","version":"1.2.0"}\n')
        if not check_version_sync(root):
            print("[clients-regen][err] selftest: version sync false negative")
            ok = False

    # 4) ignored dir/suffix filtering
    with tempfile.TemporaryDirectory(prefix="af-selftest-ign-") as tmp:
        sub = Path(tmp)
        (sub / "__pycache__" / "keep").mkdir(parents=True)
        (sub / "__pycache__" / "x.pyc").write_bytes(b"junk")
        (sub / ".ruff_cache").mkdir()
        (sub / ".ruff_cache" / "cache").write_bytes(b"junk")
        (sub / "real.py").write_bytes(b"r")
        names = [p.name for p in _collect_files(sub)]
        if names != ["real.py"]:
            print(f"[clients-regen][err] selftest: ignore filter broken: {names}")
            ok = False

    if ok:
        print(f"{TAG} selftest OK (tree-diff, version parse/sync, ignore filters)")
        return 0
    return 2


def _main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=REPO_DEFAULT)
    parser.add_argument("--check", action="store_true", help="compare fresh generation against the committed tree; exit 1 on drift")
    parser.add_argument("--python-only", action="store_true")
    parser.add_argument("--go-only", action="store_true")
    parser.add_argument("--ts-only", action="store_true")
    parser.add_argument("--version-only", action="store_true", help="only run the pyproject/package.json <-> cmake version linkage check")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        return _selftest()

    repo = args.repo.resolve()
    only_flags = [flag for flag in (args.python_only, args.go_only, args.ts_only) if flag]
    if len(only_flags) > 1:
        return _die_env("--python-only, --go-only and --ts-only are mutually exclusive")

    if not check_version_sync(repo):
        return 1
    if args.version_only:
        return 0

    results = []
    if not args.go_only and not args.ts_only:
        results.append(("python", regenerate_python(repo, args.check)))
    if not args.python_only and not args.ts_only:
        results.append(("go", regenerate_go(repo, args.check)))
    if not args.python_only and not args.go_only:
        results.append(("ts", regenerate_ts(repo, args.check)))
    for name, code in results:
        if code == 2:
            return 2
    return 1 if any(code == 1 for _, code in results) else 0


if __name__ == "__main__":
    sys.exit(_main())
