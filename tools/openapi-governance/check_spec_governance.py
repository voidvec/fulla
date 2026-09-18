#!/usr/bin/env python3
"""OpenAPI spec governance gate: 3-layer endpoint consistency + version sync.

Asserts three invariants over the HTTP API surface (spec-governance M0,
docs/productization-evolution/todo/openapi-spec-governance-plan.md):

  1. routes  == docs    -- every ADD_METHOD_TO route registers an OpenAPI doc
                           (and no doc names a route that does not exist),
                           modulo ROUTE_ONLY exclusions.
  2. docs    == yaml    -- apps/server/openapi.yaml mirrors the doc-registered
                           set, modulo YAML_EXCLUDED self-documentation paths.
  3. version -- openapi.yaml info.version == cmake/Version.cmake project
                           version.

Sources (no compilation needed; CI static-checks runs this):
  routes  : ADD_METHOD_TO macros in controller headers/sources. Multi-line
            macros are spanned; only the FIRST method token of a macro counts
            (GitHub/Google/WeChat declare `::drogon::Post, ::drogon::Options`
            -- the OPTIONS modifier is not a separate route for this gate).
  docs    : the frozen kFingerprint string literals in
            tests/integration/concurrency/Property4_OpenApiValidationBaselineTest.cc
            (the fingerprint test keeps that set honest against the binary).
  yaml    : apps/server/openapi.yaml paths.

Exit codes:
  0  all checks pass
  1  governance drift detected (diffs printed)
  2  environment/parse error (missing files, implausible extraction counts)

Usage:
  check_spec_governance.py [--repo ROOT] [--selftest]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple

HERE = Path(__file__).resolve().parent
REPO_DEFAULT = HERE.parent.parent

sys.path.insert(0, str(REPO_DEFAULT / "tools" / "refactor-baseline"))
import parse_endpoints  # type: ignore  # noqa: E402  (PyYAML or MiniYaml fallback)

FINGERPRINT_TEST = Path("tests/integration/concurrency/Property4_OpenApiValidationBaselineTest.cc")
OPENAPI_YAML = Path("apps/server/openapi.yaml")
VERSION_CMAKE = Path("cmake/Version.cmake")
CONTROLLER_GLOBS = [
    "libs/drogon/include/fulla/drogon/controllers/*.h",
    # .cc globs: today the route macros live in headers, but scanning the
    # implementation trees too closes the "new controller defines its macro in
    # the .cc" blind spot. Comment-only mentions of ADD_METHOD_TO don't match
    # the regexes (they lack the quoted-path + method-token shape).
    "libs/drogon/src/**/*.cc",
    "apps/server/**/*.h",
    "apps/server/**/*.cc",
]

# ---------------------------------------------------------------------------
# Exclusions (each entry needs a reason; the gate fails on anything else).
# ---------------------------------------------------------------------------

# Real routes that are deliberately NEVER doc-registered nor in the YAML.
ROUTE_ONLY: Set[str] = {
    # Server-rendered HTML login page (SessionController::showLoginPage), not an API.
    "GET /login",
    # Non-slash redirect variant of the Swagger UI entry point; /docs/api/ is
    # the documented one (ApiDocController).
    "GET /docs/api",
}

# Doc-registered operations deliberately ABSENT from the YAML: the API's
# self-documentation surface has no meaning for SDK consumers (design D1).
YAML_EXCLUDED: Set[str] = {
    "GET /docs/api/",
    "GET /docs/api/openapi.json",
}

# Extraction sanity floors: the real surface is ~80 ops. A parse regression
# (refactored test file, changed macro idiom) must fail loudly, not silently
# compare two empty sets.
MIN_ROUTES = 60
MIN_DOCS = 60
MIN_YAML_OPS = 60


# ---------------------------------------------------------------------------
# Extractors (pure functions over file paths -> sets/strings)
# ---------------------------------------------------------------------------

# First method token only; `::drogon::Post, ::drogon::Options` yields POST.
_ROUTE_RES = [
    re.compile(r'ADD_METHOD_TO\(\s*[^,]+,\s*"([^"]+)"\s*,\s*::drogon::(\w+)', re.S),
    re.compile(r'ADD_METHOD_TO\(\s*[^,]+,\s*"([^"]+)"\s*,\s*(Get|Post|Put|Delete|Patch)\b', re.S),
]

_FINGERPRINT_ENTRY_RE = re.compile(r'"([A-Z]+ /[^"\\]+)\\n"')


def extract_routes(files: List[Path]) -> Set[str]:
    ops: Set[str] = set()
    for f in files:
        try:
            src = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for rx in _ROUTE_RES:
            for m in rx.finditer(src):
                ops.add(f"{m.group(2).upper()} {m.group(1)}")
    return ops


def extract_docs(fingerprint_test: Path) -> Set[str]:
    """Parse the frozen kFingerprint string literals in the baseline test."""
    src = fingerprint_test.read_text(encoding="utf-8")
    # Anchor on the assignment (comments may mention "kFingerprint"/"enum
    # class" in prose before the real definition).
    m = re.search(r"kFingerprint\s*=", src)
    if not m:
        raise ValueError("kFingerprint assignment not found in fingerprint test")
    start = m.end()
    end = src.find("enum class", start)
    if end < 0:
        raise ValueError("terminating 'enum class' not found after kFingerprint")
    return {em.group(1) for em in _FINGERPRINT_ENTRY_RE.finditer(src[start:end])}


def extract_yaml_ops(openapi_yaml: Path) -> Set[str]:
    doc = parse_endpoints._load_yaml(openapi_yaml.read_text(encoding="utf-8"))
    if not isinstance(doc, dict) or not isinstance(doc.get("paths"), dict):
        raise ValueError("openapi.yaml has no paths mapping")
    ops: Set[str] = set()
    for path, item in doc["paths"].items():
        if not isinstance(item, dict):
            continue
        for method in ("get", "post", "put", "delete", "patch"):
            if isinstance(item.get(method), dict):
                ops.add(f"{method.upper()} {path}")
    return ops


def extract_yaml_version(openapi_yaml: Path) -> str:
    doc = parse_endpoints._load_yaml(openapi_yaml.read_text(encoding="utf-8"))
    info = (doc or {}).get("info") or {}
    version = info.get("version")
    if not isinstance(version, str) or not version:
        raise ValueError("openapi.yaml info.version missing")
    return version


def extract_cmake_version(version_cmake: Path) -> str:
    src = version_cmake.read_text(encoding="utf-8")

    def var(name: str) -> str:
        m = re.search(rf"set\(\s*{name}\s+(\d+)\s*\)", src)
        if not m:
            raise ValueError(f"{name} not found in {version_cmake}")
        return m.group(1)

    return ".".join(var("FULLA_PROJECT_VERSION_" + p) for p in ("MAJOR", "MINOR", "PATCH"))


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------


def diff_sets(expected: Set[str], actual: Set[str]) -> Tuple[List[str], List[str]]:
    return sorted(expected - actual), sorted(actual - expected)


def run_checks(
    routes: Set[str],
    docs: Set[str],
    yaml_ops: Set[str],
    yaml_version: str,
    cmake_version: str,
) -> List[str]:
    failures: List[str] = []

    docs_expected_from_routes = routes - ROUTE_ONLY
    missing_docs, ghost_docs = diff_sets(docs_expected_from_routes, docs)
    if missing_docs or ghost_docs:
        failures.append("[routes<->docs] route/doc registration mismatch")
        for op in missing_docs:
            failures.append(f"  route without doc registration: {op}")
        for op in ghost_docs:
            failures.append(f"  doc registration without backing route: {op}")

    yaml_expected = docs - YAML_EXCLUDED
    missing_yaml, extra_yaml = diff_sets(yaml_expected, yaml_ops)
    if missing_yaml or extra_yaml:
        failures.append("[docs<->yaml] openapi.yaml operation set mismatch")
        for op in missing_yaml:
            failures.append(f"  missing from openapi.yaml: {op}")
        for op in extra_yaml:
            failures.append(f"  in openapi.yaml but not registered in code: {op}")

    if yaml_version != cmake_version:
        failures.append(
            f"[version] openapi.yaml info.version={yaml_version} != "
            f"cmake/Version.cmake FULLA_PROJECT_VERSION={cmake_version}"
        )
    return failures


def check_write_ops_have_request_body(openapi_yaml: Path) -> List[str]:
    """Structural rule (v1.4.0 review C6): every /api/ write operation
    (post/put/patch) in openapi.yaml must declare a requestBody — an
    operation without one generates SDKs whose calls are rejected
    server-side ("JSON body required" or equivalent). Action-style
    operations declare an optional empty-object body.
    """
    doc = parse_endpoints._load_yaml(openapi_yaml.read_text(encoding="utf-8"))
    failures: List[str] = []
    for path, item in (doc.get("paths") or {}).items():
        if not isinstance(path, str) or not path.startswith("/api/"):
            continue
        if not isinstance(item, dict):
            continue
        for method in ("post", "put", "patch"):
            op = item.get(method)
            if isinstance(op, dict) and "requestBody" not in op:
                failures.append(
                    f"  {method.upper()} {path} has no requestBody "
                    f"(SDK callers cannot send a body)"
                )
    if failures:
        failures.insert(0, "[structure] /api/ write operations without requestBody")
    return failures


# ---------------------------------------------------------------------------
# Self-test (fixtures under tools/openapi-governance/fixtures/)
# ---------------------------------------------------------------------------

FIXTURES = HERE / "fixtures"


def _selftest() -> int:
    routes = extract_routes([FIXTURES / "mini_controller.h"])
    docs = extract_docs(FIXTURES / "mini_fingerprint_test.cc")
    yaml_ops = extract_yaml_ops(FIXTURES / "mini_openapi.yaml")
    yaml_ver = extract_yaml_version(FIXTURES / "mini_openapi.yaml")
    cmake_ver = extract_cmake_version(FIXTURES / "mini_version.cmake")

    # Fixture layout (4 documented ops + 1 ROUTE_ONLY + 1 YAML_EXCLUDED):
    #   routes: GET /x, POST /x, PUT /y (bare method name), GET /login, GET /docs/api/
    #   docs:   GET /x, POST /x, PUT /y, GET /docs/api/   (login is route-only)
    #   yaml:   GET /x, POST /x, PUT /y                    (docs/api excluded)
    assert "GET /x" in routes and "POST /x" in routes, routes
    assert "PUT /y" in routes, f"bare-name method macro not parsed: {routes}"
    assert docs == {"GET /x", "POST /x", "PUT /y", "GET /docs/api/"}, docs
    assert yaml_ops == {"GET /x", "POST /x", "PUT /y"}, yaml_ops

    # Happy path: multi-line macros, first-token methods, exclusions, versions.
    ok = run_checks(routes, docs, yaml_ops, "1.2.0", "1.2.0")
    if ok:
        print("[spec-governance][err] selftest: clean fixture reported failures", file=sys.stderr)
        for ln in ok:
            print("   ", ln, file=sys.stderr)
        return 1

    # Fault 1: YAML drops an op -> drift reported, named.
    bad1 = run_checks(routes, docs, yaml_ops - {"POST /x"}, "1.2.0", "1.2.0")
    if not any("missing from openapi.yaml: POST /x" in ln for ln in bad1):
        print(f"[spec-governance][err] selftest: yaml-drop fault not caught: {bad1!r}", file=sys.stderr)
        return 1

    # Fault 2: a new undocumented route appears -> drift reported.
    bad2 = run_checks(routes | {"DELETE /y"}, docs, yaml_ops | {"DELETE /y"}, "1.2.0", "1.2.0")
    if not any("route without doc registration: DELETE /y" in ln for ln in bad2):
        print(f"[spec-governance][err] selftest: undocumented-route fault not caught: {bad2!r}", file=sys.stderr)
        return 1

    # Fault 3: version drift -> reported.
    bad3 = run_checks(routes, docs, yaml_ops, "1.2.0", "1.3.0")
    if not any("[version]" in ln for ln in bad3):
        print(f"[spec-governance][err] selftest: version drift not caught: {bad3!r}", file=sys.stderr)
        return 1

    # Fault 4: ghost doc (doc names a route that does not exist).
    bad4 = run_checks(routes, docs | {"PUT /ghost"}, yaml_ops | {"PUT /ghost"}, "1.2.0", "1.2.0")
    if not any("doc registration without backing route: PUT /ghost" in ln for ln in bad4):
        print(f"[spec-governance][err] selftest: ghost-doc fault not caught: {bad4!r}", file=sys.stderr)
        return 1

    print(
        "[spec-governance] selftest OK "
        "(multi-line macros; exclusions; yaml-drop / new-route / ghost-doc / version faults)"
    )
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _main() -> int:
    p = argparse.ArgumentParser(description="OpenAPI spec governance gate")
    p.add_argument("--repo", default=str(REPO_DEFAULT))
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args()

    if args.selftest:
        return _selftest()

    repo = Path(args.repo)

    fingerprint_test = repo / FINGERPRINT_TEST
    openapi_yaml = repo / OPENAPI_YAML
    version_cmake = repo / VERSION_CMAKE
    for f in (fingerprint_test, openapi_yaml, version_cmake):
        if not f.is_file():
            print(f"[spec-governance][err] required file missing: {f}", file=sys.stderr)
            return 2

    route_files: List[Path] = []
    for pattern in CONTROLLER_GLOBS:
        route_files.extend(repo.glob(pattern))
    if not route_files:
        print("[spec-governance][err] no controller sources matched", file=sys.stderr)
        return 2

    try:
        routes = extract_routes(route_files)
        docs = extract_docs(fingerprint_test)
        yaml_ops = extract_yaml_ops(openapi_yaml)
        yaml_version = extract_yaml_version(openapi_yaml)
        cmake_version = extract_cmake_version(version_cmake)
    except (OSError, ValueError) as e:
        print(f"[spec-governance][err] extraction failed: {e}", file=sys.stderr)
        return 2

    # Sanity floors: implausible counts mean a parse regression, not agreement.
    if len(routes) < MIN_ROUTES or len(docs) < MIN_DOCS or len(yaml_ops) < MIN_YAML_OPS:
        print(
            "[spec-governance][err] extraction produced implausibly small sets "
            f"(routes={len(routes)}, docs={len(docs)}, yaml={len(yaml_ops)}; "
            f"floors {MIN_ROUTES}/{MIN_DOCS}/{MIN_YAML_OPS}) -- parse regression?",
            file=sys.stderr,
        )
        return 2

    failures = run_checks(routes, docs, yaml_ops, yaml_version, cmake_version)
    failures.extend(check_write_ops_have_request_body(openapi_yaml))
    if failures:
        print("[spec-governance] FAIL: OpenAPI governance drift detected")
        for ln in failures:
            print(ln)
        print(
            "Fix by updating apps/server/openapi.yaml AND the doc registrations "
            "(OpenApiGenerator::addEndpoint) AND the fingerprint test baseline "
            "together -- see .claude/skills/openapi-update and "
            "docs/productization-evolution/todo/openapi-spec-governance-plan.md."
        )
        return 1

    print(
        f"[spec-governance] OK: routes={len(routes)} docs={len(docs)} yaml={len(yaml_ops)} "
        f"(route_only={len(ROUTE_ONLY)}, yaml_excluded={len(YAML_EXCLUDED)}), "
        f"info.version={yaml_version} == cmake {cmake_version}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(_main())
