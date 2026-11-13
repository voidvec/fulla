#!/usr/bin/env python3
"""Verify the resolved optional-feature graph, compilation and SDK exports."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex

if not __debug__:
    raise RuntimeError("feature-matrix checks require Python without -O/PYTHONOPTIMIZE")


def enabled(value: str) -> bool:
    if value.upper() in {"ON", "TRUE", "1"}:
        return True
    if value.upper() in {"OFF", "FALSE", "0"}:
        return False
    raise argparse.ArgumentTypeError("expected ON/OFF")


def cache_values(build: Path) -> dict[str, str]:
    result = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            result[key.split(":", 1)[0]] = value
    return result


def check_consumer(build: Path, expected_root: Path, webauthn: bool, *, installed: bool) -> dict:
    cache = cache_values(build)
    if not webauthn:
        assert enabled(cache["CMAKE_DISABLE_FIND_PACKAGE_libcbor"])
    reports = list(build.glob("fulla-sdk-dependencies-*.txt"))
    assert reports, "consumer did not report its resolved SDK dependencies"
    result = {}
    for report in reports:
        for line in report.read_text().splitlines():
            key, value = line.split("=", 1)
            path = Path(value).resolve()
            assert path.is_relative_to(expected_root.resolve()), (key, path, expected_root)
            assert path.exists(), (key, path)
            if key.endswith("_DIR"):
                package = key.removesuffix("_DIR")
                if installed:
                    candidates = {expected_root / lib / "cmake" / package for lib in ("lib", "lib64")}
                    assert path in {p.resolve() for p in candidates}, (key, path)
                else:
                    assert path == (expected_root / "fulla-cmake" / package).resolve(), (key, path)
            else:
                library_root = expected_root if installed else expected_root / "libs"
                assert path.is_relative_to(library_root.resolve()), (key, path)
            result[key] = str(path)
    assert len(result) == 14, result
    return result


def check_build(repo: Path, build: Path, graph: Path, social: bool, webauthn: bool,
                allowed_platform_tools: set[str] | None = None) -> dict:
    nodes = list(json.loads(graph.read_text())["graph"]["nodes"].values())
    host_refs = {n["ref"] for n in nodes if n.get("context") == "host" and n.get("recipe") != "Consumer"}
    lock = json.loads((repo / "conan.lock").read_text())
    locked = {r.split("%", 1)[0] for key in ("requires", "build_requires", "python_requires") for r in lock.get(key, [])}
    graph_refs = {n["ref"] for n in nodes if n.get("ref") and n.get("recipe") != "Consumer"}
    allowed = allowed_platform_tools or set()
    exceptions = graph_refs - locked
    assert exceptions <= allowed, sorted(exceptions - allowed)
    assert all(n.get("recipe") == "Platform" for n in nodes if n.get("ref") in exceptions), exceptions
    assert any(r.startswith("libcbor/") for r in graph_refs) == webauthn
    entries = json.loads((build / "compile_commands.json").read_text())
    sources = {Path(e["file"]).resolve() for e in entries}
    web_sources = ["libs/identity/src/webauthn/WebAuthnService.cc", "libs/identity/src/webauthn/WebAuthnCrypto.cc",
                   "libs/identity/test/WebAuthnServiceTest.cc", "libs/identity/test/WebAuthnCryptoTest.cc",
                   "libs/identity/test/CborReaderTest.cc", "libs/drogon/src/controllers/WebAuthnController.cc",
                   "tests/integration/controllers/WebAuthnHttpTest.cc"]
    social_sources = [f"libs/identity/src/social/{name}.cc" for name in
                      ("GoogleAuthService", "WeChatAuthService", "GitHubAuthService", "SocialLinkService", "SocialLoginStateService")]
    social_sources += [f"libs/identity/test/{name}.cc" for name in
                       ("SocialAuthServiceTest", "SocialLinkServiceTest", "SocialLoginStateServiceTest")]
    social_sources += [f"libs/drogon/src/controllers/{name}Controller.cc" for name in ("Google", "WeChat", "GitHub")]
    social_sources += [f"tests/integration/controllers/{name}.cc" for name in ("SocialLoginHttpTest", "SocialLinkEndpointHttpTest")]
    social_sources += ["tests/integration/social/SoftDeleteSocialRepoTest.cc"]
    for expected, group in ((webauthn, web_sources), (social, social_sources)):
        for relative in group:
            assert ((repo / relative).resolve() in sources) == expected, relative
    probes = ("libs/identity/src/AuthService.cc", "apps/server/src/bootstrap/ControllerRegistration.cc",
              "tests/integration/error/SuccessResponseGoldenSnapshotTest.cc")
    for relative in probes:
        found = [e for e in entries if Path(e["file"]).resolve() == (repo / relative).resolve()]
        assert found, relative
        for entry in found:
            argv = entry.get("arguments") or shlex.split(entry["command"])
            for name, value in (("WITH_SOCIAL", social), ("WITH_WEBAUTHN", webauthn)):
                assert any(a == f"-D{name}" or a.startswith(f"-D{name}=") for a in argv) == value, (relative, name)
            assert "-Werror" in argv, relative
    cache = cache_values(build)
    assert enabled(cache["WITH_SOCIAL"]) == social
    assert enabled(cache["WITH_WEBAUTHN"]) == webauthn
    if not webauthn:
        assert enabled(cache["CMAKE_DISABLE_FIND_PACKAGE_libcbor"])
    exports = build / "fulla-cmake/fulla-identity"
    config = (exports / "fulla-identityConfig.cmake").read_text()
    targets = (exports / "fulla-identityTargets.cmake").read_text()
    assert ("find_dependency(libcbor)" in config) == webauthn
    assert ("libcbor::libcbor" in targets) == webauthn
    return {"social": social, "webauthn": webauthn, "host_refs": sorted(host_refs),
            "all_graph_refs": sorted(graph_refs), "platform_exceptions": sorted(exceptions),
            "compiled_entries": len(entries), "exports_checked": str(exports)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--social", type=enabled, required=True)
    parser.add_argument("--webauthn", type=enabled, required=True)
    parser.add_argument("--consumer-build-dir", type=Path)
    parser.add_argument("--consumer-root", type=Path)
    parser.add_argument("--installed-consumer", action="store_true")
    parser.add_argument("--allow-platform-tool", action="append", default=[])
    args = parser.parse_args()
    result = check_build(args.repo, args.build_dir, args.graph, args.social, args.webauthn,
                         set(args.allow_platform_tool))
    if args.consumer_build_dir:
        assert args.consumer_root, "--consumer-root is required with a consumer"
        result["consumer"] = check_consumer(args.consumer_build_dir, args.consumer_root, args.webauthn,
                                             installed=args.installed_consumer)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
