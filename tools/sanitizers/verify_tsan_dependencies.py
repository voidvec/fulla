#!/usr/bin/env python3
"""Verify the installed TSan dependencies and their actual CMake link paths."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


TARGETS = (
    "libs/common/test/CMakeFiles/fulla-common-test.dir/link.txt",
    "libs/common/testing/test/CMakeFiles/fulla-common-testing-test.dir/link.txt",
    "libs/identity/test/CMakeFiles/fulla-identity-test.dir/link.txt",
    "libs/oauth2/test/CMakeFiles/fulla-oauth2-test.dir/link.txt",
    "libs/drogon/test/CMakeFiles/fulla-drogon-test.dir/link.txt",
    "libs/drogon/test/CMakeFiles/fulla-password-login-session-test.dir/link.txt",
    "tests/CMakeFiles/fulla-tests.dir/link.txt",
    "apps/server/CMakeFiles/fulla-server.dir/link.txt",
    "examples/third-party-host/CMakeFiles/third-party-host-smoke.dir/link.txt",
)
SDK_SMOKE_TARGET = (
    "examples/full-stack-host/CMakeFiles/fulla-full-stack-smoke.dir/link.txt",
)
REQUIRED_FRAMEWORK_LINK_TARGETS = frozenset((
    "libs/drogon/test/CMakeFiles/fulla-drogon-test.dir/link.txt",
    "libs/drogon/test/CMakeFiles/fulla-password-login-session-test.dir/link.txt",
    "tests/CMakeFiles/fulla-tests.dir/link.txt",
    "apps/server/CMakeFiles/fulla-server.dir/link.txt",
    SDK_SMOKE_TARGET[0],
))
FLAG_CONFS = (
    "tools.build:cflags",
    "tools.build:cxxflags",
    "tools.build:exelinkflags",
    "tools.build:sharedlinkflags",
)


def suppressed_first_party_symbol_names():
    suppression_file = Path(__file__).with_name("tsan.supp")
    names = set()
    for line in suppression_file.read_text().splitlines():
        match = re.fullmatch(r"race:\^([A-Za-z_]\w*)\$", line.strip())
        if match:
            names.add(match.group(1))
    return names


def verify_no_suppressed_symbols(binary):
    names = suppressed_first_party_symbol_names()
    if not names:
        return
    symbols = subprocess.run(
        ["nm", "--defined-only", str(binary)],
        check=True, capture_output=True, text=True,
    )
    collisions = sorted({
        symbol.strip()
        for symbol in symbols.stdout.split()
        if symbol.strip() in names
    })
    if collisions:
        raise ValueError(
            f"first-party binary defines suppressed symbol names in {binary}: "
            + ", ".join(collisions)
        )


def verify_archive(archive):
    members_result = subprocess.run(
        ["ar", "t", str(archive)],
        check=True, capture_output=True, text=True,
    )
    members = [line.strip() for line in members_result.stdout.splitlines() if line.strip()]
    if not members:
        raise ValueError(f"archive has no members: {archive}")
    symbols = subprocess.run(
        ["nm", "-A", "--undefined-only", str(archive)],
        check=True, capture_output=True, text=True,
    )
    instrumented = set()
    for line in symbols.stdout.splitlines():
        fields = line.rsplit(":", 2)
        if len(fields) != 3:
            continue
        _, member, symbol = fields
        if re.search(r"^\s*U\s+__tsan_\w+(?:@\S+)?\s*$", symbol):
            instrumented.add(member.strip())
    missing = [member for member in members if member not in instrumented]
    if missing:
        raise ValueError(
            f"archive members missing TSan undefined references in {archive}: "
            + ", ".join(missing)
        )


def linked_first_party_archives(build_dir, relative, arguments):
    working_dir = build_dir / relative.split("/CMakeFiles/", 1)[0]
    build_root = build_dir.resolve()
    archives = set()
    for argument in arguments:
        if not argument.endswith(".a"):
            continue
        archive = (working_dir / argument).resolve()
        try:
            archive.relative_to(build_root)
        except ValueError:
            continue
        if archive.name.startswith("libfulla-"):
            archives.add(archive)
    return archives


def verify_target(build_dir, relative):
    link_file = build_dir / relative
    arguments = shlex.split(link_file.read_text())
    working_dir = build_dir / relative.split("/CMakeFiles/", 1)[0]
    archives = linked_first_party_archives(build_dir, relative, arguments)
    if not archives:
        raise ValueError(f"{relative}: no first-party archives in link command")
    for archive in sorted(archives):
        verify_no_suppressed_symbols(archive)
        verify_archive(archive)
    flags_file = link_file.parent / "flags.make"
    flags = flags_file.read_text()
    flag_line = next(
        (line for line in flags.splitlines() if line.startswith("CXX_FLAGS =")),
        "",
    )
    if "-fsanitize=thread" not in flag_line:
        raise ValueError(f"{relative}: target is not compiled with TSan")
    if not any(arg == "-fsanitize=thread" for arg in arguments):
        raise ValueError(f"{relative}: target is not linked with TSan")
    objects = [working_dir / arg for arg in arguments if arg.endswith(".o")]
    if not objects:
        raise ValueError(f"{relative}: target has no object files")
    for object_file in objects:
        verify_no_suppressed_symbols(object_file)
        symbols = subprocess.run(
            ["nm", "--undefined-only", str(object_file)],
            check=True, capture_output=True, text=True,
        )
        if not re.search(r"^\s*U\s+__tsan_\w+(?:@\S+)?\s*$", symbols.stdout, re.MULTILINE):
            raise ValueError(f"{relative}: object is not TSan-instrumented: {object_file}")
    print(f"Verified TSan compile/link instrumentation in {relative}", file=sys.stderr)


def verify(graph_path, build_dir=None, require_sdk_smoke=False):
    graph = json.loads(graph_path.read_text())
    nodes = graph["graph"]["nodes"].values()
    selected = {}
    for node in nodes:
        name = (node.get("ref") or "").split("/", 1)[0]
        if node.get("context") != "host" or name not in ("drogon", "trantor"):
            continue
        if name in selected:
            raise ValueError(f"Multiple host nodes for {name}")
        if not node.get("package_id") or not node.get("package_folder"):
            raise ValueError(f"{name}: missing installed package identity/folder")
        conf = node.get("info", {}).get("conf", {})
        for key in FLAG_CONFS:
            flags = conf.get(key, [])
            if not isinstance(flags, list) or "-fsanitize=thread" not in flags:
                raise ValueError(f"{name}: package ID does not include {key} thread flags")
        folder = Path(node["package_folder"]).resolve()
        archive = folder / "lib" / f"lib{name}.a"
        if not archive.is_file():
            raise ValueError(f"{name}: archive missing: {archive}")
        verify_archive(archive)
        selected[name] = (folder, archive.resolve())
        print(f"Verified {node['ref']}:{node['package_id']} at {archive}", file=sys.stderr)
    if set(selected) != {"drogon", "trantor"}:
        raise ValueError("Expected exactly one installed host Drogon and Trantor")

    if build_dir is not None:
        targets = TARGETS + (SDK_SMOKE_TARGET if require_sdk_smoke else ())
        for relative in targets:
            verify_target(build_dir, relative)
            link_file = build_dir / relative
            arguments = shlex.split(link_file.read_text())
            working_dir = build_dir / relative.split("/CMakeFiles/", 1)[0]
            for name, (_, expected) in selected.items():
                actual = {
                    (working_dir / arg).resolve()
                    for arg in arguments if Path(arg).name == f"lib{name}.a"
                }
                if actual:
                    if actual != {expected}:
                        raise ValueError(
                            f"{relative}: expected only {expected}, found {sorted(map(str, actual))}"
                        )
                elif relative in REQUIRED_FRAMEWORK_LINK_TARGETS:
                    raise ValueError(f"{relative}: required {name} archive is missing")
            print(f"Verified link paths in {relative}", file=sys.stderr)
    return selected["drogon"][0] / "bin"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graph", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument(
        "--require-sdk-smoke",
        action="store_true",
        help="also verify the independently configured SDK smoke after CTest builds it",
    )
    parser.add_argument("--print-drogon-bin", action="store_true")
    args = parser.parse_args()
    try:
        bin_dir = verify(args.graph, args.build_dir, args.require_sdk_smoke)
        if args.print_drogon_bin:
            executable = bin_dir / "drogon_ctl"
            if not executable.is_file() or not os.access(executable, os.X_OK):
                raise ValueError(f"Installed Drogon tool is not executable: {executable}")
            print(bin_dir)
    except (KeyError, TypeError, ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"TSan dependency verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
