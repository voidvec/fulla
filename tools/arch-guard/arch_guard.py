#!/usr/bin/env python3
"""arch-guard: enforce Fulla SDK layering rules.

Spec: .kiro/specs/fulla-sdk-refactor/tasks.md Task 33 (M6). This is a
static source check over the framework-agnostic Domain libraries
(libs/common, libs/oauth2, libs/identity). It fails (exit 1) on any violation
of the rules the refactor established:

  R1  Domain code must NOT include Drogon headers (``#include <drogon/...>``).
      jsoncpp (``<json/...>``) stays allowed -- it is the one framework-neutral
      serialization dependency the Domain layer is permitted.
  R2  libs/oauth2 and libs/identity must NOT include each other's public
      headers -- the two SDK domains stay decoupled and separately consumable.
  R3  Domain code must NOT use ``drogon::orm`` -- ORM belongs to
      libs/storage-postgres, never the Domain.
  R4  apps/server product services are a REGISTERED EXCEPTION allowed to
      construct ``drogon::orm::Mapper`` directly (#222, v1.5.0 M0; see
      apps/server/AGENTS.md). The exception is capped: the number of
      ``Mapper<`` construction points under apps/server/src (comment-stripped,
      same caliber as R1-R3) must stay <= the frozen baseline. Lower the
      baseline when the count drops; NEVER raise it.

Only production code is scanned (each lib's ``include/`` and ``src/`` trees).
The ``test/`` and ``testing/`` trees are intentionally excluded: unit tests may
legitimately pull in ``<drogon/drogon_test.h>`` or framework fakes.

Comments and string literals are stripped before matching so that prose which
merely *mentions* a forbidden token (e.g. a rule reminder in a header comment)
does not trip the guard.

Usage:
    python tools/arch-guard/arch_guard.py [--root <repo-root>]

Exit codes:
    0  all rules pass
    1  one or more violations found
    2  misconfiguration (a guarded library directory is missing)
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable, List, NamedTuple

# Domain libraries that must remain framework-agnostic. Each is scanned under
# its production sub-trees only.
DOMAIN_LIBS = ("common", "oauth2", "identity")
PRODUCTION_SUBDIRS = ("include", "src")

SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".hxx", ".cc", ".cpp", ".cxx", ".c"}

# R1: an actual Drogon include directive.
RE_DROGON_INCLUDE = re.compile(r'#\s*include\s*[<"]drogon/')
# R3: any use of the drogon::orm namespace.
RE_DROGON_ORM = re.compile(r'\bdrogon::orm\b')
# R2: cross-domain includes (oauth2 <-> identity), matched by public header path.
RE_INCLUDE_IDENTITY = re.compile(r'#\s*include\s*[<"][^">]*fulla/identity/')
RE_INCLUDE_OAUTH2 = re.compile(r'#\s*include\s*[<"][^">]*fulla/oauth2/')
# R4: a direct Mapper construction point (`Mapper<X> m(db)` or the inline
# `Mapper<X>(db).findBy(...)` form). Every use site constructs one.
RE_MAPPER_CONSTRUCTION = re.compile(r'\bMapper\s*<')

# R4 cap, frozen at v1.5.0 M0 (#222) after the ClientOwnersRepository
# extraction. Measured with this script's own caliber (comment-stripped
# apps/server/src production sources). Direction: DOWN only -- lower the
# constant when the count drops, never raise it back.
R4_MAPPER_BASELINE = 57


class Violation(NamedTuple):
    rule: str
    path: Path
    line: int
    text: str


def strip_comments(source: str) -> str:
    """Blank out C/C++ comments while preserving line numbers and layout.

    Line and block comment characters are replaced with spaces (newlines kept)
    so a subsequent line-based scan still reports accurate line numbers and
    never matches a forbidden token that only appears inside a comment or a
    string literal.
    """
    out: List[str] = []
    i, n = 0, len(source)
    state = "code"  # code | line_comment | block_comment | string | char
    while i < n:
        ch = source[i]
        nxt = source[i + 1] if i + 1 < n else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                out.append("  ")
                i += 2
                state = "line_comment"
                continue
            if ch == "/" and nxt == "*":
                out.append("  ")
                i += 2
                state = "block_comment"
                continue
            if ch == '"':
                out.append(ch)
                i += 1
                state = "string"
                continue
            if ch == "'":
                out.append(ch)
                i += 1
                state = "char"
                continue
            out.append(ch)
            i += 1
            continue
        if state == "line_comment":
            if ch == "\n":
                out.append("\n")
                state = "code"
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block_comment":
            if ch == "*" and nxt == "/":
                out.append("  ")
                i += 2
                state = "code"
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
            continue
        # string / char literal: copy verbatim, honoring escapes and closing.
        out.append(ch)
        if ch == "\\" and nxt:
            out.append(nxt)
            i += 2
            continue
        if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
            state = "code"
        i += 1
    return "".join(out)


def iter_source_files(root: Path) -> Iterable[tuple[str, Path]]:
    """Yield (lib_name, file_path) for every production source file to scan."""
    for lib in DOMAIN_LIBS:
        lib_dir = root / "libs" / lib
        if not lib_dir.is_dir():
            raise FileNotFoundError(f"guarded library directory not found: {lib_dir}")
        for sub in PRODUCTION_SUBDIRS:
            sub_dir = lib_dir / sub
            if not sub_dir.is_dir():
                continue
            for path in sorted(sub_dir.rglob("*")):
                if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                    yield lib, path


def count_mapper_points(root: Path) -> List[tuple[Path, int]]:
    """R4: per-file direct Mapper construction points under apps/server/src.

    Same caliber as R1-R3 (comment-stripped source text). Returns one
    (relative path, count) entry per file with a nonzero count.
    """
    app_src = root / "apps" / "server" / "src"
    if not app_src.is_dir():
        raise FileNotFoundError(f"guarded directory not found: {app_src}")
    per_file: List[tuple[Path, int]] = []
    for path in sorted(app_src.rglob("*")):
        if not (path.is_file() and path.suffix in SOURCE_SUFFIXES):
            continue
        cleaned = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        count = len(RE_MAPPER_CONSTRUCTION.findall(cleaned))
        if count:
            per_file.append((path.relative_to(root), count))
    return per_file


def scan_file(lib: str, path: Path, root: Path) -> List[Violation]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    cleaned = strip_comments(raw)
    rel = path.relative_to(root).as_posix()
    violations: List[Violation] = []
    for lineno, line in enumerate(cleaned.splitlines(), start=1):
        if RE_DROGON_INCLUDE.search(line):
            violations.append(Violation("R1 (no <drogon/...> include)", Path(rel), lineno, line.strip()))
        if RE_DROGON_ORM.search(line):
            violations.append(Violation("R3 (no drogon::orm)", Path(rel), lineno, line.strip()))
        if lib == "oauth2" and RE_INCLUDE_IDENTITY.search(line):
            violations.append(Violation("R2 (oauth2 must not include identity)", Path(rel), lineno, line.strip()))
        if lib == "identity" and RE_INCLUDE_OAUTH2.search(line):
            violations.append(Violation("R2 (identity must not include oauth2)", Path(rel), lineno, line.strip()))
    return violations


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Fulla Domain layering guard")
    default_root = Path(__file__).resolve().parents[2]
    parser.add_argument(
        "--root",
        type=Path,
        default=default_root,
        help="repository root (defaults to two levels above this script)",
    )
    args = parser.parse_args(argv)
    root: Path = args.root.resolve()

    print("========================================")
    print("Fulla arch-guard (Domain layering)")
    print("========================================")
    print(f"root: {root}")
    print(f"scanning libs: {', '.join(DOMAIN_LIBS)} (include/, src/)")
    print("scanning apps/server/src for R4 (Mapper construction cap)")
    print("")

    try:
        files = list(iter_source_files(root))
        mapper_points = count_mapper_points(root)
    except FileNotFoundError as exc:
        print(f"[ERROR] {exc}")
        return 2

    violations: List[Violation] = []
    for lib, path in files:
        violations.extend(scan_file(lib, path, root))

    print(f"scanned {len(files)} source files.")

    # R4: registered-exception cap on direct Mapper construction in
    # apps/server product services (#222). Only exceeding the frozen
    # baseline fails; the per-file breakdown is always printed so a
    # reviewer can re-freeze DOWN after a cleanup PR.
    total_mapper_points = sum(count for _, count in mapper_points)
    print("")
    print(f"R4 apps/server/src Mapper construction points: {total_mapper_points} "
          f"(frozen baseline {R4_MAPPER_BASELINE})")
    for path, count in mapper_points:
        print(f"    {count:3d}  {path}")

    if violations or total_mapper_points > R4_MAPPER_BASELINE:
        print("")
        print("[ERROR] 架构守卫检查失败 (arch-guard violations detected):")
        print("--------------------------------------------------------")
        for v in violations:
            print(f"  {v.rule}")
            print(f"    {v.path}:{v.line}: {v.text}")
        if total_mapper_points > R4_MAPPER_BASELINE:
            print(f"  R4 (apps/server Mapper cap {R4_MAPPER_BASELINE})")
            print(f"    apps/server/src constructs Mapper at {total_mapper_points} points")
        print("--------------------------------------------------------")
        print("Rules: Domain (common/oauth2/identity) forbids <drogon/...> and")
        print("drogon::orm; oauth2 and identity must not include each other;")
        print("apps/server/src direct Mapper constructions are capped (R4).")
        return 1

    print("[PASS] 架构守卫检查通过 (all Domain layering rules satisfied).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
