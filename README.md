# fusing-patches

A multi-writer FUSE overlay for a read-only filesystem: edits are tracked as
byte-range **patches**, conflicting edits are soft-blocked with a diff, and the
agreed result is applied back into the origin when the last mount unmounts.

```
  origin (read-only fs, may itself be a FUSE mount)
      │  reads pass through; writes become patches in the store
      ▼
  store/ (sidecar dir shared by all mountpoints)
      │
      ├── writer mount (--name=alice)   ── edit, track patches
      ├── writer mount (--name=bob)     ── edit, soft-block on conflict
      ├── merge view (--merge)          ── read-only agreed result
      └── patch view (--patch-view)     ── auditor: edit / rm / git-diff
```

---

> **Disclaimer**
>
> This utility is provided “as is” without any warranties, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose, or non-infringement.
>
> The author assumes no liability for any direct, indirect, incidental, special, exemplary, or consequential damages arising from the use or inability to use this utility.
>
> Use this software at your own risk. You are solely responsible for any outcomes resulting from its application.

---

## What it does

- **Read-through overlay** — unpatched files are read straight from the origin
  (which may itself be another FUSE mount).
- **Patch tracking** — every accepted write is a full-file snapshot tagged
  with its writer, a global sequence, and the byte ranges it touched.
- **Soft-blocking** — a write whose ranges overlap another writer's change is
  rejected (`EAGAIN`) with a conflict diff on **stderr** and in
  `/fuse/messages/`; nothing is silently clobbered. Re-read and retry.
- **Resolution** — write the agreed full content to `/fuse/merge/<path>`; it
  becomes the merged snapshot served everywhere.
- **Auditor (patch-view)** — review the merged state, update it by editing,
  reject patches with `rm`, and apply git-diff patches via
  `/fuse/audit/<name>.patch`.
- **Apply-to-origin** — when the last mount unmounts, the surviving patches
  are written back into the origin; the store persists as the audit record.
- **Multi-layered stacks** — an overlay's lower can be another overlay mount;
  each layer has its own store and materializes into its lower on unmount.
- **Self-contained** — diff/merge/patch are all in-process (no external
  tools, no shell, no fork); a fully static build is available.

## Quick start

```bash
make                                   # -> build/fusing-patches

./build/fusing-patches /srv/origin /srv/store /mnt/editor --name=me -f &
printf 'hello\n' > /mnt/editor/notes.txt
cat /mnt/editor/notes.txt              # 'hello'
fusermount3 -u /mnt/editor             # patch applied to /srv/origin/notes.txt
```

Two writers, one shared store:

```bash
./build/fusing-patches /srv/origin /srv/store /mnt/alice --name=alice -f &
./build/fusing-patches /srv/origin /srv/store /mnt/bob   --name=bob   -f &

# alice commits; bob's overlapping write is soft-blocked:
#   close() -> EAGAIN, conflict diff on stderr + /mnt/bob/fuse/messages/
# bob re-reads, merges, retries; or both agree via:
printf 'resolved\n' > /mnt/bob/fuse/merge/notes.txt
```

## CLI

```
build/fusing-patches <lower> <store> <mountpoint> [--name=ID] [--merge|--patch-view] [fuse options]
```

| Kind           | Flag             | Purpose                                                             |
|--------------- |----------------- |-----------------------------------------------------                |
| writer         | `--name=ID`      | edit; patches are tracked; soft-blocked on conflict                 |
| merge view     | `--merge`        | read-only view of the agreed merged result                          |
| patch-view     | `--patch-view`   | auditor: edit (updates merged state), `rm` (reject), git-diff apply |

## Building

```bash
make          # dynamic build  -> build/fusing-patches
make static   # static build   -> build/fusing-patches-static (no runtime deps)
```

Both builds need only a C compiler — the FUSE layer is implemented in-process
against the kernel protocol (`linux/fuse.h`), no external libraries. Mounting
uses the system's `fusermount3` setuid helper (kernel mount privilege).

## Documentation

| Doc                       | Content                                                |
|-------------------------- |------------------------------------------------------- |
| `docs/ARCHITECTURE.md`    | concepts, store layout, conflict model, concurrency    |
| `docs/USAGE.md`           | workflows and CLI reference                            |
| `docs/SECURITY.md`        | security model, guarantees, known limitations          |
| `docs/DEVELOPMENT.md`     | build, code conventions, vendored dependencies         |
| `docs/TESTING.md`         | unit, integration and stress testing                   |
| `skills/fusing-patches/`  | agent guide with multi-layered mountpoint scenarios    |

## License

MIT — see `LICENSE`. Vendored code: `src/ht.h` (MIT/Public Domain) and the
murmur3 hash (public domain). No LGPL/GPL components are used.
