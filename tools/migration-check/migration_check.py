#!/usr/bin/env python3
"""migration-check: enforce Fulla schema-migration hygiene.

Spec: .kiro/specs/fulla-sdk-refactor/tasks.md Task 35 (M6). Static check
over ``apps/server/migrations/*.sql``. It guards the assumptions the two
migration executors actually rely on but do not verify themselves:

  * ``SchemaManager::scanMigrationFiles`` matches ``V(\\d+)__.+\\.sql``, sorts
    numerically, and silently ignores non-matching files and duplicate
    versions.
  * CI (``_build-test.yml``) applies ``migrations/V*.sql`` via a shell glob,
    i.e. in *lexicographic* order — which equals numeric order only while the
    version field stays zero-padded to a fixed width.
  * ``SchemaManager`` records a checksum per applied migration but never
    compares it again, so edits to an already-applied migration go unnoticed.

Rules (all violations exit 1):

  M1  Filename: every ``*.sql`` in the migrations dir must match
      ``V<3-digit zero-padded>__<snake_case>.sql`` exactly. Non-matching
      files would be silently skipped by the runtime executor.
  M2  Sequence: versions must be unique and contiguous starting at V001
      (duplicates would be applied non-deterministically; gaps usually mean
      a lost or mis-numbered migration).
  M3  Idempotency (project convention — both executors may re-enter):
      ``CREATE TABLE``/``CREATE [UNIQUE] INDEX`` need ``IF NOT EXISTS``;
      ``ADD COLUMN`` needs ``IF NOT EXISTS``; top-level ``INSERT`` needs
      ``ON CONFLICT``; ``CREATE FUNCTION`` must be ``CREATE OR REPLACE``;
      ``ADD CONSTRAINT x`` requires a preceding
      ``DROP CONSTRAINT IF EXISTS x`` in the same file.
  M4  Rollback safety (forward-only, non-destructive policy — there are no
      down-migrations): no ``DROP TABLE``, ``DROP COLUMN``, ``TRUNCATE``,
      ``DROP SCHEMA``, ``DROP DATABASE`` or top-level ``DELETE FROM``.
      (``DROP CONSTRAINT IF EXISTS`` / ``DROP NOT NULL`` stay allowed.)
      A deliberately RATIFIED destructive migration (explicit design ruling,
      e.g. the v1.5.0 V7 dead-column cleanup) is recorded into
      ``baseline.json`` via ``--update-baseline --ratify-destructive
      <file>`` and stops being a violation; the M5 checksum lock means the
      ratification covers exactly the baselined content — editing the file
      afterwards re-trips M5.
  M5  Immutability: SHA-256 of each migration (line endings normalized to
      LF) must match ``baseline.json`` next to this script. New migrations
      must be added deliberately via ``--update-baseline``; modifying or
      deleting an already-baselined migration is a violation.

Comments, string literals and dollar-quoted bodies (plpgsql) are stripped
before M3/M4 matching, so a ``DELETE`` inside a function body does not trip
the guard.

Usage:
    python tools/migration-check/migration_check.py [--root <repo-root>]
        [--migrations-dir <dir>] [--baseline <file>] [--update-baseline]
        [--ratify-destructive FILENAME ...]

Exit codes:
    0  all rules pass
    1  one or more violations found
    2  misconfiguration (migrations dir missing, unreadable baseline)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import List, NamedTuple

FILENAME_RE = re.compile(r"^V(\d{3})__([a-z0-9_]+)\.sql$")

RE_CREATE_TABLE = re.compile(r"\bCREATE\s+TABLE\b", re.I)
RE_CREATE_TABLE_INE = re.compile(r"\bCREATE\s+TABLE\s+IF\s+NOT\s+EXISTS\b", re.I)
RE_CREATE_INDEX = re.compile(r"\bCREATE\s+(?:UNIQUE\s+)?INDEX\b", re.I)
RE_IF_NOT_EXISTS = re.compile(r"\bIF\s+NOT\s+EXISTS\b", re.I)
RE_ADD_COLUMN = re.compile(r"\bADD\s+COLUMN\b", re.I)
RE_ADD_COLUMN_INE = re.compile(r"\bADD\s+COLUMN\s+IF\s+NOT\s+EXISTS\b", re.I)
RE_INSERT = re.compile(r"^\s*INSERT\s+INTO\b", re.I)
RE_ON_CONFLICT = re.compile(r"\bON\s+CONFLICT\b", re.I)
RE_CREATE_FUNCTION = re.compile(r"\bCREATE\s+FUNCTION\b", re.I)
RE_ADD_CONSTRAINT = re.compile(r"\bADD\s+CONSTRAINT\s+(\w+)", re.I)
RE_DESTRUCTIVE = re.compile(
    r"\b(DROP\s+TABLE|DROP\s+COLUMN|TRUNCATE|DROP\s+SCHEMA|DROP\s+DATABASE)\b", re.I
)
RE_TOP_DELETE = re.compile(r"^\s*DELETE\s+FROM\b", re.I)


class Violation(NamedTuple):
    rule: str
    filename: str
    detail: str


def strip_sql(text: str) -> str:
    """Remove comments, single-quoted strings and dollar-quoted bodies."""
    out: List[str] = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "-" and nxt == "-":  # line comment
            j = text.find("\n", i)
            i = n if j == -1 else j  # keep the newline
        elif ch == "/" and nxt == "*":  # block comment
            j = text.find("*/", i + 2)
            i = n if j == -1 else j + 2
        elif ch == "'":  # string literal ('' escapes)
            i += 1
            while i < n:
                if text[i] == "'" and (i + 1 >= n or text[i + 1] != "'"):
                    break
                i += 2 if text[i] == "'" else 1
            i += 1
            out.append("''")
        elif ch == "$":  # dollar-quoted body ($$ or $tag$)
            m = re.match(r"\$(\w*)\$", text[i:])
            if m:
                closer = m.group(0)
                j = text.find(closer, i + len(closer))
                i = n if j == -1 else j + len(closer)
                out.append(" ")
            else:
                out.append(ch)
                i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def split_statements(sql: str) -> List[str]:
    return [s.strip() for s in sql.split(";") if s.strip()]


def sha256_lf(path: Path) -> str:
    data = path.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def check_naming_and_sequence(files: List[Path]) -> tuple[List[Violation], List[Path]]:
    """M1 + M2. Returns violations and the well-named subset for later rules."""
    violations: List[Violation] = []
    named: List[tuple[int, Path]] = []
    for f in sorted(files, key=lambda p: p.name):
        m = FILENAME_RE.match(f.name)
        if not m:
            violations.append(Violation(
                "M1", f.name,
                "does not match V<NNN>__<snake_case>.sql (would be silently "
                "skipped by SchemaManager and break CI's lexicographic glob)"))
            continue
        named.append((int(m.group(1)), f))

    versions = [v for v, _ in named]
    seen: dict[int, str] = {}
    for v, f in named:
        if v in seen:
            violations.append(Violation(
                "M2", f.name,
                f"duplicate version V{v:03d} (also {seen[v]}); SchemaManager "
                "applies duplicates in filesystem order — non-deterministic"))
        else:
            seen[v] = f.name
    if versions:
        expected = list(range(1, max(versions) + 1))
        missing = sorted(set(expected) - set(versions))
        for v in missing:
            violations.append(Violation(
                "M2", f"V{v:03d}", "gap in version sequence (missing file)"))
    return violations, [f for _, f in named]


def check_content(f: Path, ratified: set) -> List[Violation]:
    """M3 + M4 on one migration file. `ratified` holds filenames whose M4
    destructive statements were deliberately ratified (see --ratify-destructive);
    M3 and all other rules still apply to them."""
    violations: List[Violation] = []
    stripped = strip_sql(f.read_text(encoding="utf-8"))
    statements = split_statements(stripped)
    dropped_constraints = set(
        m.group(1).lower()
        for m in re.finditer(r"\bDROP\s+CONSTRAINT\s+IF\s+EXISTS\s+(\w+)", stripped, re.I)
    )
    for stmt in statements:
        one_line = " ".join(stmt.split())
        excerpt = one_line[:80] + ("…" if len(one_line) > 80 else "")
        if RE_CREATE_TABLE.search(stmt) and not RE_CREATE_TABLE_INE.search(stmt):
            violations.append(Violation("M3", f.name, f"CREATE TABLE without IF NOT EXISTS: {excerpt}"))
        if RE_CREATE_INDEX.search(stmt) and not RE_IF_NOT_EXISTS.search(stmt):
            violations.append(Violation("M3", f.name, f"CREATE INDEX without IF NOT EXISTS: {excerpt}"))
        if RE_ADD_COLUMN.search(stmt) and not RE_ADD_COLUMN_INE.search(stmt):
            violations.append(Violation("M3", f.name, f"ADD COLUMN without IF NOT EXISTS: {excerpt}"))
        if RE_INSERT.search(stmt) and not RE_ON_CONFLICT.search(stmt):
            violations.append(Violation("M3", f.name, f"INSERT without ON CONFLICT: {excerpt}"))
        if RE_CREATE_FUNCTION.search(stmt):
            violations.append(Violation("M3", f.name, f"CREATE FUNCTION without OR REPLACE: {excerpt}"))
        m = RE_ADD_CONSTRAINT.search(stmt)
        if m and m.group(1).lower() not in dropped_constraints:
            violations.append(Violation(
                "M3", f.name,
                f"ADD CONSTRAINT {m.group(1)} without a paired DROP CONSTRAINT "
                f"IF EXISTS {m.group(1)} in the same file: {excerpt}"))
        d = RE_DESTRUCTIVE.search(stmt)
        if d and f.name not in ratified:
            violations.append(Violation("M4", f.name, f"destructive statement ({d.group(1)}): {excerpt}"))
        if RE_TOP_DELETE.search(stmt) and f.name not in ratified:
            violations.append(Violation("M4", f.name, f"top-level DELETE FROM: {excerpt}"))
    return violations


def load_baseline(baseline_path: Path) -> tuple[dict, set]:
    """Return (migrations map, ratified-destructive filenames).

    Accepts both the pre-ratification flat shape ({name: sha}) and the
    nested shape ({"migrations": {...}, "ratified_destructive": [...]}) so
    a stale flat baseline keeps parsing (with no ratifications).
    """
    raw = json.loads(baseline_path.read_text(encoding="utf-8"))
    if isinstance(raw, dict) and "migrations" in raw and isinstance(raw["migrations"], dict):
        ratified = set(raw.get("ratified_destructive") or [])
        return raw["migrations"], ratified
    return raw, set()


def check_baseline(files: List[Path], baseline_path: Path, update: bool,
                   newly_ratified: set) -> List[Violation]:
    """M5. Compare (or rewrite) the checksum baseline, persisting any new
    M4 ratifications (see --ratify-destructive)."""
    current = {f.name: sha256_lf(f) for f in files}
    if update:
        _, existing = (load_baseline(baseline_path) if baseline_path.is_file() else ({}, set()))
        ratified = existing | newly_ratified
        baseline_path.write_text(
            json.dumps({
                "migrations": dict(sorted(current.items())),
                "ratified_destructive": sorted(ratified),
            }, indent=2) + "\n", encoding="utf-8")
        print(f"migration-check: baseline updated ({len(current)} entries, "
              f"{len(ratified)} ratified-destructive file(s)) -> {baseline_path}")
        return []

    if not baseline_path.is_file():
        return [Violation("M5", baseline_path.name,
                          "baseline missing; run with --update-baseline and commit it")]
    try:
        recorded, _ = load_baseline(baseline_path)
    except json.JSONDecodeError as e:
        print(f"migration-check: cannot parse {baseline_path}: {e}", file=sys.stderr)
        sys.exit(2)

    violations: List[Violation] = []
    for name, digest in sorted(current.items()):
        if name not in recorded:
            violations.append(Violation(
                "M5", name, "new migration not in baseline; review it, then run --update-baseline"))
        elif recorded[name] != digest:
            violations.append(Violation(
                "M5", name,
                "content changed since baselined — applied migrations are immutable; "
                "add a new V<NNN> migration instead"))
    for name in sorted(set(recorded) - set(current)):
        violations.append(Violation(
            "M5", name, "baselined migration deleted — applied migrations must not be removed"))
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description="Fulla migration hygiene check")
    parser.add_argument("--root", default=".", help="repository root (default: cwd)")
    parser.add_argument("--migrations-dir", default=None,
                        help="override migrations dir (default: <root>/apps/server/migrations)")
    parser.add_argument("--baseline", default=None,
                        help="override baseline file (default: alongside this script)")
    parser.add_argument("--update-baseline", action="store_true",
                        help="rewrite the checksum baseline from current files")
    parser.add_argument("--ratify-destructive", action="append", default=[], metavar="FILENAME",
                        help="with --update-baseline: record that FILENAME's M4 destructive "
                             "statements are ratified by an explicit design ruling; the "
                             "ratification is persisted in the baseline and pinned to the "
                             "file's M5 checksum")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    migrations_dir = (Path(args.migrations_dir) if args.migrations_dir
                      else root / "apps" / "server" / "migrations")
    baseline_path = (Path(args.baseline) if args.baseline
                     else Path(__file__).resolve().parent / "baseline.json")

    if not migrations_dir.is_dir():
        print(f"migration-check: migrations dir not found: {migrations_dir}", file=sys.stderr)
        return 2

    if args.ratify_destructive and not args.update_baseline:
        print("migration-check: --ratify-destructive requires --update-baseline "
              "(ratifications are persisted via the baseline)", file=sys.stderr)
        return 2

    sql_files = sorted(migrations_dir.glob("*.sql"), key=lambda p: p.name)
    if not sql_files:
        print(f"migration-check: no .sql files in {migrations_dir}", file=sys.stderr)
        return 2

    violations, named_files = check_naming_and_sequence(sql_files)
    known_names = {f.name for f in named_files}
    unknown = sorted(set(args.ratify_destructive) - known_names)
    if unknown:
        print(f"migration-check: --ratify-destructive names unknown migration(s): "
              f"{', '.join(unknown)}", file=sys.stderr)
        return 2

    # M4 ratifications live in the baseline, so they must be loaded before
    # the content checks run (on --update-baseline the CLI args join the
    # persisted set; the baseline rewrites with the union further down).
    ratified: set = set()
    if baseline_path.is_file():
        try:
            _, ratified = load_baseline(baseline_path)
        except json.JSONDecodeError as e:
            print(f"migration-check: cannot parse {baseline_path}: {e}", file=sys.stderr)
            return 2
    ratified |= set(args.ratify_destructive)
    if ratified:
        print(f"migration-check: M4 destructive statements ratified in: "
              f"{', '.join(sorted(ratified))}")

    for f in named_files:
        violations.extend(check_content(f, ratified))
    violations.extend(check_baseline(named_files, baseline_path, args.update_baseline,
                                     set(args.ratify_destructive)))

    if violations:
        print(f"migration-check: {len(violations)} violation(s):\n")
        for v in violations:
            print(f"  [{v.rule}] {v.filename}: {v.detail}")
        return 1

    print(f"migration-check: OK ({len(named_files)} migrations, "
          f"V001..V{len(named_files):03d}, baseline verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
