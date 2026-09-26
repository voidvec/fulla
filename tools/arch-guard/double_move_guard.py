#!/usr/bin/env python3
"""double_move_guard: flag one callback std::move'd into two lambdas of the
same call expression (#183).

The A-2 crash pattern (DeviceCodeService, fixed in PR #180 / f1f418b5): a
single std::function callback is init-capture-moved into BOTH continuation
lambdas of one Mapper call. The two lambda objects are constructed
unconditionally before the call; whichever loses the race holds an empty
std::function, and the losing branch aborts on invocation. The compiler
cannot catch it and the failure only manifests when the losing branch runs,
so review-only vigilance does not hold (the sibling functions were missed
a month after the first fix).

Rule: flag `x = std::move(x)` init-captures when BOTH hold:
  1. the two captures are direct arguments of the SAME call expression --
     the bracket-stack entry immediately below each capture `[` is the same
     `(` node;
  2. the two lambda regions are disjoint. Nested lambdas are the legal
     serial-continuation chain (the inner lambda moves the OUTER lambda's
     member copy -- TokenEndpointController's revoke chains), and mutually
     exclusive branches call DIFFERENT functions, so both legal shapes fall
     out of the same-call requirement without branch analysis.

Known blind spots (documented, accepted -- closing them needs a real AST):
  - lambdas as elements of a brace-init-list (`Foo fs = {[m(x)]{}, [m(x)]{}};`)
  - wrapped-call siblings (`f([m(x)]{}, g([m(x)]{}))` -- the inner capture's
    direct call is g, so the pair never shares one call node)
  - moves as plain function arguments (`respondError(req, std::move(cb), ...)`)
    -- not init-captures, and the mutual-exclusion style is legal
  - sequential statement-level lambda captures (each `auto l = [m(x)]{};`)
Scan scope mirrors the arch-guard caliber: production trees
libs/<lib>/{include,src} and apps/server/src; tests are excluded by design.

Usage:
    python3 tools/arch-guard/double_move_guard.py [--root <repo-root>]
    python3 tools/arch-guard/double_move_guard.py --selftest

Exit codes:
    0  no violations (or selftest passes)
    1  violations found (selftest failed)
    2  misconfiguration (a guarded directory is missing)
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# The moved-from variable is the identity that matters: two captures
# `[a = std::move(cb)]` and `[b = std::move(cb)]` both consume the SAME
# original object, so group(2) (the moved name) is the violation key.
RE_CAPTURE_MOVE = re.compile(r'(\w+)\s*=\s*std::move\s*\(\s*(\w+)\s*\)')

SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".hxx", ".cc", ".cpp", ".cxx", ".c"}
OPENERS = "([{"
CLOSERS = ")]}"
MATCHING = {")": "(", "]": "[", "}": "{"}


def blank_literals(source: str) -> str:
    """Blank comments AND string/char literal contents, preserving newlines.

    Unlike arch_guard's strip_comments (which keeps literals verbatim for
    R1-R3), this scanner tracks bracket balance: `sql.append(1, ')')` style
    character literals would poison the stack, so literal contents must go.
    Newlines are kept so reported line numbers stay accurate.
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
            if ch in ('"', "'"):
                out.append(ch)
                i += 1
                state = "string" if ch == '"' else "char"
                continue
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
        # string / char literal: blank the contents, keep the delimiters.
        if ch == "\\" and nxt:
            out.append("  ")
            i += 2
            continue
        if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
            out.append(ch)
            state = "code"
        else:
            out.append("\n" if ch == "\n" else " ")
        i += 1
    return "".join(out)


def build_bracket_map(cleaned: str) -> Dict[int, int]:
    """Position map open_pos -> close_pos for balanced bracket pairs.

    Stray closers (none expected after blanking, but generated code is
    creative) are ignored rather than desynchronizing the stack.
    """
    stack: List[Tuple[str, int]] = []
    pairs: Dict[int, int] = {}
    for pos, ch in enumerate(cleaned):
        if ch in OPENERS:
            stack.append((ch, pos))
        elif ch in CLOSERS:
            if stack and stack[-1][0] == MATCHING[ch]:
                pairs[stack[-1][1]] = pos
                stack.pop()
    return pairs


class CaptureMove:
    __slots__ = ("moved_var", "pos", "capture_open", "direct_call_open", "region")

    def __init__(self, moved_var: str, pos: int, capture_open: int,
                 direct_call_open: Optional[int], region: Tuple[int, int]):
        self.moved_var = moved_var
        self.pos = pos
        self.capture_open = capture_open
        self.direct_call_open = direct_call_open
        self.region = region


def lambda_body_end(cleaned: str, pairs: Dict[int, int], start: int) -> Tuple[int, int]:
    """From just after a capture list's `]`, find the lambda's body span.

    Skips the optional signature tail (`(params)`, `mutable`, `noexcept(...)`,
    `constexpr`, `-> trailing-return-type`) and returns (body_open, body_close)
    of the first `{` reached with balanced `()`/`[]` depth. A `;` at depth 0
    before any `{` means this capture list is not a lambda definition here;
    returns (start, start) so the region degenerates to the capture itself.
    """
    depth = 0
    i, n = start, len(cleaned)
    while i < n:
        ch = cleaned[i]
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
            if depth < 0:
                return (start, start)
        elif ch == "{" and depth == 0:
            close = pairs.get(i)
            if close is None:
                return (start, start)
            return (i, close)
        elif ch == ";" and depth == 0:
            return (start, start)
        i += 1
    return (start, start)


def find_capture_moves(cleaned: str, pairs: Dict[int, int]) -> List[CaptureMove]:
    """Single pass: bracket stack + interleaved regex matches."""
    matches = [(m.start(), m.group(2)) for m in RE_CAPTURE_MOVE.finditer(cleaned)]
    matches.reverse()  # pop from the end = next match in order
    hits: List[CaptureMove] = []
    stack: List[Tuple[str, int]] = []
    match_pos, moved_var = (matches.pop() if matches else (None, None))
    for pos, ch in enumerate(cleaned):
        if match_pos is not None and pos == match_pos:
            # Nearest unclosed `[` = the capture list this init-capture sits in.
            cap_idx = next((k for k in range(len(stack) - 1, -1, -1)
                            if stack[k][0] == "["), None)
            if cap_idx is not None:
                capture_open = stack[cap_idx][1]
                # Reject hits inside a lambda BODY that merely nests inside an
                # enclosing capture list: any `{` above the capture `[` means
                # the match is in code, not in the capture initializer.
                if not any(e[0] == "{" for e in stack[cap_idx + 1:]):
                    # Direct-argument paren: the entry immediately below the
                    # capture `[` must be the call's `(`; `{`/bottom = the
                    # lambda is statement-level (not a call argument).
                    direct = (stack[cap_idx - 1][1]
                              if cap_idx > 0 and stack[cap_idx - 1][0] == "("
                              else None)
                    cap_close = pairs.get(capture_open)
                    if cap_close is not None:
                        body_open, body_close = lambda_body_end(
                            cleaned, pairs, cap_close + 1)
                        region = ((capture_open, body_close)
                                  if body_close > body_open
                                  else (capture_open, cap_close))
                    else:
                        region = (capture_open, capture_open)
                    hits.append(CaptureMove(moved_var, pos, capture_open,
                                            direct, region))
            if matches:
                match_pos, moved_var = matches.pop()
            else:
                match_pos, moved_var = None, None
        if ch in OPENERS:
            stack.append((ch, pos))
        elif ch in CLOSERS:
            if stack and stack[-1][0] == MATCHING[ch]:
                stack.pop()
    return hits


def find_violations(cleaned: str) -> List[Tuple[int, int, str]]:
    """Pairs of hit positions moving the same var into two sibling lambdas
    of one call. Returns [(first_pos, second_pos, var), ...]."""
    pairs_map = build_bracket_map(cleaned)
    hits = find_capture_moves(cleaned, pairs_map)
    violations: List[Tuple[int, int, str]] = []
    by_var: Dict[str, List[CaptureMove]] = {}
    for h in hits:
        by_var.setdefault(h.moved_var, []).append(h)
    for var, group in by_var.items():
        for a in range(len(group)):
            for b in range(a + 1, len(group)):
                h1, h2 = group[a], group[b]
                if h1.direct_call_open is None or h2.direct_call_open is None:
                    continue
                if h1.direct_call_open != h2.direct_call_open:
                    continue
                r1, r2 = h1.region, h2.region
                nested = (r1[0] <= r2[0] and r2[1] <= r1[1]) or \
                         (r2[0] <= r1[0] and r1[1] <= r2[1])
                if nested:
                    continue
                violations.append((h1.pos, h2.pos, var))
    return violations


# --------------------------------------------------------------------------
# Selftest
# --------------------------------------------------------------------------

POSITIVE_A2 = """void f(Cb cb) {
    mapper.findOne(cr,
        [cb = std::move(cb)](const Row &r) { cb(r); },
        [cb = std::move(cb)](const Err &e) { fail(e); });
}
"""

NEGATIVE_NESTED = """void f(Cb cb, Plugin *p) {
    p->revokeAccessToken(token, clientId, [cb = std::move(cb)]() mutable {
        p->revokeRefreshToken(token, [cb = std::move(cb)]() mutable { done(); });
    });
}
"""

NEGATIVE_BRANCHES = """void f(Cb cb) {
    if (svc) {
        svc->validate(u, [cb = std::move(cb)](bool ok) { cb(ok); });
    } else {
        AuthService::validateUser(u, [cb = std::move(cb)](bool ok) { cb(ok); });
    }
}
"""

NEGATIVE_DIRECT_ARG = """void f(Cb cb) {
    respondError(req, std::move(cb), "oops");
}
"""

NEGATIVE_STATEMENT_LAMBDAS = """void f(Cb cb) {
    auto a = [cb = std::move(cb)]() { return 1; };
    auto b = [cb = std::move(cb)]() { return 2; };
}
"""

NEGATIVE_STRING_AND_COMMENT = """const char *s = ") ([cb = std::move(cb)](";
char c = ')';
// [cb = std::move(cb)] inside a comment
void f(Cb cb) {
    mapper.findOne(cr,
        [cb = std::move(cb)](const Row &r) { cb(r); },
        [cb = std::move(cb)](const Err &e) { fail(e); });
}
"""

NEGATIVE_DIFFERENT_VARS = """void f(Cb a, Cb b) {
    mapper.findOne(cr,
        [a = std::move(a)](const Row &r) { a(r); },
        [b = std::move(b)](const Err &e) { b(e); });
}
"""

NEGATIVE_WRAPPED_CALL = """void f(Cb cb) {
    outer([cb = std::move(cb)]() {
        inner([cb = std::move(cb)]() { done(); });
    });
}
"""


def selftest() -> int:
    cases = [
        ("A-2 same-call double capture", POSITIVE_A2, 1),
        ("nested serial chain", NEGATIVE_NESTED, 0),
        ("mutually exclusive branches", NEGATIVE_BRANCHES, 0),
        ("direct-argument move", NEGATIVE_DIRECT_ARG, 0),
        ("sequential statement lambdas", NEGATIVE_STATEMENT_LAMBDAS, 0),
        ("string/comment noise around a real hit", NEGATIVE_STRING_AND_COMMENT, 1),
        ("different moved vars", NEGATIVE_DIFFERENT_VARS, 0),
        ("wrapped-call siblings (blind spot)", NEGATIVE_WRAPPED_CALL, 0),
    ]
    failed = 0
    for name, source, expected in cases:
        got = len(find_violations(blank_literals(source)))
        status = "PASS" if got == expected else "FAIL"
        if got != expected:
            failed += 1
        print(f"  [{status}] {name}: expected {expected}, got {got}")
    if failed:
        print(f"selftest FAILED ({failed} case(s))")
        return 1
    print("selftest passed")
    return 0


# --------------------------------------------------------------------------
# Tree scan
# --------------------------------------------------------------------------

def iter_source_files(root: Path):
    """Production trees: every lib under libs/ (include/, src/) + apps/server/src."""
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


def line_of(cleaned: str, pos: int) -> int:
    return cleaned.count("\n", 0, pos) + 1


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Double-move capture guard")
    default_root = Path(__file__).resolve().parents[2]
    parser.add_argument("--root", type=Path, default=default_root,
                        help="repository root (defaults to two levels above)")
    parser.add_argument("--selftest", action="store_true",
                        help="run embedded positive/negative cases and exit")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    root = args.root.resolve()
    print("========================================")
    print("Double-move capture guard (#183, A-2 pattern)")
    print("========================================")
    print(f"root: {root}")
    print("")

    try:
        files = list(iter_source_files(root))
    except FileNotFoundError as exc:
        print(f"[ERROR] {exc}")
        return 2

    violations = 0
    for path in files:
        cleaned = blank_literals(path.read_text(encoding="utf-8", errors="replace"))
        found = find_violations(cleaned)
        if not found:
            continue
        rel = path.relative_to(root)
        for first, second, var in found:
            violations += 1
            print(f"[VIOLATION] {rel}:{line_of(cleaned, second)}")
            print(f"    '{var}' is std::move'd into two lambdas of the SAME call "
                  f"expression")
            print(f"    (capture sites at lines {line_of(cleaned, first)} and "
                  f"{line_of(cleaned, second)})")
            print(f"    whichever lambda loses the race holds an empty "
                  f"std::function and aborts when invoked.")
            print(f"    Fix: keep one owner -- "
                  f"auto sharedCb = std::make_shared<CallbackType>(std::move({var})); "
                  f"then capture [sharedCb] by value.")

    print("")
    print(f"scanned {len(files)} source files.")
    if violations:
        print(f"[ERROR] {violations} double-move violation(s) found.")
        return 1
    print("[PASS] no double-move captures detected.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
