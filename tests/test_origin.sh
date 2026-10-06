#!/usr/bin/env bash
# origin-change conflict: detect external changes to the lower fs that happen
# behind our overlay stack, and emit a 3-way diff-conflict instead of clobbering.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${BIN:-$HERE/../build/fusing-patches}"

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; echo "       expected: $(printf '%q' "$2")"; echo "       actual:   $(printf '%q' "$3")"; }
check_eq() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "$2" "$3"; fi; }

T=$(mktemp -d /tmp/oc.XXXXXX)
mkdir -p "$T/origin" "$T/store" "$T/mnt"
LOG="$T/writer.log"

cleanup() {
    fusermount3 -u "$T/mnt" 2>/dev/null || fusermount -u "$T/mnt" 2>/dev/null || true
    wait $P1 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT

printf 'line1\nline2\nline3\n' > "$T/origin/a.txt"

# --- no external change: apply normally --------------------------------
"$BIN" "$T/origin" "$T/store" "$T/mnt" --name=alice -f 2>/dev/null & P1=$!
sleep 1
printf 'line1\nCHANGED\nline3\n' > "$T/mnt/a.txt"
sleep 0.3
fusermount3 -u "$T/mnt" 2>/dev/null; wait $P1 2>/dev/null
sleep 0.6
check_eq "no external change: patch applied to origin" "line1
CHANGED
line3" "$(cat "$T/origin/a.txt")"

# --- external change behind the stack: report conflict, don't clobber ---
printf 'line1\nline2\nline3\n' > "$T/origin/b.txt"
"$BIN" "$T/origin" "$T/store" "$T/mnt" --name=bob -f 2>"$LOG" & P1=$!
sleep 1
printf 'line1\nOURS\nline3\n' > "$T/mnt/b.txt"   # our patch (snapshots origin)
sleep 0.3
printf 'line1\nEXTERNAL\nline3\n' > "$T/origin/b.txt"   # external change to origin
sleep 0.3
fusermount3 -u "$T/mnt" 2>/dev/null; wait $P1 2>/dev/null
sleep 0.7

check_eq "external change: origin NOT clobbered" "line1
EXTERNAL
line3" "$(cat "$T/origin/b.txt")"
if grep -q "ORIGIN CONFLICT" "$LOG"; then pass "conflict reported on stderr"; else fail "conflict reported on stderr" "<ORIGIN CONFLICT>" "$(cat "$LOG")"; fi
if grep -q "<<<<<<<" "$LOG"; then pass "3-way conflict markers present"; else fail "3-way conflict markers present" "<markers>" "$(cat "$LOG")"; fi

echo
echo "== results: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
