#!/usr/bin/env python3
"""Build, test and consume one supported Fulla feature combination (Python 3.10+)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

from check_build import check_build, check_consumer, enabled


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--social", type=enabled, required=True)
    parser.add_argument("--webauthn", type=enabled, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--generator", default="Unix Makefiles")
    parser.add_argument("--offline", action="store_true", help="use cached packages only")
    parser.add_argument("--lockfile-partial", action="store_true", help="explicit local profile exception; CI uses the strict lock")
    parser.add_argument("--allow-platform-tool", action="append", default=[],
                        help="exact platform ref permitted outside the lock (requires --lockfile-partial)")
    parser.add_argument("--release-flags", help="optional local compiler override, recorded in evidence")
    parser.add_argument("--runtime-config", type=Path, help="default: shipped PostgreSQL config; FULLA_DB_*/REDIS_* override at runtime")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    build = args.build_dir.resolve()
    evidence = args.evidence_dir.resolve()
    assert build != repo and not repo.is_relative_to(build), "choose a dedicated build directory"
    assert args.jobs > 0
    assert not args.allow_platform_tool or args.lockfile_partial
    # Reusing compiled objects is fine; consumer and install caches are not.
    for path in (build / "sdk-install", build / "installed-consumer", build / "examples/full-stack-host"):
        assert not path.exists(), f"consumer/install directory already exists; choose a fresh build directory: {path}"
    evidence.mkdir(parents=True, exist_ok=False)  # never overwrite an earlier attempt
    env = os.environ.copy()
    env["FULLA_FEATURE_REQUIRE_BACKENDS"] = "ON"
    env["CMAKE_BUILD_PARALLEL_LEVEL"] = str(args.jobs)
    result = {"status": "running", "social": args.social, "webauthn": args.webauthn,
              "build_dir": str(build), "commands": [], "lockfile_partial": args.lockfile_partial,
              "allowed_platform_tools": args.allow_platform_tool}
    started = time.monotonic()

    def run(name: str, command: list[str], cwd: Path = repo, json_output: Path | None = None) -> None:
        result["commands"].append({"stage": name, "argv": command, "cwd": str(cwd)})
        print(f"[{name}]", flush=True)
        with (evidence / f"{name}.log").open("wb") as log:
            if json_output:
                with json_output.open("wb") as output:
                    subprocess.run(command, cwd=cwd, env=env, stdout=output, stderr=log, check=True)
            else:
                subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

    def expect_failure(name: str, command: list[str], diagnostic: str) -> None:
        result["commands"].append({"stage": name, "argv": command, "cwd": str(repo), "expected_failure": diagnostic})
        output = subprocess.run(command, cwd=repo, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (evidence / f"{name}.log").write_bytes(output.stdout)
        assert output.returncode != 0 and diagnostic in output.stdout.decode(errors="replace"), name

    try:
        run("versions", [sys.executable, "-c",
            "import platform,subprocess; print(platform.platform()); "
            "[subprocess.run(c,check=True) for c in [['conan','--version'],['cmake','--version']]]"])
        graph = evidence / "conan-graph.json"
        install = ["conan", "install", str(repo), "--output-folder=" + str(build),
                   "-s", "build_type=Release", "-s", "compiler.cppstd=17",
                   "-o", f"&:with_social={args.social}", "-o", f"&:with_webauthn={args.webauthn}",
                   "--lockfile=" + str(repo / "conan.lock"), "--format=json",
                   "--build=never" if args.offline else "--build=missing"]
        if args.offline:
            install.append("--no-remote")
        if args.lockfile_partial:
            install.append("--lockfile-partial")
        run("conan-install", install, json_output=graph)
        identity_off = [arg if not arg.startswith("--output-folder=") else
                        "--output-folder=" + str(build / "identity-off-conan") for arg in install]
        identity_off += ["-o", "&:with_identity=False"]
        expect_failure("identity-off-conan", identity_off, "with_identity=False is not supported")
        expect_failure("identity-off-cmake", ["cmake", "-S", str(repo), "-B", str(build / "identity-off-cmake"),
            "-DWITH_IDENTITY=OFF", f"-DWITH_SOCIAL={'ON' if args.social else 'OFF'}",
            f"-DWITH_WEBAUTHN={'ON' if args.webauthn else 'OFF'}", "-DCMAKE_DISABLE_FIND_PACKAGE_jsoncpp=TRUE"],
            "WITH_IDENTITY=OFF is not supported")
        result["identity_off_rejected"] = True
        # drogon_create_views invokes the tool supplied by this resolved graph.
        for node in json.loads(graph.read_text())["graph"]["nodes"].values():
            if str(node.get("ref", "")).startswith("drogon/"):
                bindir = Path(node["package_folder"]) / "bin"
                assert (bindir / "drogon_ctl").is_file(), bindir
                env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
        common = ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=" + str(build / "conan_toolchain.cmake")]
        if args.release_flags:
            common.append("-DCMAKE_CXX_FLAGS_RELEASE=" + args.release_flags)
        if not args.webauthn:
            common.append("-DCMAKE_DISABLE_FIND_PACKAGE_libcbor=TRUE")
        run("configure", ["cmake", "-S", str(repo), "-B", str(build), "-G", args.generator,
                          *common, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DFULLA_WERROR=ON"])
        run("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", str(args.jobs)])
        result["build_audit"] = check_build(repo, build, graph, args.social, args.webauthn,
                                             set(args.allow_platform_tool))
        (evidence / "build-audit.json").write_text(json.dumps(result["build_audit"], indent=2))
        config = json.loads((args.runtime_config or repo / "apps/server/config/config.json").read_text())
        config.setdefault("app", {}).update(number_of_threads=4, relaunch_on_error=False)
        for subdir in ("tests", "apps/server"):
            (build / subdir / "config.json").write_text(json.dumps(config, indent=2))
        result["runtime_config_sha256"] = hashlib.sha256(json.dumps(config, sort_keys=True).encode()).hexdigest()
        # Uses this task's isolated PG/Redis fixture (or the CI service fixture).
        run("seed", [str(build / "tests/fulla-tests"), "-r", "Database_P0_Seed_Setup_Works"], build / "tests")
        run("registry", ["ctest", "--show-only=json-v1"], build, evidence / "ctest-registered.json")
        registry = json.loads((evidence / "ctest-registered.json").read_text())["tests"]
        names = {t["name"] for t in registry}
        required = {"OAuth2Tests", "FeatureSurface_OutOfProcess", "SdkSmoke.FullStack"}
        if args.social:
            required.add("SocialLoginState_OutOfProcess")
        if args.social and args.webauthn:
            required.add("EndpointTests_OutOfProcess")
        assert required <= names, sorted(required - names)
        run("ctest", ["ctest", "-C", "Release", "-j1", "--timeout", "1200", "--no-tests=error", "--output-on-failure",
                      "--output-junit", str(evidence / "ctest.xml")], build)
        cases = list(ET.parse(evidence / "ctest.xml").getroot().iter("testcase"))
        assert len(cases) == len(names) == len(registry)
        assert {c.attrib["name"] for c in cases} == names
        for case in cases:
            assert case.find("failure") is None and case.find("error") is None and case.find("skipped") is None, case.attrib
            assert case.get("status") in (None, "run"), case.attrib
        result["registered_tests_passed"] = len(cases)
        result["required_tests"] = sorted(required)
        result["build_consumer"] = check_consumer(build / "examples/full-stack-host", build, args.webauthn, installed=False)
        if (build / "Testing/Temporary/LastTest.log").exists():
            shutil.copyfile(build / "Testing/Temporary/LastTest.log", evidence / "ctest-detail.log")
        prefix = build / "sdk-install"
        run("install", ["cmake", "--install", str(build), "--config", "Release", "--prefix", str(prefix)])
        consumer = build / "installed-consumer"
        run("consumer-configure", ["cmake", "-S", str(repo / "examples/full-stack-host"), "-B", str(consumer),
                                   "-G", args.generator, *common, "-DCMAKE_PREFIX_PATH=" + str(prefix)])
        run("consumer-build", ["cmake", "--build", str(consumer), "--config", "Release", "--parallel", str(args.jobs)])
        result["installed_consumer"] = check_consumer(consumer, prefix, args.webauthn, installed=True)
        run("consumer-test", ["ctest", "-C", "Release", "--no-tests=error", "--output-on-failure", "--timeout", "120",
                               "--output-junit", str(evidence / "installed-consumer.xml")], consumer)
        smoke = list(ET.parse(evidence / "installed-consumer.xml").getroot().iter("testcase"))
        assert len(smoke) == 1 and smoke[0].get("name") == "smoke"
        assert smoke[0].find("failure") is None and smoke[0].find("skipped") is None
        result["status"] = "passed"
    finally:
        for source in (build / "Testing/Temporary/LastTest.log", build / "tests/feature-surface-server.log",
                       build / "installed-consumer/Testing/Temporary/LastTest.log"):
            if source.is_file():
                label = "installed-" if "installed-consumer" in source.parts else ""
                shutil.copyfile(source, evidence / (label + source.name))
        result["elapsed_seconds"] = round(time.monotonic() - started, 2)
        if result["status"] != "passed":
            result["status"] = "failed"
        (evidence / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps({"status": result["status"], "evidence": str(evidence)}))


if __name__ == "__main__":
    main()
