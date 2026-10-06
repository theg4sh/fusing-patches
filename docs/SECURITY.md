# Security — audit findings, hardening and rules

This document records the security review of the fusing-patches overlay,
the issues found and fixed, the remaining attack surface, and the rules the
codebase must follow going forward.

---

## 1. Threat model

The system is a multi-writer FUSE overlay: several agents edit files through
separate mountpoints that share one sidecar patch store.

Trust boundaries:

| Boundary | What crosses it | Risk level |
|----------|-----------------|------------|
| Agent -> FUSE mount | filenames, file contents, write ranges | high (untrusted input) |
| Auditor -> patch-view mount | edits, `rm` (reject), unified/git-diff patches | high (untrusted input) |
| Store directory (shared, cross-process) | patch index, blobs, seq counter, messages | high (all daemons read/write it) |
| Lower/origin filesystem | reads; writes only on apply-to-origin | medium |
| Operator -> CLI | `lower`, `store`, mountpoint, `--name` | low (operator-controlled) |

Adversarial inputs of interest:

- **Filenames** with shell metacharacters (`'`, `;`, `$()`), `..`, `%`,
  or paths whose percent-encoding exceeds the fixed buffers.
- **Git-diff content** supplied to `/fuse/audit/` (attacker-controlled text).
- **Concurrent daemons** on one store: torn reads, races, out-of-order commits.
- **Unprivileged readers** of the store (messages/blobs are mode 0644).

## 2. Fixed issues (security review)

| # | Issue | Severity | Fix |
|---|-------|----------|-----|
| 1 | Command injection: `diff`/`diff3`/`patch` were invoked via `system()`/`popen()` with paths derived from FUSE filenames — a filename containing `'` escaped into the shell | high | All external tools now run via `fork`+`execvp` with an argument array (`run_prog()`); stdin/stdout redirected with `dup2`, stderr to `/dev/null`. No shell is ever involved. |
| 2 | Data race: process-local `ps->seq++` used for temp-file names, read-modify-write from multiple FUSE worker threads | low | `__atomic_add_fetch` |
| 3 | Torn meta reads: `mark_pending_accepted()` rewrote the patch index in place (`fopen "w"`) while other threads read it | low | Write to a temp file and atomically `rename()` into place |
| 4 | Lost write / stale content: commits can land out of order (async), so "highest seq wins" served older content | high (data integrity) | Content resolution orders patches/merges by `opened_at` (latest write wins), seq as tie-break |
| 5 | Spurious `ENOENT` on `open(O_CREAT)`: the commit ran in `release`, which libfuse processes asynchronously; the next `open` could observe pre-commit state (kernel stripped `O_CREAT` after a successful LOOKUP) | medium | Commit moved to `flush` (synchronous with `close()`), guarded by a `written` flag (shell dup2/close can flush before writes); `release` is the fallback; `entry_timeout`/`negative_timeout`/`attr_timeout = 0`; `open` retries briefly and proceeds as create |
| 6 | Test-path knowledge in production code (`/stress`, `noop.txt` left in `ps_resolve`) | low (hygiene) | removed |
| 7 | ht.h (vendored hash table) OOM: `ht__expand` on failed `malloc` leaves a NULL slot array that the rehash dereferences | medium | library patched internally: OOM-safe `ht__expand`/`ht__put` (returns NULL), hash clamp (no `+2` wrap into `HT__EMPTY`/`HT__DELETED` markers), tombstone-safe insert, overflow-guarded capacity math; upstream fix shipped as `patches/ht.h.patch` (applies to pristine ht.h with `patch -p0`) |
| 8 | ht.h usage: `collect_path` used `*arr = realloc(*arr, …)` — leaks the old buffer on allocation failure | low | realloc via temp pointer, checked; `ht_put` results now NULL-checked (OOM degrades to unindexed session / dedup-skip, never a crash) |
| 9 | ht.h usage audit: key lifetime (session `relpath` / strdup'd dedup keys), value-overwrite on duplicate keys, and delete identity checks verified; all `sessions_ht` ops under `ps->lock` | informational | audited, no defects found |
| 10 | `x = realloc(x, …)` pattern in `rl_add`, `meta_load`, `slurp_file`, `split_diff`, `split_lines`, `ops_add`, `region_add_lines`, `build_regions`: lost buffer on allocation failure (leak / NULL deref / lost partial parse) | low | all converted to temp-pointer realloc with graceful degradation (partial parse kept, range skipped, -1 returned) |
| 11 | World-readable store: files 0644, dirs 0755 | medium | daemon sets `umask(0077)`; store dirs/files now 0700/0600 |
| 12 | Symlink escape on apply-to-origin: `copy_file_to_path` followed a symlink planted in the lower tree, redirecting the write elsewhere | medium | `O_NOFOLLOW` on the target open (write fails with ELOOP instead) |
| 13 | Silent truncation of percent-encoded path names (`enc[4096]`): distinct paths could collide on the same truncated store filename | low | `encode_path` detects truncation (returns -1, empty name → clean failure), buffers sized `ENC_MAX = 3*PATH_MAX+1` |
| 14 | Path traversal via git-diff headers: `+++ b/../../etc/passwd` made the overlay read arbitrary files as a diff base | high | `sanitize_relpath` rejects absolute paths and `.`/`..` components before use (verified: malicious diff rejected, target untouched) |
| 15 | DoS via oversized inputs: audit diffs read from sparse `pwrite`s could `malloc(st_size+1)` unbounded; `slurp_file` grew unbounded on huge bases | medium | audit diffs capped at 16 MiB (rejected), diff bases capped at 64 MiB (`SLURP_MAX`) |
| 16 | Last external tool `patch` (fork from multithreaded daemon, runtime dependency, version variance) | medium | replaced by the in-process applier `apply_unified_diff`; `run_prog` removed — the daemon never forks, no external tools remain |

## 3. Remaining attack surface & known limitations

Not fixed — accepted or tracked:

1. ~~One external tool at runtime~~ **fixed**: `patch` was replaced by the
   in-process unified-diff applier (`src/diff.c: apply_unified_diff` — hunk
   parsing, context matching with forward/backward offset search, git-style
   conflict markers on unmatched hunks). **Zero external tools remain**:
   the static binary has no runtime dependencies beyond the kernel, and
   `run_prog`/fork was removed entirely.

2. ~~Fixed-size path buffers~~ **fixed**: `encode_path` now detects truncation
   (returns -1, no silent collision) and encoding buffers are sized for the
   worst case (`ENC_MAX = 3*PATH_MAX+1`). Paths whose encoded name still
   exceeds the filesystem `NAME_MAX` fail loudly (ENAMETOOLONG) instead of
   colliding — a hashed/nested naming scheme would fully lift that limit.

3. **Patch-index append atomicity**: `meta` appends rely on O_APPEND atomicity
   for single-line writes (< PIPE_BUF). Safe today; a rewrite+rename scheme
   would be needed if lines grow.

4. **Same-owner stale base = last-writer-wins**: a writer that opened before
   its own previous commit landed silently overwrites it. Only cross-owner
   range overlaps are soft-blocked. Documented semantics, not a bug.

5. ~~No privilege separation~~ **fixed**: the daemon sets `umask(0077)` and
   creates store dirs/files 0700/0600, so patches/messages are no longer
   world-readable.

6. ~~`fork()`-in-threaded-daemon~~ **fixed**: `run_prog` was removed entirely
   (all diff/merge/patch work is now in-process); the daemon never forks.

7. **Symlink/`..` handling**: relpaths are used as-is in `lower_path`
   (lower + "/" + relpath). Apply-to-origin now opens the target with
   `O_NOFOLLOW`, so an attacker-planted symlink in the lower tree cannot
   redirect writes outside it. Git-diff headers are sanitized (`sanitize_relpath`:
   no `..`/`.` components, no absolute paths) before use. Intermediate
   directory components that are symlinks are still followed (full fix:
   `openat2(RESOLVE_BENEATH)`). The overlay does not follow symlinks for its
   own state.

## 4. Secure-development checklist

Rules for any change to this codebase:

- No `system()`, `popen()`, or any shell construction. External programs
  only via `run_prog()` with argv arrays.
- All string formatting via bounded `snprintf`; never `sprintf`/`strcpy`/`strcat`.
- Every read of a file that can be written concurrently must tolerate
  missing/partial content (append is atomic for one line only).
- All mutations of `ps->sessions` under `ps->lock`; process-local counters
  atomic (`__atomic_*`); cross-process state under `flock` (seq counter).
- Any file that is rewritten while others read it: temp file + atomic `rename`.
- New FUSE ops: handle `entry_timeout=0` semantics (no dentry caching),
  re-validate existence, and never trust that LOOKUP preceded OPEN.
- New inputs (audit diffs, filenames): assume adversarial; keep them out of
  shell/command strings entirely.
- Stress test (`make stress-test`, `make stress-long`) before merging anything
  touching the commit/resolution path.

## 5. Recommendations

1. DONE: `diff`/`diff3` replaced by the in-process Myers engine
   (`src/diff.c`: `diff_unified`, `merge3`) — two runtime dependencies and the
   fork-from-worker-thread concern removed for those paths.
2. Keep `patch` behind `run_prog` as the sole external tool until the
   unified-diff applier is implemented and pinned by tests, then drop it.
3. Harden store permissions (0700/0600) for multi-user deployments.
4. Move path encoding to dynamically sized buffers to eliminate truncation.
5. Re-run the full security stress after each of the above.
