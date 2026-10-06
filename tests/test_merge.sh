#!/usr/bin/env bash
# Extended tests: patch-file workflow with range-based soft-blocking.
#
# Writers apply patches as whole-file rewrites through separate mountpoints.
# A write whose byte-ranges overlap another writer's (unseen) change is
# soft-blocked (rejected) and reported on stderr + /fuse/messages/, de-dup'd
# so a repeated/identical conflict is not re-reported. Disjoint (non-
# overlapping) ranges are accepted as small pieces.

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${BIN:-$HERE/../build/fusing-patches}"

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; echo "       expected: $(printf '%q' "$2")"; echo "       actual:   $(printf '%q' "$3")"; }
check_eq() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "$2" "$3"; fi; }
check_contains() { if printf '%s' "$3" | grep -qF -- "$2"; then pass "$1"; else fail "$1" "<contains $2>" "$3"; fi; }

LOWER=$(mktemp -d /tmp/ol.XXXXXX)
STORE=$(mktemp -d /tmp/os.XXXXXX)
MA=$(mktemp -d /tmp/ma.XXXXXX)
MB=$(mktemp -d /tmp/mb.XXXXXX)
MM=$(mktemp -d /tmp/mm.XXXXXX)

cleanup() {
    for m in "$MA" "$MB" "$MM"; do fusermount3 -u "$m" 2>/dev/null || fusermount -u "$m" 2>/dev/null || true; done
    wait $PA $PB $PM 2>/dev/null || true
    rm -rf "$LOWER" "$STORE" "$MA" "$MB" "$MM" /tmp/ol-alice.log /tmp/ol-bob.log
}
trap cleanup EXIT

mkdir -p "$LOWER"
printf 'hello world\nline two\n' > "$LOWER/notes.txt"

"$BIN" "$LOWER" "$STORE" "$MA" --name=alice -f 2>/tmp/ol-alice.log & PA=$!
"$BIN" "$LOWER" "$STORE" "$MB" --name=bob   -f 2>/tmp/ol-bob.log   & PB=$!
"$BIN" "$LOWER" "$STORE" "$MM" --merge      -f 2>/dev/null          & PM=$!
sleep 1.5

# 1. passthrough ---------------------------------------------------------
check_eq "passthrough read" "hello world
line two" "$(cat "$MA/notes.txt")"

# 2. clean sequential edit ----------------------------------------------
printf 'alice-edit\n' > "$MA/notes.txt"
check_eq "clean edit accepted" "alice-edit" "$(cat "$MA/notes.txt")"

# 3. conflict -> soft-block + message -----------------------------------
exec 3>"$MB/notes.txt"                 # bob opens (base = alice-edit)
printf 'alice-v2\n' > "$MA/notes.txt"  # alice commits again
printf 'bob-v2\n' >&3                  # bob writes overlapping range
exec 3>&-                              # bob close -> soft-block
sleep 0.4

check_eq "conflicted write blocked (content unchanged)" "alice-v2" "$(cat "$MA/notes.txt")"
MSGFILE=$(ls "$MB/fuse/messages/" 2>/dev/null | head -1)
[ -n "$MSGFILE" ] && pass "conflict message in /fuse/messages/" || fail "conflict message in /fuse/messages/" "<any>" ""
MSG=$(cat "$MB/fuse/messages/$MSGFILE" 2>/dev/null)
check_contains "message diff shows other writer's change" "alice-v2" "$MSG"
check_contains "message on stderr" "soft-blocked" "$(cat /tmp/ol-bob.log)"

# 4. dedup: same conflict not re-reported -------------------------------
CNT_BEFORE=$(ls "$MB/fuse/messages/" 2>/dev/null | wc -l)
exec 3>"$MB/notes.txt"; printf 'bob-v2\n' >&3; exec 3>&-
sleep 0.4
CNT_AFTER=$(ls "$MB/fuse/messages/" 2>/dev/null | wc -l)
check_eq "identical conflict not re-reported (dedup)" "$CNT_BEFORE" "$CNT_AFTER"

# 5. re-sync + retry -> accepted ----------------------------------------
printf 'bob-v3 (rebased)\n' > "$MB/notes.txt"
sleep 0.3
check_eq "post re-sync write accepted" "bob-v3 (rebased)" "$(cat "$MB/notes.txt")"

# 6. disjoint ranges -> accepted (small pieces) --------------------------
printf '0123456789abcdefghij' > "$LOWER/data.bin"
python3 - "$MA" "$MB" <<'PY'
import os, sys
ma, mb = sys.argv[1], sys.argv[2]
p = "/data.bin"
fd = os.open(mb + p, os.O_RDWR)          # bob holds a stale base
with open(ma + p, "r+b") as f:           # alice writes [0,5)
    f.seek(0); f.write(b"AAAAA")
os.pwrite(fd, b"BBBBB", 10)              # bob writes [10,15)  (disjoint)
os.close(fd)
PY
sleep 0.4
DISJOINT_MSGS=$(ls "$MB/fuse/messages/" 2>/dev/null | wc -l)
check_eq "disjoint ranges accepted (no new conflict)" "$CNT_AFTER" "$DISJOINT_MSGS"

# 7. merge resolution ----------------------------------------------------
printf 'resolved content\n' > "$MB/fuse/merge/notes.txt"
sleep 0.3
check_eq "merge view shows resolved" "resolved content" "$(cat "$MM/notes.txt")"

# 8. lower untouched -----------------------------------------------------
check_eq "lower fs untouched" "hello world
line two" "$(cat "$LOWER/notes.txt")"

echo
echo "== results: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
