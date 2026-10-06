#!/usr/bin/env bash
# Stress test: high-frequency writes into different places across several
# mountpoints sharing one store. Also verifies that writes which do not change
# the final content ("no-op" patches) are accepted but do not create patches.

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${BIN:-$HERE/build/overlayfs}"

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; echo "       expected: $(printf '%q' "$2")"; echo "       actual:   $(printf '%q' "$3")"; }
check_eq() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "$2" "$3"; fi; }
check_le() { if [ "$2" -le "$3" ]; then pass "$1"; else fail "$1" "<=$3" "$2"; fi; }

LOWER=$(mktemp -d /tmp/ol.XXXXXX)
STORE=$(mktemp -d /tmp/os.XXXXXX)
MOUNTS=()

cleanup() {
    for m in "${MOUNTS[@]}"; do fusermount3 -u "$m" 2>/dev/null || fusermount -u "$m" 2>/dev/null || true; done
    rm -rf "$LOWER" "$STORE" "${MOUNTS[@]}"
}
trap cleanup EXIT

mkdir -p "$LOWER"
printf 'seed\n' > "$LOWER/seed.txt"

for i in 0 1 2 3; do
    M=$(mktemp -d /tmp/sm.XXXXXX)
    MOUNTS+=("$M")
    "$BIN" "$LOWER" "$STORE" "$M" --name="agent$i" -f 2>/dev/null &
    PIDS+=($!)
done
sleep 1.5

# ---- concurrent high-frequency writes to disjoint files -------------------
python3 - "${MOUNTS[@]}" <<'PY'
import sys, threading, time
mounts = sys.argv[1:5]
N_FILES, N_ITERS = 20, 20
results = [None] * 4

def worker(idx):
    m = mounts[idx]
    n = 0
    for f in range(N_FILES):
        p = "/stress-%d-%d.txt" % (idx, f)
        for k in range(N_ITERS):
            with open(m + p, "w") as fh:
                fh.write("agent%d file%d iter%d\n" % (idx, f, k))
            n += 1
    results[idx] = n

ts = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
t0 = time.time()
for t in ts: t.start()
for t in ts: t.join()
elapsed = time.time() - t0
print("stress writes=%d elapsed=%.2fs rate=%.0f w/s" %
      (sum(results), elapsed, sum(results) / elapsed))
PY

sleep 0.3
check_eq "stress file content correct" "agent0 file5 iter19" "$(cat "${MOUNTS[0]}/stress-0-5.txt")"
check_eq "stress file content correct (agent3)" "agent3 file12 iter19" "$(cat "${MOUNTS[3]}/stress-3-12.txt")"

# ---- no-op writes: accepted but not stored as patches ---------------------
python3 - "${MOUNTS[0]}" <<'PY'
import sys, time
m = sys.argv[1]
p = "/noop.txt"
with open(m + p, "w") as fh:      # 1 real patch (new content)
    fh.write("constant content\n")
time.sleep(0.5)                   # let that patch commit before rewriting
for _ in range(200):              # 200 no-op rewrites of identical content
    with open(m + p, "w") as fh:
        fh.write("constant content\n")
PY
sleep 0.3
check_eq "no-op content correct" "constant content" "$(cat "${MOUNTS[0]}/noop.txt")"

# the noop.txt meta index must have exactly one patch (the 200 rewrites skipped)
NOOP_META=$(ls "$STORE/meta/" | grep -i 'noop' | head -1)
if [ -n "$NOOP_META" ]; then
    LINES=$(wc -l < "$STORE/meta/$NOOP_META")
    check_eq "no-op rewrites produced exactly 1 patch" "1" "$LINES"
else
    fail "noop.txt meta exists" "<exists>" ""
fi

# ---- total patch count is bounded (no no-op spam) -------------------------
BLOBS=$(ls "$STORE/blobs/" | wc -l)
# 4 agents * 20 files * 20 iters = 1600 real patches + 1 noop patch = 1601
check_le "total patches bounded (<=1610)" "$BLOBS" 1610

# ---- mounts still responsive after stress ---------------------------------
check_eq "mount responsive after stress" "agent0 file0 iter19" "$(cat "${MOUNTS[0]}/stress-0-0.txt")"

# ---- lower fs untouched ----------------------------------------------------
check_eq "lower untouched" "seed" "$(cat "$LOWER/seed.txt")"

echo
echo "== results: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
