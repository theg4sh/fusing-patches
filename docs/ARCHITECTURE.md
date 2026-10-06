# Fusing Patches — Architecture & Behavioral Features

A multi-writer FUSE overlay that tracks edits to a read-only lower filesystem as
**patches**, detects conflicting edits by **byte range**, and materializes the
agreed result back into the origin on unmount.

```
  origin (read-only fs, may itself be a FUSE mount)
      │  reads pass through; writes become patches in the store
      ▼
  ┌─────────────────────────────────────────────────────────┐
  │  store/ (sidecar dir shared by all mountpoints)          │
  │    seq, blobs/, meta/, merge/, ranges/, mounts/,         │
  │    messages/, tmp/                                       │
  └─────────────────────────────────────────────────────────┘
      │
      ├── writer mount (--name=alice)   ── edit, track patches
      ├── writer mount (--name=bob)     ── edit, soft-block on conflict
      ├── merge view (--merge)          ── read-only agreed result
      └── patch view (--patch-view)     ── auditor: edit / rm / git-diff
```

---

## 1. Core concepts

| Term      | Meaning |
|-----------|---------|
| **lower / origin** | The read-only source directory. Reads pass straight through. After the last mount unmounts, accepted patches are written back here. |
| **patch**  | A committed full-file snapshot, tagged with `owner`, a global monotonic `seq`, and the **byte ranges** it touched. |
| **merge**  | The *agreed* final snapshot for a file, produced by `/fuse/merge/<path>`, patch-view edits, or git-diff application. Highest priority when serving content. |
| **seq**    | A globally monotonic counter, flock-guarded in `<store>/seq`, shared across processes. |
| **owner**  | The mountpoint's identity (`--name=`); tracks per-mountpoint last-incorporated state and resolution messages. |
| **base_seq** | The `seq` of the content a writer based its edit on (captured at open). |

**Content resolution** (what a read serves), highest `seq` wins:

```
merge (agreed)  >  accepted patch (state A)  >  lower (origin)
```

---

## 2. Store layout

```
<store>/
  seq                         global monotonic counter (flock)
  blobs/<id>.blob             committed patch snapshots
  meta/<enc-path>             patch index per file: "P <id> <opened> <committed> <size> <owner> <seq> <state>"
  ranges/<id>                 "lo hi" byte ranges a patch touched
  merge/<enc>.blob            agreed merged snapshot
  merge/<enc>.meta            "M <seq> <size> <owner>"
  mounts/<owner>              last incorporated seq for a mountpoint
  mounts/active/<kind>.<owner>.<pid>   active-mount registry
  messages/<owner>/<seq>.msg  conflict / resolution messages
  messages/<owner>/.reported/<enc>     de-dup: already-reported conflict ranges
  tmp/                        transient buffers
```

---

## 3. Mount kinds

| Kind | Flag | Writable? | Behavior |
|------|------|-----------|----------|
| **writer** | *(default)* `--name=X` | yes | Edits are tracked as patches. Writes are soft-blocked when they overlap another writer's change. |
| **merge** | `--merge` | no (`EROFS`) | Read-only view of the agreed merged result. |
| **patch-view** | `--patch-view` | yes | The auditor's view. Edits update the merged state; `rm` rejects a patch; `/fuse/audit/<n>.patch` applies a git-diff. |

Every mount registers itself in `<store>/mounts/active/` at `init` and unregisters
at `destroy`.

---

## 4. Conflict model — range-based soft-blocking

Each write session accumulates the **byte ranges** it touches (`ps_write` /
`ps_truncate`). On commit, a write is:

- **accepted** if its ranges do *not* overlap another writer's committed ranges
  (with `seq > base_seq`) nor an unseen merge; otherwise
- **soft-blocked**: nothing is committed, `close()` returns `EAGAIN`, and a
  conflict message (with the diff) is printed to **stderr** and stored under
  `messages/`.

Disjoint ("small piece") edits are accepted independently. Messages are
**de-duplicated by range**: a message is emitted only when the conflict
*extends* the set of already-reported ranges for that `(owner, file)`.

A write whose final content equals its base (a "no-op" patch) is accepted but
stores no patch.

---

## 5. Messages

Conflict messages are delivered to two channels so they cannot be missed:

- **stderr** of the FUSE daemon (visible to the operator / `patch` user), and
- **`/fuse/messages/`** in the editing mount (durable in `<store>/messages/<owner>/`).

A git-diff conflict prints the **real final diff-conflict** — the merged output
with `<<<<<<<` / `=======` / `>>>>>>>` markers (via `patch --merge`) — to both
channels.

---

## 6. Multi-level mounts

The lower directory may itself be a FUSE mount, so overlays stack:

```
origin ──► overlay(m1, store1) ──► overlay(m2, store2)
```

Each layer has its own store. Reading `m2` passes through `m1` to `origin`.
When `m2` unmounts, its patches are applied to *its* lower (`m1`), becoming
`m1` patches; when `m1` unmounts, they are applied to `origin`.

---

## 7. Lifecycle — apply-to-origin

1. Writers/patch-view/merge mounts register in the active-mount registry.
2. On `destroy`, a mount unregisters itself.
3. When **no non-merge mount remains**, `ps_apply_to_origin()` runs: for every
   un-rejected file, the resolved content (merge → latest patch) is copied into
   the origin, creating parent directories as needed.
4. Rejected files are skipped, so they keep their origin content.

The store is never deleted — it persists as the audit/history record.

---

## 8. patch-view & the auditor

The auditor mounts `--patch-view` and can, before the final unmount:

- **update** a patch: write the file (edits go to the merged state),
- **reject** a patch: `rm` the file (clears its patch/merge state → reverts to
  origin),
- **produce new patches** in native git/diff format: write a (possibly
  multi-file) unified diff to `/fuse/audit/<name>.patch`; on close it is split
  by file path and each file's hunks are applied with `patch --merge`. Conflicting
  hunks yield the real 3-way diff-conflict and are *not* committed.

---

## 9. Static build

`make static` downloads and builds libfuse3 as a static archive, then links a
fully static binary with no runtime dynamic dependencies:

```
make static   # -> build/overlayfs-static
$ ldd build/overlayfs-static
        not a dynamic executable
```

---

## 10. Concurrency & correctness (FUSE usage)

- **Commits are synchronous with `close()`.** The patch commit runs in the
  `flush` handler, whose completion the kernel's `close()` waits for.
  (`release` is processed asynchronously by libfuse, so committing there left
  a window where the next `open()` could observe pre-commit state — a real
  race found by the C stress harness.) `release` remains as a fallback for
  truncate-only edits.
- **`flush` ≠ end of edit.** Shell redirection (dup2/close) can fire `flush`
  before the writes arrive, so a flush only commits once real `write()`s
  occurred (`ps_session_written`).
- **No kernel dentry caching.** `entry_timeout = negative_timeout =
  attr_timeout = 0`, since store state changes behind the kernel's back.
  `open` on a transiently-invisible file retries briefly and then proceeds as
  a create (the caller had `O_CREAT`; the kernel stripped it after its
  LOOKUP succeeded).
- **Current content = latest *opened* patch**, not highest seq: async commits
  can land out of order, so seq order ≠ write order. Resolution orders
  patches/merges by `opened_at` (tie-break by seq).

## 10.1 In-process diff engine & fast search

- `src/diff.c` implements everything in-process: the unified-diff producer
  (`diff_unified`, Myers O(ND) with linear backtracking), a git-style 3-way
  merge with conflict markers (`merge3`), and the unified-diff **applier**
  (`apply_unified_diff`: hunk parsing, context matching with a
  forward/backward offset search, conflict markers on unmatched hunks).
  **No external tools, no fork** — the daemon is fully self-contained.
  Diff inputs larger than 2048 total lines fall back to a coarse replace-all
  edit script; audit diffs are capped at 16 MiB, diff bases at 64 MiB.
- Fast path lookups use a vendored hash table (`src/ht.h`, tsoding/ht.h,
  MIT/Public Domain, patched for OOM safety — see `patches/ht.h.patch`):
  active sessions are indexed by relpath (`ps->sessions_ht`, O(1)
  `ps_active_size`) and apply-to-origin path dedup is O(1) via a
  murmur3-hashed string set.
- Path keys are hashed with murmur3 32-bit (public domain, byte-wise,
  alignment-safe implementation in `src/patchstore.c`).

## 10.2 Licensing

The project is **MIT-licensed** (`LICENSE`). Vendored code: `src/ht.h`
(MIT/Public Domain) and the murmur3 hash (public domain). The diff engine is
original code — no LGPL/GPL components are used or linked, so the static
binary is redistributable under MIT.

## 11. Security review — issues found & fixed

| # | Issue | Severity | Fix |
|---|-------|----------|-----|
| 1 | **Command injection**: `diff`/`diff3`/`patch` invoked via `system()`/`popen()` with paths built from FUSE filenames (a filename containing `'` escapes into the shell) | high | all external tools now run via `fork`+`execvp` with an argument array (`run_prog`), I/O redirected with `dup2` — no shell involved |
| 2 | **Data race**: process-local `ps->seq++` for temp-file names | low | `__atomic_add_fetch` |
| 3 | **Torn meta reads**: `mark_pending_accepted` rewrote the patch index in place while other threads read it | low | write temp + atomic `rename` |
| 4 | **Lost write / stale content**: highest-`seq`-wins wrong when async commits land out of order | high (data) | resolution orders by `opened_at` (see §10) |
| 5 | **ENOENT on `open(O_CREAT)`**: async `release`-commit window vs the next open | medium | commit moved to `flush` (synchronous) + `entry_timeout=0` + tolerant `open` (§10) |

Remaining known limitations (documented, not fixed):
- Path encoding buffers are fixed-size; >PATH_MAX/3 encoded names are silently
  truncated (collision risk for pathological names).
- The patch-index append (`fopen "a"`) relies on O_APPEND atomicity for
  single-line writes; a full rewrite+rename would be needed for large lines.
- Same-owner writes that *read* a stale base (open before the previous commit)
  are treated as last-writer-wins, not conflicts; cross-owner overlaps are
  caught by the range-based soft-block.

## 12. Usage

```bash
make                       # dynamic dev build -> build/overlayfs
make static                # static build       -> build/overlayfs-static

./build/overlayfs <lower> <store> <mnt> --name=alice -f      # writer
./build/overlayfs <lower> <store> <mnt> --name=bob   -f      # another writer
./build/overlayfs <lower> <store> <mnt> --merge      -f      # merged view
./build/overlayfs <lower> <store> <mnt> --patch-view -f      # auditor view

# virtual namespace inside a mount:
#   /fuse/merge/<path>      write agreed content (resolution)
#   /fuse/messages/         read this mountpoint's conflict messages
#   /fuse/audit/<n>.patch   apply a (multi-file) git-diff
```

Mounting still requires the `fusermount3` setuid helper (kernel mount
privilege), which is not a library dependency.

---

## 13. Tests

| Script | Covers |
|--------|--------|
| `tests/test_merge.sh`   | passthrough, clean accept, soft-block + stderr + message, de-dup, re-sync retry, disjoint ranges, merge, lower untouched |
| `tests/test_lifecycle.sh` | apply-to-origin, multi-level mounts, patch-view edit/reject, git-diff audit |
| `stress/stress_test.sh`  | high-frequency concurrent writes + no-op patch acceptance |
| `stress/stress_long.sh [sec]` | sustained stress (default 120 s) watching for stability |
