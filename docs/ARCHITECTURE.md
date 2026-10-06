# Fusing Patches — Architecture & Behavior

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
  │    messages/, origin/, tmp/                              │
  └─────────────────────────────────────────────────────────┘
      │
      ├── writer mount (--name=alice)   ── edit, track patches
      ├── writer mount (--name=bob)     ── edit, soft-block on conflict
      ├── merge view (--merge)          ── read-only agreed result
      └── patch view (--patch-view)     ── auditor: edit / rm / git-diff
```

---

## 1. Core concepts

| Term               | Meaning                                                                                                                                             |
|------------------- |---------------------------------------------------------------------------------------------------------------------------------------------------- |
| **lower / origin** | The read-only source directory. Reads pass straight through. After the last mount unmounts, accepted patches are written back here.                 |
| **patch**          | A committed full-file snapshot, tagged with `owner`, a global monotonic `seq`, and the **byte ranges** it touched.                                  |
| **merge**          | The agreed final snapshot for a file, produced by `/fuse/merge/<path>`, patch-view edits, or git-diff application.                                  |
| **seq**            | A globally monotonic counter, flock-guarded in `<store>/seq`, shared across processes.                                                              |
| **owner**          | The mountpoint's identity (`--name=`); owns per-mountpoint state and resolution messages.                                                           |
| **base_seq**       | The `seq` of the content a writer based its edit on (captured at open).                                                                             |
| **opened_at**      | The open timestamp of an edit; orders snapshots because async commits can land out of order (so `seq` order ≠ write order).                         |

**Content resolution** (what a read serves): the latest-written snapshot wins.
Patches and merges are ordered by `opened_at`, tie-broken by `seq`; a file with
neither a merge nor an accepted patch is served from the lower.

---

## 2. Store layout

```
<store>/
  seq                         global monotonic counter (flock)
  blobs/<id>.blob             committed patch snapshots
  meta/<enc-path>             patch index per file: "P <id> <opened> <committed> <size> <owner> <seq> <state>"
  ranges/<id>                 "lo hi" byte ranges a patch touched
  merge/<enc>.blob            agreed merged snapshot
  merge/<enc>.meta            "M <seq> <size> <owner> <opened_at>"
  origin/<enc>                lower-content snapshot taken at the first edit's base
  mounts/<owner>              last incorporated seq for a mountpoint
  mounts/active/<kind>.<owner>.<pid>   active-mount registry
  messages/<owner>/<seq>.msg  conflict / resolution messages
  messages/<owner>/.reported/<enc>     de-dup: already-reported conflict ranges
  tmp/                        transient buffers
```

All store paths are percent-encoded flat filenames (no path traversal in the
store); directories 0700, files 0600.

---

## 3. Mount kinds

| Kind           | Flag                   | Writable?    | Behavior                                                                                                             |
|--------------- |----------------------  |------------- |--------------------------------------------------------------------------------------------------------------------- |
| **writer**     | *(default)* `--name=X` | yes          | Edits are tracked as patches. Writes are soft-blocked when they overlap another writer's change.                     |
| **merge**      | `--merge`              | no (`EROFS`) | Read-only view of the agreed merged result.                                                                          |
| **patch-view** | `--patch-view`         | yes          | The auditor's view. Edits update the merged state; `rm` rejects a patch; `/fuse/audit/<n>.patch` applies a git-diff. |

Every mount registers itself in `<store>/mounts/active/` at `init` and
unregisters at `destroy`.

---

## 4. Conflict model — range-based soft-blocking

Each write session accumulates the **byte ranges** it touches (`ps_write` /
`ps_truncate`). On commit, a write is:

- **accepted** if its ranges do *not* overlap another writer's committed ranges
  (with `seq > base_seq`) nor an unseen merge; otherwise
- **soft-blocked**: nothing is committed, `close()` returns `EAGAIN`, and a
  conflict message (with the diff) goes to **stderr** and to `messages/`.

Disjoint ("small piece") edits are accepted independently. Messages are
**de-duplicated by range**: one is emitted only when the conflict *extends* the
already-reported ranges for that `(owner, file)`.

A write whose final content equals its base (a "no-op" patch) is accepted but
stores no patch.

---

## 5. Messages

Conflict messages are delivered to two channels so they cannot be missed:

- **stderr** of the FUSE daemon, and
- **`/fuse/messages/`** in the editing mount (durable in `<store>/messages/<owner>/`).

A git-diff conflict prints the merged output with `<<<<<<<` / `=======` /
`>>>>>>>` conflict markers (produced in-process by the diff engine) to both
channels.

---

## 6. Multi-level mounts

The lower directory may itself be a FUSE mount, so overlays stack:

```
origin ──► overlay(m1, store1) ──► overlay(m2, store2)
```

Each layer has its own store. Reading `m2` passes through `m1` to `origin`.
Unmounting `m2` applies its patches into *its* lower (`m1`), becoming `m1`
patches; unmounting `m1` applies them to `origin`.

---

## 7. Lifecycle — apply-to-origin

1. Mounts register in the active-mount registry.
2. On `destroy`, a mount unregisters itself.
3. When **no non-merge mount remains**, `ps_apply_to_origin()` runs: for every
   un-rejected file, the resolved content (merge → latest patch) is copied into
   the origin (with parent directories as needed), the target opened with
   `O_NOFOLLOW`.
4. Rejected files are skipped, keeping their origin content.

The store is never deleted — it persists as the audit record.

---

## 8. patch-view & the auditor

The auditor mounts `--patch-view` and can, before the final unmount:

- **update** a patch: write the file (edits go to the merged state),
- **reject** a patch: `rm` the file (clears its patch/merge state → reverts to
  origin),
- **produce new patches** in git/diff format: write a (possibly multi-file)
  unified diff to `/fuse/audit/<name>.patch`; on close it is split by file path
  (headers sanitized) and each file's hunks are applied by the in-process
  applier. Unmatched hunks yield a conflict-marked diff and are *not* committed.

---

## 9. Static build

`make static` downloads and builds libfuse3 as a static archive, then links a
fully static binary with no runtime dynamic dependencies:

```
make static   # -> build/fusing-patches-static
$ ldd build/fusing-patches-static
        not a dynamic executable
```

---

## 10. Concurrency & correctness

- **Commits are synchronous with `close()`.** The patch commit runs in the
  `flush` handler, whose completion the kernel's `close()` waits for; `release`
  is the fallback for truncate-only edits.
- **`flush` ≠ end of edit.** Shell redirection (dup2/close) can fire `flush`
  before the writes arrive; a flush commits only once real `write()`s occurred
  (`ps_session_written`).
- **No kernel dentry caching.** `entry_timeout = negative_timeout =
  attr_timeout = 0`, because store state changes behind the kernel's back.
  `open` on a transiently-invisible file retries briefly and then proceeds as
  a create.
- **Latest-written content wins.** Snapshots are ordered by `opened_at`
  (tie-break `seq`), since async commits land out of order.
- All in-process state mutations are under `ps->lock`; the cross-process seq is
  under `flock`; store files that are rewritten while read go through
  temp-file + atomic `rename`.

---

## 11. In-process diff engine & fast search

- `src/diff.c` implements the unified-diff producer (`diff_unified`, Myers
  O(ND)), a git-style 3-way merge with conflict markers (`merge3`), and the
  unified-diff **applier** (`apply_unified_diff`: hunk parsing, context
  matching with a forward/backward offset search, conflict markers on
  unmatched hunks). **No external tools, no fork** — the daemon is fully
  self-contained. Diff inputs beyond 2048 total lines fall back to a coarse
  replace-all edit script; audit diffs are capped at 16 MiB and diff bases at
  64 MiB.
- Fast lookups use a vendored hash table (`src/ht.h`, MIT/Public Domain, with
  hardening fixes tracked in `patches/ht.h.patch`): active sessions are indexed
  by relpath (O(1) `ps_active_size`) and apply-to-origin path dedup is O(1)
  via a murmur3-hashed string set.
- Path keys are hashed with murmur3 32-bit (public domain, byte-wise,
  alignment-safe implementation in `src/patchstore.c`).

---

## 12. Licensing

The project is **MIT-licensed** (`LICENSE`). Vendored code: `src/ht.h`
(MIT/Public Domain) and the murmur3 hash (public domain). The diff engine is
original code — no LGPL/GPL components are used or linked.

---

## 13. Security model

See `SECURITY.md` for the trust boundaries, implementation guarantees and
known limitations. In brief: no shell, no fork, no external tools; private
store (0700/0600); sanitized diff paths; bounded input sizes; `O_NOFOLLOW` on
apply-to-origin; atomic index replacement.

---

## 14. Usage

Workflows and CLI reference: `USAGE.md`.

## 15. Testing

Unit, integration and stress testing: `TESTING.md`.
