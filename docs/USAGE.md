# Usage — workflow guide

How to run fusing-patches and drive the multi-writer merge workflow.

## CLI

```
build/fusing-patches <lower> <store> <mountpoint> [--name=ID] [--merge|--patch-view] [fuse options]
```

| Argument       | Meaning                                                                                       |
|----------      |---------                                                                                      |
| `lower`        | read-only source directory (may itself be a FUSE mount — see multi-level)                     |
| `store`        | sidecar patch-store directory, shared by all mountpoints                                      |
| `mountpoint`   | where this view is mounted                                                                    |
| `--name=ID`    | mountpoint identity (agent name); default `agent`                                             |
| `--merge`      | read-only view of the agreed merged result                                                    |
| `--patch-view` | auditor's view: writable (edits update the merged state), `rm` = reject, git-diff application |

Mounting requires the `fusermount3` setuid helper (kernel mount privilege);
it is not a library dependency. The daemon runs with the caller's uid and
creates all store files private (0700/0600).

## Store directory

`<store>/` is created on first use and shared by every mountpoint over the
same `lower`. It persists after unmounting (it is the audit record). Layout is
documented in `ARCHITECTURE.md` §2.

## Workflows

### 1. Single writer: edit then materialize into origin

```bash
./build/fusing-patches /srv/origin /srv/store /mnt/editor --name=me -f &
printf 'new content\n' > /mnt/editor/notes.txt
fusermount3 -u /mnt/editor
# on last unmount, the patch is applied to /srv/origin/notes.txt
```

### 2. Two writers: soft-block + message + retry

```bash
./build/fusing-patches /srv/origin /srv/store /mnt/alice --name=alice -f &
./build/fusing-patches /srv/origin /srv/store /mnt/bob   --name=bob   -f &

# bob opens based on the current state, alice commits, bob writes overlapping
# ranges -> bob's close() fails with EAGAIN and a conflict message is printed
# to the daemon's stderr AND stored under /mnt/bob/fuse/messages/:

#   CONFLICT: your change to /notes.txt was soft-blocked (not applied) because it
#   overlaps changes made by another writer:
#   --- your base
#   +++ other writers
#   ...

# bob re-reads (re-syncs) and retries; the write is then accepted.
```

Messages are de-duplicated by range: repeating an identical conflict does not
re-print it; only a conflict that *extends* the already-reported ranges does.

### 3. Resolution via /fuse/merge/

A writer (or the auditor) writes the agreed full content to the virtual path
`/fuse/merge/<relpath>` inside any mount:

```bash
printf 'bob + alice resolved\n' > /mnt/bob/fuse/merge/notes.txt
```

This becomes the merged snapshot — the content served by every mount and by
the `--merge` view — and it is what gets applied to origin on the final
unmount.

### 4. Auditor: patch-view

```bash
./build/fusing-patches /srv/origin /srv/store /mnt/audit --patch-view -f &
```

- **Review**: reads show the merged state.
- **Update**: editing a file updates the merged state.
- **Reject**: `rm` a file reverts it to origin content (skipped at apply time).
- **Produce patches**: write a (possibly multi-file) unified/git diff to
  `/fuse/audit/<name>.patch`; on close it is split by file path, each file's
  hunks are applied in-process. Unmatched hunks produce a conflict diff
  (with `<<<<<<<`/`=======`/`>>>>>>>` markers) on stderr and under
  `/fuse/messages/`, and are **not** committed.

```bash
cat > /mnt/audit/fuse/audit/change.patch <<'EOF'
diff --git a/notes.txt b/notes.txt
--- a/notes.txt
+++ b/notes.txt
@@ -1,2 +1,2 @@
 line1
-line2
+LINE2-CHANGED
EOF
```

Diff paths are sanitized (`..`, absolute paths rejected).

### 5. Multi-level mounts

The `lower` may itself be an overlay mount, so stacks work:

```bash
./build/fusing-patches /srv/origin /srv/store1 /mnt/l1 --name=l1 -f &
./build/fusing-patches /mnt/l1      /srv/store2 /mnt/l2 --name=l2 -f &
```

Unmounting `l2` applies its patches into `l1` (its lower); unmounting `l1`
applies them into origin.

### 6. External origin changes → 3-way conflict

If the origin file is modified behind the stack's back (another layer, a
direct edit), apply-to-origin detects it against the stored origin snapshot
and emits a 3-way conflict diff (base snapshot / theirs=current lower /
ours=resolved) instead of clobbering. See `tests/test_origin.sh`.

## Virtual namespace

| Path                    | Purpose                                    |
|------                   |---------                                   |
| `/fuse/merge/<path>`    | write the agreed resolution (full content) |
| `/fuse/messages/`       | read this mountpoint's conflict messages   |
| `/fuse/audit/<n>.patch` | apply a (multi-file) git-diff              |

## Static binary

`make static` produces `build/fusing-patches-static` — self-contained
(glibc-static, no `.so` deps, no external tools). Usage is identical.
