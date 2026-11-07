#!/usr/bin/env bash
# check-doc-links.sh - Verify relative links in markdown files resolve to existing files.
#
# Usage: scripts/check-doc-links.sh [ROOT_DIR]
#   ROOT_DIR defaults to the repository root (parent of this script's directory).
#
# Exit codes:
#   0 - All relative links are valid
#   1 - One or more broken links found

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${1:-$(cd "$SCRIPT_DIR/.." && pwd)}"

broken=0
checked=0

# Use a temp file to collect broken links (avoids subshell variable loss from pipes)
tmpfile=$(mktemp)
trap 'rm -f "$tmpfile"' EXIT

# TRACKED files only: `git ls-files` keeps the check reproducible in CI and
# skips local scratch/untracked directories (the old `find` walked them).
# Fallback to find when run outside a git work tree.
mdfiles=$(mktemp)
trap 'rm -f "$tmpfile" "$mdfiles"' EXIT
if git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$ROOT_DIR" ls-files -z -- '*.md' > "$mdfiles"
else
    find "$ROOT_DIR" -name '*.md' -not -path '*/node_modules/*' -not -path '*/.git/*' -print0 > "$mdfiles"
fi

while IFS= read -r -d '' mdfile; do
    dir="$(dirname "$mdfile")"

    # Extract markdown links outside of fenced code blocks.
    # awk skips lines inside ``` fences, then grep extracts link targets.
    # Process substitution (not a pipe) keeps the loop in THIS shell so the
    # counters below actually accumulate — the old piped variant lost every
    # increment to the subshell and always reported "0 links checked".
    while IFS= read -r link; do
        # Strip anchor fragments
        link_path="${link%%#*}"

        # Skip empty (anchor-only), http(s), mailto, file:// links
        [[ -z "$link_path" ]] && continue
        [[ "$link_path" =~ ^https?:// ]] && continue
        [[ "$link_path" =~ ^mailto: ]] && continue
        [[ "$link_path" =~ ^file:// ]] && continue

        # Skip false positives from C++/code snippets that survive nested code
        # fences (e.g. lambda captures `[&x](const T& x)`). Real doc paths never
        # contain C++ scope operators, references, angle brackets, or spaces.
        [[ "$link_path" == *"::"* ]] && continue
        [[ "$link_path" == *"&"* ]] && continue
        [[ "$link_path" == *"<"* || "$link_path" == *">"* ]] && continue
        [[ "$link_path" == *" "* ]] && continue

        checked=$((checked + 1))

        # Resolve relative to the markdown file's directory (GitHub
        # semantics — the exact blind spot of the Docusaurus onBrokenLinks
        # gate, which resolves bare relative links against the docs root
        # and never looks at md files outside docs/).
        target="$dir/$link_path"

        if [[ ! -e "$target" ]]; then
            echo "BROKEN: $mdfile -> $link_path" >> "$tmpfile"
        fi
    done < <(awk '
        { sub(/\r$/, "") }
        /^[[:space:]]*```/ { in_fence = !in_fence; next }
        !in_fence { print }
    ' "$mdfile" | grep -oP '\[(?:[^\]]*)\]\(\K[^)]+' 2>/dev/null) || true
done < "$mdfiles"

broken=$(wc -l < "$tmpfile" 2>/dev/null || echo 0)
broken=$((broken + 0))  # ensure numeric

if [[ $broken -gt 0 ]]; then
    cat "$tmpfile"
    echo ""
    echo "Found $broken broken link(s)."
    exit 1
else
    echo "All doc links are valid ($checked links checked)."
    exit 0
fi
