#!/bin/sh
# PostToolUse hook: block Edit/Write/MultiEdit results that leak local-
# environment references (absolute paths, non-vanilla plugin filenames).
#
# Rule source: project AGENTS.md / CLAUDE.md
#   NEVER reference or hard-code any local file path. Neither in the code
#   nor in any comment. NEVER share any information about the local
#   development machine, installed mods or any other installed software
#   or configuration.

set -u

payload=$(cat)

# Extract the first "file_path" value from the JSON payload without jq.
file_path=$(printf '%s' "$payload" | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)
[ -z "$file_path" ] && exit 0

# Limit the check to source/doc files; skip third-party libs.
case "$file_path" in
	*.cpp|*.h|*.hpp|*.cc|*.md|*.txt) ;;
	*) exit 0 ;;
esac
case "$file_path" in
	*/lib/*) exit 0 ;;
esac

[ -f "$file_path" ] || exit 0

violations=""

# Absolute paths — Unix prefixes and Windows drive letters.
abs_hits=$(grep -nE '/home/|/Users/|/Volumes/|/mnt/|/tmp/|/var/|[A-Za-z]:\\' "$file_path" 2>/dev/null | head -n 10)
if [ -n "$abs_hits" ]; then
	violations="${violations}Absolute paths found:
$abs_hits
"
fi

# Plugin filenames (.esp/.esm/.esl) excluding the five vanilla Bethesda masters.
plugin_hits=$(grep -nE '[A-Za-z0-9_.-]+\.(esp|esm|esl)\b' "$file_path" 2>/dev/null \
	| grep -viE '\b(skyrim|update|dawnguard|hearthfires|dragonborn)\.esm\b' \
	| head -n 10)
if [ -n "$plugin_hits" ]; then
	violations="${violations}Non-vanilla plugin references found:
$plugin_hits
"
fi

if [ -n "$violations" ]; then
	printf 'check-local-refs: FAIL for %s\n' "$file_path" >&2
	printf 'AGENTS.md / CLAUDE.md rule: NEVER reference or hard-code any local file path or installed mod / software / configuration.\n' >&2
	printf '%s' "$violations" >&2
	exit 2
fi
exit 0
