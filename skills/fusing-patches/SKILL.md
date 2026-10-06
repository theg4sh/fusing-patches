---
name: fusing-patches
description: Drive the fusing-patches multi-writer FUSE overlay (patch tracking, soft-block conflicts, /fuse/merge/ resolution, patch-view audit, multi-layered mountpoint stacks). Use when agents edit files through this overlay, resolve conflicts, audit patches, or apply patches back to an origin.
---

# fusing-patches — agent guide

A multi-writer FUSE overlay. Agents mount it over a read-only origin, edit
files, and the overlay tracks each edit as a **patch** (full-file snapshot +
byte ranges) in a shared **store**. Overlapping edits are soft-blocked with a
diff message; the agreed result is materialized back into the origin when the
last mount unmounts.

## Quick start

```bash
make                                   # build -> build/fusing-patches
./build/fusing-patches <lower> <store> <mnt> --name=agent1 -f &
echo hello > <mnt>/file.txt            # edit (becomes a patch)
cat <mnt>/file.txt                     # read (passthrough or latest patch)
fusermount3 -u <mnt>                   # unmount -> patch applied to <lower>
```

Key facts agents rely on:

- A write commits **synchronously with `close()`**; after `close()` returns,
  the write is visible on that mount.
- A write whose ranges overlap another writer's change is **soft-blocked**:
  `close()` fails with `EAGAIN`, nothing is committed, and a conflict message
  (diff) goes to the **daemon's stderr** and to **`/fuse/messages/`** in the
  mount. Re-read and retry to resolve.
- Writes that produce no content change ("no-op") are accepted but store no
  patch.
- The store is shared state: every mountpoint for the same `<lower>` must use
  the same `<store>` to collaborate; different stores = isolated layers.

## CLI

```
build/fusing-patches <lower> <store> <mountpoint> [--name=ID] [--merge|--patch-view] [fuse options]
```

| Kind           | Flag             | Purpose                                                             |
|--------------- |----------------- |-----------------------------------------------------                |
| writer         | `--name=ID`      | edit; patches are tracked; soft-blocked on conflict                 |
| merge view     | `--merge`        | read-only view of the agreed merged result                          |
| patch-view     | `--patch-view`   | auditor: edit (updates merged state), `rm` (reject), git-diff apply |

## Core workflows

### 1. Edit a file

```bash
printf 'new content\n' > <mnt>/path/file.txt
cat <mnt>/path/file.txt
```

### 2. Handle a soft-blocked write

```bash
printf 'my change\n' > <mnt>/notes.txt
# close() failed with EAGAIN -> read the message:
cat <mnt>/fuse/messages/*.msg
#   CONFLICT: your change to /notes.txt was soft-blocked (not applied) because it
#   overlaps changes made by another writer:
#   --- your base
#   +++ other writers
#   ...
# re-read (re-sync), merge the other change into your edit, retry the write.
```

Identical retries of the same conflicting range are de-duplicated; a message
is printed again only when the conflict extends to new ranges.

### 3. Resolve via /fuse/merge/

Write the agreed full content to the virtual path inside any mount:

```bash
printf 'resolved content\n' > <mnt>/fuse/merge/notes.txt
```

This becomes the merged snapshot — served by every mount and by `--merge`
views — and is what gets applied to origin at the final unmount.

### 4. Audit with patch-view

```bash
./build/fusing-patches <lower> <store> <mnt> --patch-view -f &
cat <mnt>/file.txt                          # review merged state
printf 'fixed\n' > <mnt>/file.txt           # update the patch
rm <mnt>/other.txt                          # reject a patch (reverts to origin)
cat > <mnt>/fuse/audit/change.patch <<'EOF'  # apply a git-diff
diff --git a/notes.txt b/notes.txt
--- a/notes.txt
+++ b/notes.txt
@@ -1 +1 @@
-old
+new
EOF
```

Diff header paths are sanitized (`..` and absolute paths rejected). Unmatched
hunks produce conflict markers (on stderr + `/fuse/messages/`) and are not
committed.

## Multi-layered mountpoint scenarios

A mountpoint's `lower` may itself be an overlay mount, so stacks are
first-class. Each layer has its own store; unmounting a layer applies its
patches into *its* lower.

### A. Origin → writer → writer (staged edits, two layers)

```bash
./build/fusing-patches /srv/origin /srv/store1 /mnt/l1 --name=l1 -f &
./build/fusing-patches /mnt/l1      /srv/store2 /mnt/l2 --name=l2 -f &

printf 'edit\n' > /mnt/l2/file.txt      # edit through the top layer
cat /mnt/l2/file.txt                    # 'edit' (top layer's patch)
cat /mnt/l1/file.txt                    # unchanged (layer below sees nothing yet)

fusermount3 -u /mnt/l2                  # l2 unmounts -> its patch lands in l1
cat /mnt/l1/file.txt                    # 'edit' (now an l1 patch)
fusermount3 -u /mnt/l1                  # l1 unmounts -> applied to origin
cat /srv/origin/file.txt                # 'edit'
```

Expected behavior: changes materialize one layer at a time, bottom-up, on
unmount; each layer's store persists as its audit record.

### B. Origin → writer → patch-view (audit before materialization)

```bash
./build/fusing-patches /srv/origin /srv/store1 /mnt/w --name=w -f &
./build/fusing-patches /mnt/w      /srv/store2 /mnt/audit --patch-view -f &

printf 'draft\n' > /mnt/w/notes.txt     # writer drafts a patch
# auditor reviews through the upper layer:
cat /mnt/audit/notes.txt                # sees the draft via store1's view
printf 'final\n' > /mnt/audit/notes.txt # auditor updates the merged state
fusermount3 -u /mnt/audit               # upper layer unmounts first
fusermount3 -u /mnt/w                   # then the writer layer
cat /srv/origin/notes.txt               # 'final'
```

Expected behavior: the auditor's merged state wins; only what remains after
the audit is applied to origin.

### C. Writer stack + merge view (observe the agreed result)

```bash
./build/fusing-patches /srv/origin /srv/store1 /mnt/w --name=w -f &
./build/fusing-patches /mnt/w      /srv/store2 /mnt/m --merge -f &

printf 'v1\n' > /mnt/w/notes.txt
printf 'v2\n' > /mnt/w/fuse/merge/notes.txt   # agreement via the writer's mount
cat /mnt/m/notes.txt                          # 'v2' (read-only merged view)
```

Expected behavior: `--merge` views are read-only (`EROFS` on writes) and never
hold the apply-to-origin trigger back (only non-merge mounts count).

### D. Two writer stacks sharing one origin and one store (collaboration)

```bash
./build/fusing-patches /srv/origin /srv/store /mnt/alice --name=alice -f &
./build/fusing-patches /srv/origin /srv/store /mnt/bob   --name=bob   -f &
```

This is NOT stacking: both mounts sit directly on the origin but share one
store, so their patches are visible to each other and conflicts are
soft-blocked with diffs (see "Handle a soft-blocked write"). A writer that
re-reads and retries converges; unresolved overlapping edits are rejected,
never silently merged.

### E. Three layers: writer → writer → patch-view

```bash
./build/fusing-patches /srv/origin /srv/store1 /mnt/l1 --name=l1 -f &
./build/fusing-patches /mnt/l1      /srv/store2 /mnt/l2 --name=l2 -f &
./build/fusing-patches /mnt/l2      /srv/store3 /mnt/audit --patch-view -f &

printf 'draft\n' > /mnt/l2/notes.txt         # edit on the middle layer
cat /mnt/audit/notes.txt                     # top layer sees it
rm /mnt/audit/notes.txt                      # auditor rejects it
fusermount3 -u /mnt/audit
fusermount3 -u /mnt/l2
fusermount3 -u /mnt/l1
cat /srv/origin/notes.txt                    # unchanged (rejected at the top)
```

Expected behavior: rejection at the top layer means nothing reaches the
layers below; unmount order is top-down, and only the surviving state is
materialized into origin.

## Agent rules and pitfalls

- After a soft-block, **re-read before rewriting** — the retry must be based
  on the current content, or it will be soft-blocked again.
- Watch **two channels** for conflict messages: the daemon's stderr and
  `/fuse/messages/` in the mount.
- No-op writes are free (no patch stored) — safe for idempotent retries.
- Keep store directories private (the daemon creates them 0700/0600) and
  never share a store between unrelated lower paths.
- Git-diff audits: target paths must be relative; `..`/absolute paths are
  rejected; diffs over 16 MiB are rejected.
- Unmount order matters in stacks: unmount top-down; the last non-merge mount
  triggers apply-to-origin.
- If the origin changes behind a stack (another layer, direct edit), the final
  apply emits a 3-way conflict diff (base / theirs / ours) instead of
  clobbering.

## Troubleshooting

| Symptom                                  | Cause / fix                                                                                                                               |
|----------------------------------------- |-----------------------------------------------------                                                                                      |
| `close()` returns EAGAIN                 | soft-blocked conflict: read `/fuse/messages/`, re-sync, retry                                                                             |
| write not visible on another mount       | different stores, or the other mount is a lower layer — verify `<store>` paths and unmount order                                          |
| `open()` fails with ENOENT on a new file | use `O_CREAT`; the daemon disables dentry caching and re-validates every open                                                             |
| audit diff "rejecting unsafe diff path"  | the header contains `..` or an absolute path                                                                                              |
| nothing applied to origin after unmount  | a non-merge mount is still mounted, or the file was rejected (`rm` in patch-view), or the origin changed (3-way conflict emitted instead) |
| conflict message absent                  | the conflicting range was already reported (de-duplicated by range)                                                                       |

## References

- Architecture: `docs/ARCHITECTURE.md`
- Workflows & CLI: `docs/USAGE.md`
- Security model: `docs/SECURITY.md`
- Development & build: `docs/DEVELOPMENT.md`
- Testing: `docs/TESTING.md`
