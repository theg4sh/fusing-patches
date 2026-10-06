#!/usr/bin/env bash
# Sustained stress: hammer the mounts with high-frequency concurrent writes for
# a configurable duration and watch for stability problems (crashes, hangs,
# corruption, unexpected write failures).
#
# usage: stress_long.sh [duration_seconds]

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${BIN:-$HERE/build/overlayfs}"
DURATION="${1:-120}"

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

PIDS=()
for i in 0 1 2 3; do
    M=$(mktemp -d /tmp/sm.XXXXXX)
    MOUNTS+=("$M")
    "$BIN" "$LOWER" "$STORE" "$M" --name="agent$i" -f 2>/tmp/stress-agent$i.log &
    PIDS+=($!)
done
sleep 1.5

python3 - "$DURATION" "${MOUNTS[@]}" <<'PY'
import sys, threading, time, os
duration = float(sys.argv[1])
mounts = sys.argv[2:6]

stop = threading.Event()
stats = {"writes": 0, "errors": 0, "last": ""}

def agent(idx):
    m = mounts[idx]
    k = 0
    while not stop.is_set():
        # cycle across 20 disjoint files
        p = "/h%d-%d.txt" % (idx, k % 20)
        try:
            with open(m + p, "w") as fh:
                fh.write("agent%d iter%d @%.3f\n" % (idx, k, time.time()))
            stats["writes"] += 1
        except Exception as e:
            stats["errors"] += 1
            stats["last"] = "agent%d: %s" % (idx, e)
        k += 1

ts = [threading.Thread(target=agent, args=(i,)) for i in range(4)]
for t in ts: t.start()

t0 = time.time()
while time.time() - t0 < duration:
    time.sleep(10)
    el = time.time() - t0
    print("  ... %4.0fs  writes=%d  errors=%d" % (el, stats["writes"], stats["errors"]), flush=True)

stop.set()
for t in ts: t.join()
el = time.time() - t0
print("RESULT writes=%d errors=%d elapsed=%.1fs rate=%.0f w/s" %
      (stats["writes"], stats["errors"], el, stats["writes"] / el))
if stats["last"]:
    print("LAST_ERROR: %s" % stats["last"])
PY

# integrity check: each mount still responsive, sample content sane
sleep 0.3
for i in 0 1 2 3; do
    if kill -0 "${PIDS[$i]}" 2>/dev/null; then
        echo "agent$i alive, sample: $(cat "${MOUNTS[$i]}/h$i-0.txt" 2>&1 | head -1)"
    else
        echo "agent$i PROCESS DIED"
    fi
done
echo "lower seed: $(cat "$LOWER/seed.txt")"
