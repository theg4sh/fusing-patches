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
| 7 | ht.h (vendored hash table) OOM: `ht__expand` on failed `malloc` leaves a NULL slot array that the rehash dereferences (library API has no error return) | medium | `HT_MALLOC` overridden with a fatal-on-OOM allocator before the include |
| 8 | ht.h usage: `collect_path` used `*arr = realloc(*arr, …)` — leaks the old buffer on allocation failure | low | realloc via temp pointer, checked |
| 9 | ht.h usage audit: key lifetime (session `relpath` / strdup'd dedup keys), value-overwrite on duplicate keys, and delete identity checks verified; all `sessions_ht` ops under `ps->lock` | informational | audited, no defects found |

## 3. Remaining attack surface & known limitations

Not fixed — accepted or tracked:

1. **One external tool still used at runtime**: `patch` (via `run_prog`) for
   audit-diff application. Injection-free (argv arrays, no shell), but:
   - the *static* binary still requires `patch` on the target host;
   - `run_prog` forks from multithreaded FUSE worker threads (child restricted
     to async-signal-safe calls before `exec`, so currently safe, but fragile);
   - GNU patch behavior varies across versions (exit codes 0/1/2, fuzz
     matching) — the code special-cases these.
   `diff -u` and `diff3 -m` were already replaced by the in-process
   Myers-based engine (`src/diff.c`: `diff_unified`, `merge3`) — no LGPL/GPL
   code; the project is MIT-licensed. A hand-rolled unified-diff applier would
   drop the last external tool.

2. **Fixed-size path buffers**: path encoding (`encode_path`) silently
   truncates for names whose encoded form exceeds the 4096-byte buffers —
   collision risk for pathological names. FUSE paths are bounded by PATH_MAX
   but the 3x percent-encoding expansion can exceed the buffer.

3. **Patch-index append atomicity**: `meta` appends rely on O_APPEND atomicity
   for single-line writes (< PIPE_BUF). Safe today; a rewrite+rename scheme
   would be needed if lines grow.

4. **Same-owner stale base = last-writer-wins**: a writer that opened before
   its own previous commit landed silently overwrites it. Only cross-owner
   range overlaps are soft-blocked. Documented semantics, not a bug.

5. **No privilege separation**: the daemon runs with the caller's uid; store
   files are mode 0644/0755 (world-readable). Messages may contain file
   content diffs. Consider 0700 dirs/0600 files in a multi-user deployment.

6. **`fork()`-in-threaded-daemon**: `run_prog` is invoked from FUSE worker
   threads. The child only calls `open`/`dup2`/`close`/`execvp` before
   `exec`, which is async-signal-safe; keep it that way — no malloc in the
   child path.

7. **Symlink/`..` handling**: relpaths are used as-is in `lower_path`
   (lower + "/" + relpath). A malicious path component is confined to the
   lower subtree only if the lower itself contains no symlinks pointing
   outside. The overlay does not follow symlinks for its own state.

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
