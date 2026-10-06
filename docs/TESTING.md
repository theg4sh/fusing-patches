# Testing

Unit, integration and stress testing of the tool.

## Quick reference

```bash
make test-diff                          # diff/merge/applier unit tests
tests/test_merge.sh                     # soft-block / conflict message / dedup
tests/test_lifecycle.sh                 # apply-to-origin / multi-level / patch-view / git-diff
tests/test_origin.sh                    # external-origin-change 3-way conflicts
make stress-test                        # C stress, burst mode (~0.6 s)
make stress-long STRESS_DURATION=120    # C stress, sustained mode
```

All scripts accept a `BIN` override to test another build:

```bash
BIN="$PWD/build/fusing-patches-static" tests/test_merge.sh
```

## Unit tests — `make test-diff`

`tests/difftest.c` links `src/diff.c` and covers the in-process engine
(`tests/test_diff.sh` is a thin wrapper):

- `diff_unified`: identical, modify, insert, delete, no-newline marker,
  fully-different files, labels, hunk headers.
- `merge3`: clean non-overlapping merge, overlapping conflict (markers),
  identical base, EOF insert conflict, one-sided change.
- `apply_unified_diff`: clean modify, EOF insertion, deletion, offset search
  (context mismatch at the claimed position), unmatched-hunk conflict
  markers, multi-hunk, no-trailing-newline base + `\ No newline` marker.

## Integration tests

| Script                    | What it covers                                                                                                                                                                                                                                                                 |
|--------                   |----------------                                                                                                                                                                                                                                                                |
| `tests/test_merge.sh`     | passthrough read; clean sequential edit; soft-block on overlapping write (content unchanged, message on stderr + `/fuse/messages/`); conflict-message de-dup; re-sync + retry accepted; disjoint-range ("small piece") acceptance; `/fuse/merge/` resolution; lower untouched. |
| `tests/test_lifecycle.sh` | apply-to-origin on last unmount; two-level stack (`origin → m1 → m2`) with staged materialization; patch-view edit + `rm` (reject); git-diff applied via `/fuse/audit/`; rejected file not applied.                                                                            |
| `tests/test_origin.sh`    | external origin change: normal apply when unchanged; 3-way conflict diff (markers) + no clobber when the lower changed behind the stack.                                                                                                                                       |

Each script prints `PASS:`/`FAIL:` lines and exits non-zero on any failure.

## Stress tests — native C harness

The stress is a standalone C program (`stress/stress.c` → `build/stress`) that
mounts the agents itself (no shell orchestration) and hammers them with real
pthread concurrency:

```
stress <fusing-patches-binary> [options]
  --duration=SECS   sustained mode (default: burst)
  --agents=A        overlay mountpoints to drive   (default 4)
  --threads=T       worker threads per agent       (default 2)
  --files=F         disjoint files per thread      (default 20)
  --iters=N         burst: iterations per file     (default 20)
  --noop=N          thread 0 also does N identical rewrites
  --shared          add shared-file contention (soft-blocks expected)
  --verify          verify final content
```

What it asserts:

- **0 errors** on the disjoint-file workload (any write failure is a bug).
- **no-op acceptance**: N identical rewrites produce exactly one patch
  (`--noop=200` is part of `make stress-test`).
- content correctness + agents alive after the run.

`make stress-test` runs the burst; `make stress-long STRESS_DURATION=120`
runs the sustained stability watch (the gate for changes touching the
commit/resolution path). Expected: `errors=0` and, under `--shared`,
`blocks>0` is fine (soft-blocks are the designed behavior).

## Conventions for new tests

- Scripts: `set -u`, temp dirs via `mktemp -d`, `trap cleanup EXIT`,
  unmount then `wait` for the daemon PIDs before `rm -rf` (apply-to-origin
  runs during teardown).
- Follow the `PASS/FAIL` helper style of the existing scripts.
- Anything touching `src/diff.c` gets a case in `tests/difftest.c`.
- Timing: commits are synchronous with `close()` (see ARCHITECTURE §10), so
  no sleeps are needed between a write and its effect on *that* mount;
  cross-mount and unmount effects may still need a short settle.
