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
    "libs/drogon/test/CMakeFiles/fulla-password-login-session-test.dir/link.txt",
    "tests/CMakeFiles/fulla-tests.dir/link.txt",
    "apps/server/CMakeFiles/fulla-server.dir/link.txt",
)
FLAG_CONFS = (
    "tools.build:cflags",
    "tools.build:cxxflags",
    "tools.build:exelinkflags",
    "tools.build:sharedlinkflags",
)


def verify(graph_path, build_dir=None):
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
        symbols = subprocess.run(
            ["nm", "--undefined-only", str(archive)],
            check=True, capture_output=True, text=True,
        )
        if not re.search(r"^\s+U\s+__tsan_\w+(?:@\S+)?\s*$", symbols.stdout, re.MULTILINE):
            raise ValueError(f"{name}: archive has no TSan references: {archive}")
        selected[name] = (folder, archive.resolve())
        print(f"Verified {node['ref']}:{node['package_id']} at {archive}", file=sys.stderr)
    if set(selected) != {"drogon", "trantor"}:
        raise ValueError("Expected exactly one installed host Drogon and Trantor")

    if build_dir is not None:
        for relative in TARGETS:
            link_file = build_dir / relative
            arguments = shlex.split(link_file.read_text())
            working_dir = build_dir / relative.split("/CMakeFiles/", 1)[0]
            for name, (_, expected) in selected.items():
                actual = {
                    (working_dir / arg).resolve()
                    for arg in arguments if Path(arg).name == f"lib{name}.a"
                }
                if actual != {expected}:
                    raise ValueError(
                        f"{relative}: expected only {expected}, found {sorted(map(str, actual))}"
                    )
            print(f"Verified link paths in {relative}", file=sys.stderr)
    return selected["drogon"][0] / "bin"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graph", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--print-drogon-bin", action="store_true")
    args = parser.parse_args()
    try:
        bin_dir = verify(args.graph, args.build_dir)
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
