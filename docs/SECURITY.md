# Security

The security model, implementation guarantees, input handling, known
limitations and development rules of fusing-patches.

## 1. Security model

The system is a multi-writer FUSE overlay: agents edit files through separate
mountpoints that share one sidecar patch store.

| Boundary                   | What crosses it                                   | Risk level        |
|--------------------------- |-------------------------------------------------- |------------------ |
| Agent → FUSE mount         | filenames, file contents, write ranges            | untrusted input   |
| Auditor → patch-view mount | edits, `rm` (reject), unified/git-diff patches    | untrusted input   |
| Store directory (shared)   | patch index, blobs, seq, messages, merge state    | trusted mechanism |
| Lower/origin filesystem    | reads; writes only via apply-to-origin            | trusted mechanism |
| Operator → CLI             | `lower`, `store`, mountpoint, `--name`            | trusted           |

Adversarial inputs the design accounts for: shell-metacharacter filenames,
`..`/absolute paths in git-diff headers, oversized diffs and bases,
concurrent daemons on one store, and unprivileged store readers.

## 2. Implementation guarantees

- **No shell, no fork, no external tools.** All diff/merge/patch work is
  in-process (`src/diff.c`); there is no `system()`/`popen()` and the daemon
  never forks. The static binary has no runtime dependencies beyond the kernel.
- **No command injection surface.** No user-derived string ever reaches a
  shell or `exec`; all formatting uses bounded `snprintf`.
- **Private store.** The daemon sets `umask(0077)`; store directories are
  0700 and files 0600 (patches/messages may contain file data).
- **Safe path encoding.** Store filenames are percent-encoded flat names;
  `encode_path` detects truncation (no silent name collisions). Git-diff
  header paths pass through `sanitize_relpath` (no `..`, no `.`, no absolute
  paths).
- **Symlink-safe materialization.** Apply-to-origin opens the target with
  `O_NOFOLLOW`; a symlink planted in the lower tree causes the write to fail
  instead of redirecting it.
- **Bounded inputs.** Audit diffs are capped at 16 MiB; diff bases at 64 MiB;
  the exact Myers diff is capped at 2048 total lines (coarse fallback above).
- **OOM-safe growth.** Dynamic arrays realloc via temp pointers with graceful
  degradation; the vendored hash table is patched for OOM safety
  (`patches/ht.h.patch`).
- **Atomic index updates.** Files rewritten while read (patch index after a
  merge) go through temp-file + `rename`; appends are single-line O_APPEND.
- **Concurrency discipline.** In-process state under `ps->lock`; process-local
  counters atomic; the cross-process seq under `flock`; commits synchronous
  with `close()`; no kernel dentry caching (`entry_timeout = 0`), so every
  open re-validates store state.

## 3. Input handling

| Input              | Handling                                                                                                             |
|------------------- |--------------------------------------------------------------------------------------------------------------------  |
| Filenames          | used verbatim against the lower path (the kernel supplies canonical paths); percent-encoded for all store names.     |
| File contents      | stored as opaque snapshots; bounded reads for diff operations.                                                       |
| Write ranges       | accumulated per session; only used for conflict detection (integer math on offsets).                                 |
| Git-diff patches   | headers split in-process; target paths sanitized; hunks applied in-process; unmatched hunks become conflict markers. |
| Audit diff size    | capped at 16 MiB; larger diffs are rejected.                                                                         |
| Diff bases         | capped at 64 MiB during read.                                                                                        |

## 4. Known limitations

- **Encoded name length.** Percent-encoding a path whose encoded form exceeds
  the filesystem `NAME_MAX` (255) fails loudly (ENAMETOOLONG) rather than
  colliding. A hashed or nested store-naming scheme would lift this limit.
- **Symlinked intermediate directories.** During apply-to-origin, intermediate
  directory components of the lower path are still followed; only the final
  component is `O_NOFOLLOW`. A complete fix is `openat2(RESOLVE_BENEATH)`.
- **Patch-index append atomicity.** `meta` appends rely on O_APPEND atomicity
  for single-line writes (< PIPE_BUF).
- **Same-owner last-writer-wins.** A writer that opened before its own
  previous commit landed silently overwrites it; only cross-owner range
  overlaps are soft-blocked.
- **Static build.** The static binary is glibc-static (not musl); mounting
  still requires the `fusermount3` setuid helper (kernel mount privilege),
  which is not a library dependency.

## 5. Secure-development rules

- No `system()`, `popen()`, shell construction, or `fork()`. External
  programs are not used.
- Bounded `snprintf` only; never `sprintf`/`strcpy`/`strcat`.
- `x = realloc(x, …)` is forbidden: temp pointer + checked, graceful failure.
- All mutations of `ps->sessions`/`sessions_ht` under `ps->lock`; process-local
  counters atomic; cross-process state under `flock`.
- Files rewritten while read: temp file + atomic `rename`.
- New FUSE ops: `entry_timeout = 0` semantics — never assume LOOKUP preceded
  OPEN; re-validate existence.
- New inputs (audit diffs, filenames): assume adversarial; sanitize paths and
  cap sizes.
- Run `make test-diff`, `tests/*.sh` and the stress suite (`TESTING.md`)
  before merging anything touching the commit/resolution path.
