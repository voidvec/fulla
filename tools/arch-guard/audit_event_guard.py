#!/usr/bin/env python3
"""audit_event_guard: keep the audit event vocabulary and its emitters in sync.

The registry in libs/common/include/fulla/common/observability/AuditActions.h
is the contract between audit emitters and event-name consumers (audit log
filters, webhook subscriptions). Two drift paths survive both the compiler and
the unit tests:

  1. Declare `kNewEvent[]` but forget the `kAllEvents` entry. The array size
     static_assert still passes and AuditActionsTest only inspects array
     members, so the new event is silently absent from the closed set.
  2. Emit a bare string literal from a new call site. The sink's interface is
     string-based by design, so nothing rejects a literal that drifted from
     the registry value.

This guard closes both paths:

  * Registry: every declared `k*` constant must appear in `kAllEvents`, every
    array entry must be a declared constant, values must be unique and
    byte-sorted, and the static_assert count must match the declarations.
  * Emitters (production trees only): the action argument of the known audit
    helpers must not be a string literal, and `.action = <literal>` is
    rejected. Only the action position is checked, so legitimate literal
    target/outcome arguments ("user", "success") stay allowed.

Known blind spots (documented, accepted -- closing them needs a real AST):
  - macro-generated call sites;
  - action expressions built by string concatenation across lines
    (`"login_" "success"`), which never appear today;
  - emitters whose action position differs from the table below (add the
    callee here, with its 0-based action argument index).

Usage:
    python3 tools/arch-guard/audit_event_guard.py [--root <repo-root>]
    python3 tools/arch-guard/audit_event_guard.py --selftest

Exit codes:
    0  no violations (or selftest passes)
    1  violations found (selftest failed)
    2  misconfiguration (registry or a guarded directory is missing)
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".hxx", ".cc", ".cpp", ".cxx", ".c"}
REGISTRY_REL = Path("libs/common/include/fulla/common/observability/AuditActions.h")

RE_CONST = re.compile(r'inline\s+constexpr\s+char\s+(k\w+)\[\]\s*=\s*"((?:[^"\\]|\\.)*)"\s*;')
RE_ARRAY_BLOCK = re.compile(r"kAllEvents\s*\{(.*?)\}", re.S)
RE_ARRAY_IDENT = re.compile(r"\b(k\w+)\b")
RE_ASSERT_COUNT = re.compile(r"static_assert\s*\(\s*kAllEvents\.size\(\)\s*==\s*(\d+)")
RE_ACTION_ASSIGN = re.compile(r"\.action\s*=\s*([^;]*);")
RE_STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')

# (callee regex, 0-based action argument index, human name)
EMITTERS: List[Tuple[re.Pattern, int, str]] = [
    (re.compile(r"(?<![\w.>:])audit\s*\("), 1, "audit"),
    (re.compile(r"(?:->|\.)\s*audit\s*\("), 0, "member audit"),
    (re.compile(r"\blogFromRequest\s*\("), 1, "logFromRequest"),
    (re.compile(r"\bauditFromRequest\s*\("), 1, "auditFromRequest"),
    (re.compile(r"\bauditOrg\s*\("), 1, "auditOrg"),
    (re.compile(r"\bauditClaimMapping\s*\("), 1, "auditClaimMapping"),
]

OPENERS = "([{"
CLOSERS = ")]}"
MATCHING = {")": "(", "]": "[", "}": "{"}


def strip_comments(source: str) -> str:
    """Blank C/C++ comments, preserving layout and string literals verbatim."""
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
            if ch in ('"', "'"):
                state = "string" if ch == '"' else "char"
            out.append(ch)
            i += 1
            continue
        if state in ("line_comment", "block_comment"):
            if state == "line_comment" and ch == "\n":
                out.append("\n")
                state = "code"
            elif state == "block_comment" and ch == "*" and nxt == "/":
                out.append("  ")
                i += 2
                state = "code"
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue
        # string / char literal: keep verbatim, honor escapes
        if ch == "\\" and nxt:
            out.append(ch + nxt)
            i += 2
            continue
        if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
            state = "code"
        out.append(ch)
        i += 1
    return "".join(out)


def mask_literals(source: str) -> str:
    """Blank string/char literal contents (delimiters kept), preserving layout."""
    out: List[str] = []
    i, n = 0, len(source)
    state = "code"
    while i < n:
        ch = source[i]
        if state == "code":
            if ch in ('"', "'"):
                state = "string" if ch == '"' else "char"
            out.append(ch)
            i += 1
            continue
        if ch == "\\" and i + 1 < n:
            out.append("  ")
            i += 2
            continue
        if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
            state = "code"
            out.append(ch)
        else:
            out.append("\n" if ch == "\n" else " ")
        i += 1
    return "".join(out)


def build_bracket_map(masked: str) -> Dict[int, int]:
    stack: List[Tuple[str, int]] = []
    pairs: Dict[int, int] = {}
    for pos, ch in enumerate(masked):
        if ch in OPENERS:
            stack.append((ch, pos))
        elif ch in CLOSERS:
            if stack and stack[-1][0] == MATCHING[ch]:
                pairs[stack[-1][1]] = pos
                stack.pop()
    return pairs


def split_arguments(masked: str, open_pos: int, close_pos: int) -> List[Tuple[int, int]]:
    """Top-level comma-separated argument spans; template commas stay a blind spot."""
    spans: List[Tuple[int, int]] = []
    depth = 0
    start = open_pos + 1
    for i in range(open_pos + 1, close_pos):
        ch = masked[i]
        if ch in OPENERS:
            depth += 1
        elif ch in CLOSERS:
            depth -= 1
        elif ch == "," and depth == 0:
            spans.append((start, i))
            start = i + 1
    if masked[start:close_pos].strip():
        spans.append((start, close_pos))
    return spans


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def parse_registry(stripped: str) -> Tuple[List[Tuple[str, str]], List[str], Optional[int]]:
    constants = [(m.group(1), m.group(2)) for m in RE_CONST.finditer(stripped)]
    block = RE_ARRAY_BLOCK.search(stripped)
    members = RE_ARRAY_IDENT.findall(block.group(1)) if block else []
    asserted = RE_ASSERT_COUNT.search(stripped)
    return constants, members, int(asserted.group(1)) if asserted else None


def check_registry(stripped: str) -> List[str]:
    issues: List[str] = []
    constants, members, asserted = parse_registry(stripped)
    if not constants:
        return ["no `inline constexpr char k*[]` constants found in the registry"]
    if asserted is None:
        issues.append("missing `static_assert(kAllEvents.size() == N, ...)`")
    elif asserted != len(constants):
        issues.append(
            f"static_assert pins {asserted} entries but {len(constants)} constants are declared"
        )
    declared = {name for name, _ in constants}
    seen = set()
    for name, value in constants:
        if name in seen:
            issues.append(f"constant `{name}` is declared more than once")
        seen.add(name)
        if not re.fullmatch(r"[a-z0-9_]+|[A-Z0-9_]+", value):
            issues.append(f"`{name}` value `{value}` is not snake_case/uppercase")
    values = [value for _, value in constants]
    duplicates = sorted({v for v in values if values.count(v) > 1})
    for value in duplicates:
        issues.append(f"duplicate registry value `{value}`")
    for name, _ in constants:
        if name not in members:
            issues.append(
                f"`{name}` is declared but missing from kAllEvents "
                f"(add it; the size assert alone does not catch this)"
            )
    member_set = set(members)
    for name in members:
        if name not in declared:
            issues.append(f"kAllEvents references undeclared constant `{name}`")
    if len(members) != len(member_set):
        issues.append("kAllEvents contains duplicate entries")
    by_name = dict(constants)
    ordered = [by_name[name] for name in members if name in by_name]
    if ordered != sorted(ordered):
        issues.append("kAllEvents is not byte-sorted by value")
    return issues


def iter_emitter_violations(path: str, stripped: str, known_values: Dict[str, str]) -> List[str]:
    issues: List[str] = []
    masked = mask_literals(stripped)
    pairs = build_bracket_map(masked)
    for pattern, action_index, name in EMITTERS:
        for match in pattern.finditer(masked):
            open_pos = masked.find("(", match.start())
            close_pos = pairs.get(open_pos)
            if close_pos is None:
                continue
            args = split_arguments(masked, open_pos, close_pos)
            if action_index >= len(args):
                continue
            start, end = args[action_index]
            for literal in RE_STRING.finditer(stripped, start, end):
                value = literal.group(1)
                known = known_values.get(value)
                hint = f" (registry constant `{known}`)" if known else ""
                issues.append(
                    f"{path}:{line_of(stripped, literal.start())}: {name} action argument "
                    f'is a string literal "{value}"{hint}; use the audit_events constant'
                )
    for match in RE_ACTION_ASSIGN.finditer(stripped):
        rhs_start, rhs_end = match.start(1), match.end(1)
        for literal in RE_STRING.finditer(stripped, rhs_start, rhs_end):
            value = literal.group(1)
            known = known_values.get(value)
            hint = f" (registry constant `{known}`)" if known else ""
            issues.append(
                f"{path}:{line_of(stripped, literal.start())}: .action assignment uses "
                f'string literal "{value}"{hint}; use the audit_events constant'
            )
    return issues


def iter_source_files(root: Path):
    libs_dir = root / "libs"
    if not libs_dir.is_dir():
        raise FileNotFoundError(f"guarded directory not found: {libs_dir}")
    for lib in sorted(p for p in libs_dir.iterdir() if p.is_dir()):
        for sub in ("include", "src"):
            sub_dir = lib / sub
            if not sub_dir.is_dir():
                continue
            for path in sorted(sub_dir.rglob("*")):
                if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                    yield path
    app_src = root / "apps" / "server" / "src"
    if not app_src.is_dir():
        raise FileNotFoundError(f"guarded directory not found: {app_src}")
    for path in sorted(app_src.rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


# --------------------------------------------------------------------------
# Selftest
# --------------------------------------------------------------------------

GOOD_HEADER = """inline constexpr char kLoginSuccess[] = "login_success";
inline constexpr char kTokenIssued[] = "token_issued";
inline constexpr std::array<std::string_view, 2> kAllEvents{
  kLoginSuccess,
  kTokenIssued,
};
static_assert(kAllEvents.size() == 2, "register new audit events in kAllEvents");
"""

MISSING_MEMBER_HEADER = GOOD_HEADER.replace("  kTokenIssued,\n", "")
EXTRA_MEMBER_HEADER = GOOD_HEADER.replace("  kTokenIssued,\n", "  kTokenIssued,\n  kGhost,\n")
DUPLICATE_VALUE_HEADER = GOOD_HEADER.replace('"token_issued"', '"login_success"')
UNSORTED_HEADER = GOOD_HEADER.replace(
    "  kLoginSuccess,\n  kTokenIssued,", "  kTokenIssued,\n  kLoginSuccess,"
)
ASSERT_MISMATCH_HEADER = GOOD_HEADER.replace("== 2", "== 3")


def selftest() -> int:
    cases = [
        ("consistent registry", GOOD_HEADER, 0),
        ("declared but unregistered", MISSING_MEMBER_HEADER, 1),
        ("array entry without constant", EXTRA_MEMBER_HEADER, 1),
        ("duplicate value", DUPLICATE_VALUE_HEADER, 1),
        ("unsorted array", UNSORTED_HEADER, 1),
        ("static_assert count drift", ASSERT_MISMATCH_HEADER, 1),
    ]
    failed = 0
    for name, header, expected in cases:
        got = len(check_registry(strip_comments(header)))
        status = "PASS" if got == expected else "FAIL"
        if got != expected:
            failed += 1
        print(f"  [{status}] registry/{name}: expected {expected}, got {got}")

    known = {"login_success": "kLoginSuccess", "token_issued": "kTokenIssued"}
    emitter_cases = [
        ("literal action", 'void f() { audit(req, "user_create", id); }', 1),
        ("constant action", "void f() { audit(req, ae::kUserCreate, id); }", 0),
        ("literal target stays legal", 'void f() { audit(req, ae::kUserCreate, "user", id); }', 0),
        ("action assignment", 'void f() { ev.action = "login_success"; }', 1),
        ("constant assignment", "void f() { ev.action = ae::kLoginSuccess; }", 0),
        (
            "sink call",
            'void f() { DrogonAuditSink::logFromRequest(s, "login_success", "ok", r); }',
            1,
        ),
        (
            "sink constant",
            'void f() { DrogonAuditSink::logFromRequest(s, ae::kLoginSuccess, "ok", r); }',
            0,
        ),
        ("member audit", 'void f() { self->audit("token_issued", "ok", a, t, i); }', 1),
        ("declaration only", "void audit(const Req &req, const char *action, int id);", 0),
        (
            "non-audit literal with same value",
            'void f() { incrementCacheCounter("mfa_enabled", labels, 1); }',
            0,
        ),
        ("commented literal ignored", '// audit(req, "login_success", id);', 0),
        ("org wrapper", 'void f() { auditOrg(req, "org_updated", slug); }', 1),
    ]
    for name, source, expected in emitter_cases:
        got = len(iter_emitter_violations("fixture.cc", strip_comments(source), known))
        status = "PASS" if got == expected else "FAIL"
        if got != expected:
            failed += 1
        print(f"  [{status}] emitters/{name}: expected {expected}, got {got}")

    if failed:
        print(f"selftest FAILED ({failed} case(s))")
        return 1
    print("selftest passed")
    return 0


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Audit event registry/emitter guard")
    default_root = Path(__file__).resolve().parents[2]
    parser.add_argument("--root", type=Path, default=default_root)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    root = args.root.resolve()
    print("========================================")
    print("Audit event registry/emitter guard")
    print("========================================")
    print(f"root: {root}")
    print("")

    registry_path = root / REGISTRY_REL
    if not registry_path.is_file():
        print(f"[ERROR] registry not found: {registry_path}")
        return 2
    registry_source = strip_comments(registry_path.read_text(encoding="utf-8", errors="replace"))
    constants, _, _ = parse_registry(registry_source)
    known_values = {value: name for name, value in constants}

    issues = check_registry(registry_source)
    try:
        files = list(iter_source_files(root))
    except FileNotFoundError as exc:
        print(f"[ERROR] {exc}")
        return 2
    for path in files:
        if path == registry_path:
            continue
        stripped = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        issues.extend(iter_emitter_violations(str(path.relative_to(root)), stripped, known_values))

    print(f"scanned {len(files)} source files; {len(constants)} registered events.")
    for issue in issues:
        print(f"[VIOLATION] {issue}")
    if issues:
        print(f"[ERROR] {len(issues)} audit event violation(s) found.")
        return 1
    print("[PASS] registry matches declarations; emitters use registry constants.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))