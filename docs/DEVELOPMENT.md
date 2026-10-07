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

Requirements: a C compiler. Nothing else — the FUSE layer is implemented
in-process against the kernel protocol (`linux/fuse.h`).

Static build (no runtime dynamic dependencies):

```bash
make static           # -> build/fusing-patches-static
```

Same requirement (a C compiler); it is a plain `-static` link. The static
binary is glibc-static; its only runtime dependencies are the kernel and the
`fusermount3` setuid helper needed to mount.

Cleaning: `make clean` removes `build/`.

## Module map

| File               | Responsibility                                                                                                                                                                                                                                            |
|------              |----------------                                                                                                                                                                                                                                           |
| `src/fuse.c`       | Own FUSE layer: kernel protocol loop over `/dev/fuse` (single-threaded), mount/unmount via the `fusermount3` helper, inode table, reply framing. No libfuse.                                                                                              |
| `src/overlayfs.c`  | FUSE callbacks (business logic). Write commits happen in `flush` (synchronous with `close()`); `release` is the fallback. Owns the `/fuse/` virtual namespace and mount-kind routing.                                                                      |
| `src/patchstore.c` | Everything durable: global seq (flock), patch index (`meta/`), byte ranges (`ranges/`), merged snapshots (`merge/`), per-mountpoint state (`mounts/`), messages (`messages/`), origin snapshots (`origin/`), apply-to-origin, git-diff splitting + audit. |
| `src/diff.c`       | `diff_unified` (Myers), `merge3` (git-style 3-way), `apply_unified_diff` (hunk applier with offset search + conflict markers). No external tools.                                                                                                         |
| `src/ht.h`         | Hash table used for session indexing, inode table and path dedup (murmur3-hashed string keys).                                                                                                                                                            |
| `src/hash.h`       | murmur3 32-bit + C-string hasheq, shared by the modules.                                                                                                                                                                                                 |

## Code conventions

These are enforced by review, not tooling:

- No `system()`, `popen()`, or shell construction anywhere. `fork()` is used
  only to run the `fusermount3` mount helper at mount/unmount time. All
  formatting via bounded `snprintf`.
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

- murmur3 32-bit (public domain) is reimplemented in `src/hash.h`
  (`murmur3_32`, byte-wise, alignment-safe).
- The kernel UAPI header `linux/fuse.h` is used under its BSD-2-Clause option.

No LGPL/GPL code is used or linked — the project is MIT-licensed (`LICENSE`).

## Debugging

- Run the daemon in the foreground (`-f`); add debug prints in `src/fuse.c`'s
  dispatcher for protocol-level tracing.
- Conflict messages go to the daemon's stderr and to `/fuse/messages/` in the
  mount — see `USAGE.md`.
- The store directory is fully inspectable while mounts are live
  (`meta/`, `merge/`, `messages/`, `origin/`, `mounts/active/`).
