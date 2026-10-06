# Development

Build, project layout, code conventions and vendored dependencies.

## Project layout

```
src/            production code (C)
  overlayfs.c     FUSE layer: ops, virtual /fuse/ namespace, mount kinds
  patchstore.c    patch store: seq, patches, ranges, merge, messages, apply-to-origin
  diff.c/.h       in-process engine: unified diff, 3-way merge, unified-diff applier
  ht.h            vendored hash table (tsoding/ht.h, patched — see patches/)
stress/         native C stress harness (stress.c)
tests/          unit + integration tests (shell + C)
docs/           ARCHITECTURE.md, SECURITY.md, DEVELOPMENT.md, USAGE.md, TESTING.md
patches/        ht.h.patch — upstream fix for the vendored hash table
build/          build artifacts (gitignored)
Makefile        all / static / test-diff / stress-test / stress-long / clean
LICENSE         MIT (vendored code: ht.h MIT/Public Domain, murmur3 public domain)
```

## Building

Dynamic (development) build:

```bash
make                  # -> build/fusing-patches (+ build/stress, build/difftest)
```

Requirements: C compiler, `pkg-config`, libfuse3 development headers.

Static build (no runtime dynamic dependencies):

```bash
make static           # -> build/fusing-patches-static
```

Requirements: network access (downloads the libfuse3 source tarball once into
`build/fuse-<ver>/`) and `meson`/`ninja` (builds libfuse3 as a static archive).
The static binary is glibc-static; it has no runtime dependencies besides the
kernel and the `fusermount3` setuid helper needed to mount.

Cleaning: `make clean` removes `build/` (including the downloaded libfuse;
`make static` re-fetches it).

## Module map

| File               | Responsibility                                                                                                                                                                                                                                            |
|------              |----------------                                                                                                                                                                                                                                           |
| `src/overlayfs.c`  | All FUSE callbacks. Write commits happen in `flush` (synchronous with `close()`); `release` is the fallback. Owns the `/fuse/` virtual namespace and mount-kind routing.                                                                                  |
| `src/patchstore.c` | Everything durable: global seq (flock), patch index (`meta/`), byte ranges (`ranges/`), merged snapshots (`merge/`), per-mountpoint state (`mounts/`), messages (`messages/`), origin snapshots (`origin/`), apply-to-origin, git-diff splitting + audit. |
| `src/diff.c`       | `diff_unified` (Myers), `merge3` (git-style 3-way), `apply_unified_diff` (hunk applier with offset search + conflict markers). No external tools.                                                                                                         |
| `src/ht.h`         | Hash table used for session indexing and path dedup (murmur3-hashed string keys).                                                                                                                                                                         |

## Code conventions

These are enforced by review, not tooling:

- No `system()`, `popen()`, or shell construction anywhere; no `fork()` (the
  daemon is single-process). All formatting via bounded `snprintf`.
- `x = realloc(x, …)` is forbidden: use a temp pointer and handle failure.
  On OOM prefer graceful degradation (partial parse, skipped entry) over abort.
- All mutations of `ps->sessions` / `sessions_ht` under `ps->lock`; process-local
  counters atomic (`__atomic_*`); cross-process state under `flock`.
- Files rewritten while others read them: temp file + atomic `rename`.
- Store files are private: the daemon sets `umask(0077)`; dirs/files 0700/0600.
- New FUSE ops: `entry_timeout = 0` semantics (no dentry caching) — never
  assume LOOKUP preceded OPEN; re-validate existence.
- New inputs (audit diffs, filenames): assume adversarial; sanitize paths
  (`sanitize_relpath`) and cap sizes (`SLURP_MAX`, 16 MiB audit cap).
- Run `make test-diff && tests/*.sh && make stress-test` (and a
  `make stress-long STRESS_DURATION=120` before releases) after touching the
  commit/resolution path. See `TESTING.md`.

## Vendored code

- `src/ht.h` — tsoding/ht.h v1.1.0 (MIT/Public Domain). It carries project
  fixes (OOM-safe expansion, hash clamp, tombstone reuse, overflow guards).
  The fix is tracked as a patch against pristine upstream:

  ```bash
  # after editing src/ht.h, regenerate the patch:
  diff -u --label ht.h.orig --label ht.h <pristine-ht.h> src/ht.h > patches/ht.h.patch
  # verify it applies to pristine upstream:
  cp <pristine-ht.h> /tmp/ht.h && (cd /tmp && patch -s -p0 < patches/ht.h.patch)
  ```

- murmur3 32-bit (public domain) is reimplemented in `src/patchstore.c`
  (`murmur3_32`, byte-wise, alignment-safe).

No LGPL/GPL code is used or linked — the project is MIT-licensed (`LICENSE`).

## Debugging

- Run the daemon with `-d` (libfuse request trace) and `-f` (foreground).
- Conflict messages go to the daemon's stderr and to `/fuse/messages/` in the
  mount — see `USAGE.md`.
- The store directory is fully inspectable while mounts are live
  (`meta/`, `merge/`, `messages/`, `origin/`, `mounts/active/`).
