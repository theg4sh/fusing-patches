#!/usr/bin/env bash
# Lifecycle tests: multi-level mounts, apply-to-origin on last unmount,
# patch-view (edit/reject), and git-diff audit application.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${BIN:-$HERE/../build/fusing-patches}"

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; echo "       expected: $(printf '%q' "$2")"; echo "       actual:   $(printf '%q' "$3")"; }
check_eq() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "$2" "$3"; fi; }

T=$(mktemp -d /tmp/lc.XXXXXX)
mkdir -p "$T/origin" "$T/store1" "$T/store2" "$T/m1" "$T/m2" "$T/mp" "$T/mm"

cleanup() {
    for m in "$T/m1" "$T/m2" "$T/mp" "$T/mm"; do fusermount3 -u "$m" 2>/dev/null || fusermount -u "$m" 2>/dev/null || true; done
    rm -rf "$T"
}
trap cleanup EXIT

# ==========================================================================
# 1. apply-to-origin on last unmount (single level)
# ==========================================================================
printf 'orig\n' > "$T/origin/a.txt"
"$BIN" "$T/origin" "$T/store1" "$T/m1" --name=alice -f 2>/dev/null & P1=$!
sleep 1
printf 'alice\n' > "$T/m1/a.txt"
sleep 0.3
check_eq "1. write visible in mount" "alice" "$(cat "$T/m1/a.txt")"
check_eq "1. origin unchanged before unmount" "orig" "$(cat "$T/origin/a.txt")"
fusermount3 -u "$T/m1" 2>/dev/null || fusermount -u "$T/m1"; wait $P1 2>/dev/null
sleep 0.5
check_eq "1. origin applied after last unmount" "alice" "$(cat "$T/origin/a.txt")"

# ==========================================================================
# 2. multi-level: origin -> m1 -> m2
# ==========================================================================
printf 'base\n' > "$T/origin/b.txt"
"$BIN" "$T/origin" "$T/store1" "$T/m1" --name=l1 -f 2>/dev/null & P1=$!
sleep 1
"$BIN" "$T/m1" "$T/store2" "$T/m2" --name=l2 -f 2>/dev/null & P2=$!
sleep 1
check_eq "2. passthrough through 2 layers" "base" "$(cat "$T/m2/b.txt")"
printf 'l2-edit\n' > "$T/m2/b.txt"
sleep 0.3
check_eq "2. m2 sees its own edit" "l2-edit" "$(cat "$T/m2/b.txt")"
check_eq "2. m1 unaffected" "base" "$(cat "$T/m1/b.txt")"

# unmount m2 -> applies to m1 (its lower) -> m1 gets a patch
fusermount3 -u "$T/m2" 2>/dev/null || fusermount -u "$T/m2"; wait $P2 2>/dev/null
sleep 0.7
check_eq "2. m2 unmount applied into m1" "l2-edit" "$(cat "$T/m1/b.txt")"
check_eq "2. origin still unchanged (m1 not unmounted)" "base" "$(cat "$T/origin/b.txt")"

# unmount m1 -> applies to origin
fusermount3 -u "$T/m1" 2>/dev/null || fusermount -u "$T/m1"; wait $P1 2>/dev/null
sleep 0.7
check_eq "2. m1 unmount applied into origin" "l2-edit" "$(cat "$T/origin/b.txt")"

# ==========================================================================
# 3. patch-view: edit (update) + rm (reject)
# ==========================================================================
printf 'c1\nc2\nc3\n' > "$T/origin/c.txt"
"$BIN" "$T/origin" "$T/store1" "$T/m1" --name=writer -f 2>/dev/null & P1=$!
sleep 1
"$BIN" "$T/origin" "$T/store1" "$T/mp" --patch-view -f 2>/dev/null & PP=$!
sleep 1

# writer changes c.txt, then auditor updates + rejects in patch-view
printf 'c1\nc2-modified\nc3\n' > "$T/m1/c.txt"   # writer's patch
sleep 0.3
printf 'c1\nc2-FINAL\nc3\n' > "$T/mp/c.txt"      # auditor updates merged state
sleep 0.3
check_eq "3. patch-view update visible" "c1
c2-FINAL
c3" "$(cat "$T/mp/c.txt")"

printf 'd1\nd2\n' > "$T/origin/d.txt"
printf 'd1\nX\nd2\n' > "$T/m1/d.txt"             # writer patch on d.txt
sleep 0.3
rm "$T/mp/d.txt"                                 # auditor rejects d.txt patch
sleep 0.3
check_eq "3. rejected file reverts to origin" "d1
d2" "$(cat "$T/mp/d.txt")"

# unmount both -> apply to origin: c.txt updated, d.txt rejected (unchanged)
fusermount3 -u "$T/m1" 2>/dev/null; wait $P1 2>/dev/null
fusermount3 -u "$T/mp" 2>/dev/null; wait $PP 2>/dev/null
sleep 0.8
check_eq "3. origin: c.txt applied" "c1
c2-FINAL
c3" "$(cat "$T/origin/c.txt")"
check_eq "3. origin: d.txt rejected (unchanged)" "d1
d2" "$(cat "$T/origin/d.txt")"

# ==========================================================================
# 4. git-diff application via /fuse/audit/
# ==========================================================================
printf 'line1\nline2\nline3\n' > "$T/origin/e.txt"
"$BIN" "$T/origin" "$T/store1" "$T/mp" --patch-view -f 2>/dev/null & PP=$!
sleep 1
cat > "$T/mp/fuse/audit/change.patch" <<'EOF'
diff --git a/e.txt b/e.txt
--- a/e.txt
+++ b/e.txt
@@ -1,3 +1,3 @@
 line1
-line2
+LINE2-CHANGED
 line3
EOF
sleep 0.5
check_eq "4. git-diff applied via /fuse/audit/" "line1
LINE2-CHANGED
line3" "$(cat "$T/mp/e.txt")"
fusermount3 -u "$T/mp" 2>/dev/null; wait $PP 2>/dev/null
sleep 0.6
check_eq "4. git-diff applied to origin on unmount" "line1
LINE2-CHANGED
line3" "$(cat "$T/origin/e.txt")"

echo
echo "== results: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
