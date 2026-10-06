#ifndef PATCHSTORE_H
#define PATCHSTORE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Patch store v2: sidecar directory shared by every FUSE overlay mountpoint
 * sitting on top of the same read-only lower filesystem.
 *
 * Every committed change (a "patch") and every agreed resolution (a "merge")
 * carries a globally monotonic sequence number. A mountpoint tracks the last
 * state it has incorporated; a write whose base is older than another writer's
 * committed change is accepted but reported as a conflict, so the writer can
 * resolve it by writing the agreed content under /fuse/merge/.
 *
 * Layout:
 *   <store>/
 *     seq                 global monotonic counter (flock-guarded)
 *     blobs/<id>.blob     committed patch snapshots
 *     meta/<enc-path>     line-based patch index (one file per path)
 *     merge/<enc>.blob    agreed merged snapshot
 *     merge/<enc>.meta    "M <seq> <size> <owner>"
 *     mounts/<owner>      last incorporated seq for a mountpoint
 *     messages/<owner>/<seq>.msg   resolution messages for a mountpoint
 *     tmp/                transient buffers
 */

typedef struct ps_store ps_store;
typedef struct ps_session ps_session;

/* Where the current content of a file comes from. */
typedef enum {
    PS_SRC_LOWER = 0, /* unchanged, read from lower */
    PS_SRC_PATCH = 1, /* a committed (accepted) patch blob */
    PS_SRC_MERGE = 2, /* the agreed merged snapshot */
} ps_src;

ps_store *ps_open(const char *store_dir, const char *lower_dir);
void ps_close(ps_store *ps);

/* Identity of this mountpoint ("merge" for the merge view). */
void ps_set_owner(ps_store *ps, const char *owner);

/* Begin a normal write session. `exists` seeds from current content. */
ps_session *ps_begin(ps_store *ps, const char *relpath, int exists);
/* Begin a merge session (write under /fuse/merge/<relpath>). */
ps_session *ps_begin_merge(ps_store *ps, const char *relpath);
/* Begin a merge session seeded from current content (patch-view edits). */
ps_session *ps_begin_merge_seeded(ps_store *ps, const char *relpath);

ssize_t ps_write(ps_store *ps, ps_session *s, const char *buf, size_t size, off_t off);
ssize_t ps_read(ps_store *ps, ps_session *s, char *buf, size_t size, off_t off);
int ps_truncate(ps_store *ps, ps_session *s, off_t size);
int ps_session_fd(ps_session *s);
int ps_session_dirty(ps_session *s);
/* Non-zero if actual write()s happened (truncate alone does not count). */
int ps_session_written(ps_session *s);
/* Non-zero if this session writes to the merge snapshot (resolution). */
int ps_session_is_merge(ps_session *s);

/*
 * Finalize the session:
 *   return  0 -> committed (clean accept or merge)
 *   return -2 -> soft-blocked: the write's byte ranges overlap another
 *                writer's change; nothing was committed and a conflict
 *                message was written (stderr + /fuse/messages/)
 *   return -1 -> internal error
 */
int ps_commit(ps_store *ps, ps_session *s);
void ps_abort(ps_store *ps, ps_session *s);

/* Non-destructive: 1 if committing this session now would conflict. */
int ps_check_conflict(ps_store *ps, ps_session *s);

/* Emit the conflict message (stderr + /fuse/messages/) for a session, with
 * per-(owner,file) de-duplication so a message is only produced when the
 * conflict extends a previously reported range. No store content is changed. */
void ps_report_conflict(ps_store *ps, ps_session *s);

/*
 * Resolve current content of `relpath`:
 *   returns the ps_src; for PATCH/MERGE writes *blob_out and *seq_out.
 *   For LOWER, *seq_out = 0.
 */
ps_src ps_resolve(ps_store *ps, const char *relpath, char *blob_out, size_t blob_out_sz,
                  int64_t *seq_out);

/* If `relpath` has merged content, write its blob path and return 1. */
int ps_merge_resolve(ps_store *ps, const char *relpath, char *blob_out, size_t blob_out_sz);

/* If `relpath` has an active (uncommitted) session, write its size and return 1. */
int ps_active_size(ps_store *ps, const char *relpath, off_t *size_out);

typedef void (*ps_list_cb)(void *ctx, const char *name);
/* Entries (lower + accepted patches + merges + active) directly under `dir`. */
void ps_list_dir(ps_store *ps, const char *dir, ps_list_cb cb, void *ctx);
/* Merged files directly under `dir`. */
void ps_list_merge(ps_store *ps, const char *dir, ps_list_cb cb, void *ctx);
/* Pending resolution-message files for this mountpoint's owner. */
void ps_list_messages(ps_store *ps, ps_list_cb cb, void *ctx);
/* Full path of a message file given its basename. Returns 0/-1. */
int ps_message_path(ps_store *ps, const char *name, char *out, size_t out_sz);

/* ------------------------------------------------------------------ */
/* multi-level mounts, patch-view + apply-to-origin                     */
/* ------------------------------------------------------------------ */

/* Kind of a mountpoint. */
typedef enum {
    PS_KIND_WRITER = 0, /* editing mount (default) */
    PS_KIND_MERGE = 1,  /* read-only merged-result view */
    PS_KIND_PATCHVIEW = 2, /* auditor's view: writable, removable */
} ps_kind;

/* Register this mountpoint in the store's active-mount registry. */
void ps_mount_register(ps_store *ps, ps_kind kind);
/* Unregister; returns 1 when no non-merge mount remains (apply time). */
int ps_mount_unregister(ps_store *ps, ps_kind kind);

/* Materialize every un-rejected patch/merge into the origin (lower) fs.
 * Returns the number of files written, or -1 on error. */
int ps_apply_to_origin(ps_store *ps);

/* 1 if the lower/origin content changed since we snapshotted our base. */
int ps_origin_changed(ps_store *ps, const char *relpath);

/* For every patched file whose origin changed behind our stack, emit a 3-way
 * diff-conflict (diff3 -m base/theirs/ours) to stderr + messages/. Returns the
 * number of conflicting files. */
int ps_report_origin_conflicts(ps_store *ps);

/* Reject `relpath`: clear all patch/merge state so it reverts to origin. */
int ps_reject(ps_store *ps, const char *relpath);

/* Apply a (possibly multi-file) unified/git diff. Each file's hunks are split
 * by path and applied to that file's merged state using the normal conflict
 * rules. On conflict the real 3-way diff-conflict (with markers) is printed to
 * stderr and stored under messages/. Returns 0 on success, -1 on error. */
int ps_apply_gitdiff(ps_store *ps, const char *diff_text);

#endif /* PATCHSTORE_H */
