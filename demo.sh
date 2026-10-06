#!/usr/bin/env bash
# Quick visual demo of the multi-writer merge workflow.
# Run ./test_merge.sh for the exhaustive test suite.

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${BIN:-$HERE/build/overlayfs}"

LOWER=$(mktemp -d /tmp/ol.XXXXXX)
STORE=$(mktemp -d /tmp/os.XXXXXX)
MA=$(mktemp -d /tmp/ma.XXXXXX)
MB=$(mktemp -d /tmp/mb.XXXXXX)
MM=$(mktemp -d /tmp/mm.XXXXXX)

cleanup() {
    for m in "$MA" "$MB" "$MM"; do fusermount3 -u "$m" 2>/dev/null || fusermount -u "$m" 2>/dev/null || true; done
    rm -rf "$LOWER" "$STORE" "$MA" "$MB" "$MM"
}
trap cleanup EXIT

printf 'hello world\nline two\n' > "$LOWER/notes.txt"

"$BIN" "$LOWER" "$STORE" "$MA" --name=alice -f 2>/dev/null & PA=$!
"$BIN" "$LOWER" "$STORE" "$MB" --name=bob   -f 2>&1 | tee /tmp/ol-bob-demo.log & PB=$!
"$BIN" "$LOWER" "$STORE" "$MM" --merge      -f 2>/dev/null & PM=$!
sleep 1.2

echo "== initial (passthrough) =="; cat "$MA/notes.txt"

echo; echo "== bob opens (stale base), alice commits, bob writes -> conflict =="
exec 3>"$MB/notes.txt"
printf 'alice changed line two\n' > "$MA/notes.txt"
printf 'bob changed line one\n' >&3
exec 3>&-
sleep 0.4

echo; echo "== bob's write was soft-blocked: content unchanged =="; cat "$MB/notes.txt"
echo; echo "== conflict message (bob's /fuse/messages/) =="
cat "$MB/fuse/messages/"*.msg

echo; echo "== bob resolves by writing the agreement to /fuse/merge/ =="
printf 'bob + alice resolved\n' > "$MB/fuse/merge/notes.txt"
sleep 0.4

echo; echo "== merge view (final agreed result) =="; cat "$MM/notes.txt"
echo "== lower fs untouched =="; cat "$LOWER/notes.txt"

kill $PA $PB $PM 2>/dev/null || true
