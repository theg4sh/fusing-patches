#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "patchstore.h"

struct overlay {
    ps_store *ps;
    char *lower_dir;
    ps_kind kind; /* writer / merge / patchview */
};

#define ov_readonly(ov) ((ov)->kind == PS_KIND_MERGE)

/* per-open file handle */
struct fh {
    ps_session *sess; /* non-NULL for write handles */
    int fd;           /* lower/blob/merge/message fd (read) or session temp fd (write) */
    int write;
    int conflicted;   /* set by flush when the write was soft-blocked */
    int audit;        /* this handle accumulates a git-diff to apply */
    char *audit_tmp;  /* temp file holding the accumulated diff */
    char *relpath;
};

#define OVERLAY_DATA ((struct overlay *)fuse_get_context()->private_data)

static void lower_path_of(struct overlay *ov, const char *relpath, char *out, size_t sz) {
    snprintf(out, sz, "%s%s", ov->lower_dir, relpath);
}

/* ---- virtual /fuse namespace ---- */

enum vkind { V_NONE = 0, V_FUSE, V_MERGE_DIR, V_MSG_DIR, V_MERGE_FILE, V_MSG_FILE, V_AUDIT_DIR, V_AUDIT_FILE };

static enum vkind classify(const char *path, char *rest, size_t rest_sz) {
    if (strcmp(path, "/fuse") == 0) return V_FUSE;
    if (strncmp(path, "/fuse/merge", 11) == 0) {
        if (path[11] == '\0') return V_MERGE_DIR;
        if (path[11] == '/') {
            snprintf(rest, rest_sz, "%s", path + 12);
            return V_MERGE_FILE;
        }
    }
    if (strncmp(path, "/fuse/messages", 14) == 0) {
        if (path[14] == '\0') return V_MSG_DIR;
        if (path[14] == '/') {
            snprintf(rest, rest_sz, "%s", path + 15);
            return V_MSG_FILE;
        }
    }
    if (strncmp(path, "/fuse/audit", 11) == 0) {
        if (path[11] == '\0') return V_AUDIT_DIR;
        if (path[11] == '/') {
            snprintf(rest, rest_sz, "%s", path + 12);
            return V_AUDIT_FILE;
        }
    }
    return V_NONE;
}

static void dir_stat(struct stat *st) {
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = 2;
}

static int ov_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    struct overlay *ov = OVERLAY_DATA;

    char rest[4096];
    switch (classify(path, rest, sizeof rest)) {
    case V_FUSE:
    case V_MERGE_DIR:
    case V_MSG_DIR:
    case V_AUDIT_DIR:
        dir_stat(st);
        return 0;
    case V_AUDIT_FILE:
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        return 0;
    case V_MERGE_FILE: {
        char relpath[4096];
        snprintf(relpath, sizeof relpath, "/%s", rest);
        char blob[4096];
        if (ps_merge_resolve(ov->ps, relpath, blob, sizeof blob)) {
            if (lstat(blob, st) == 0) {
                st->st_mode = (st->st_mode & ~S_IFMT) | S_IFREG;
                return 0;
            }
            return -errno;
        }
        off_t asize;
        if (ps_active_size(ov->ps, relpath, &asize)) {
            memset(st, 0, sizeof *st);
            st->st_mode = S_IFREG | 0644;
            st->st_nlink = 1;
            st->st_size = asize;
            clock_gettime(CLOCK_REALTIME, &st->st_mtim);
            st->st_atim = st->st_ctim = st->st_mtim;
            return 0;
        }
        return -ENOENT;
    }
    case V_MSG_FILE: {
        char mp[4096];
        ps_message_path(ov->ps, rest, mp, sizeof mp);
        if (lstat(mp, st) == 0)
            return 0;
        return -errno;
    }
    default:
        break;
    }

    /* active (uncommitted) session -> synthesized regular file */
    off_t asize;
    if (ps_active_size(ov->ps, path, &asize)) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_size = asize;
        clock_gettime(CLOCK_REALTIME, &st->st_mtim);
        st->st_atim = st->st_ctim = st->st_mtim;
        return 0;
    }

    char blob[4096];
    int64_t seq;
    ps_src src = ps_resolve(ov->ps, path, blob, sizeof blob, &seq);
    if (src != PS_SRC_LOWER) {
        if (lstat(blob, st) == 0) {
            st->st_mode = (st->st_mode & ~S_IFMT) | S_IFREG;
            return 0;
        }
        return -errno;
    }

    char lp[4096];
    lower_path_of(ov, path, lp, sizeof lp);
    if (lstat(lp, st) == 0)
        return 0;
    return -errno;
}

struct fill_ctx {
    void *buf;
    fuse_fill_dir_t filler;
};

static void list_add_cb(void *ctx, const char *name) {
    struct fill_ctx *fc = ctx;
    struct stat st;
    memset(&st, 0, sizeof st);
    st.st_mode = S_IFREG | 0644;
    fc->filler(fc->buf, name, &st, 0, 0);
}

static void list_dir_add_cb(void *ctx, const char *name) {
    struct fill_ctx *fc = ctx;
    struct stat st;
    memset(&st, 0, sizeof st);
    st.st_mode = S_IFDIR | 0755;
    fc->filler(fc->buf, name, &st, 0, 0);
}

static int ov_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    struct overlay *ov = OVERLAY_DATA;
    struct fill_ctx fc = { buf, filler };

    char rest[4096];
    switch (classify(path, rest, sizeof rest)) {
    case V_FUSE:
        list_dir_add_cb(&fc, "merge");
        list_dir_add_cb(&fc, "messages");
        list_dir_add_cb(&fc, "audit");
        return 0;
    case V_MERGE_DIR:
        ps_list_merge(ov->ps, "/", list_add_cb, &fc);
        return 0;
    case V_MSG_DIR:
        ps_list_messages(ov->ps, list_add_cb, &fc);
        return 0;
    case V_AUDIT_DIR:
        return 0;
    default:
        break;
    }

    /* lower entries */
    char lp[4096];
    lower_path_of(ov, path, lp, sizeof lp);
    DIR *d = opendir(lp);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            struct stat st;
            memset(&st, 0, sizeof st);
            st.st_ino = e->d_ino;
            st.st_mode = DTTOIF(e->d_type);
            filler(buf, e->d_name, &st, 0, 0);
        }
        closedir(d);
    }

    /* patched / merged / new files */
    ps_list_dir(ov->ps, path, list_add_cb, &fc);

    /* expose the virtual namespace at the root */
    if (strcmp(path, "/") == 0)
        list_dir_add_cb(&fc, "fuse");

    return 0;
}

static int ov_open(const char *path, struct fuse_file_info *fi) {
    struct overlay *ov = OVERLAY_DATA;
    int accmode = fi->flags & O_ACCMODE;
    int is_write = accmode != O_RDONLY;

    char rest[4096];
    enum vkind kind = classify(path, rest, sizeof rest);

    if (is_write && kind == V_MSG_FILE)
        return -EACCES;

    if (ov_readonly(ov) && is_write)
        return -EROFS;

    struct fh *h = calloc(1, sizeof *h);
    if (!h) return -ENOMEM;
    h->fd = -1;
    h->relpath = strdup(path);

    if (kind == V_AUDIT_FILE) {
        /* a git-diff written here is applied on close */
        if (!is_write) {
            free(h->relpath);
            free(h);
            return -ENOENT;
        }
        char tpl[] = "/tmp/overlay-audit-XXXXXX";
        h->fd = mkstemp(tpl);
        if (h->fd < 0) {
            free(h->relpath);
            free(h);
            return -errno;
        }
        h->audit_tmp = strdup(tpl);
        h->audit = 1;
        h->write = 1;
        fi->fh = (uint64_t)(uintptr_t)h;
        return 0;
    }

    if (kind == V_MSG_FILE) {
        char mp[4096];
        ps_message_path(ov->ps, rest, mp, sizeof mp);
        h->fd = open(mp, O_RDONLY);
        if (h->fd < 0) {
            free(h->relpath);
            free(h);
            return -errno;
        }
        h->write = 0;
        fi->fh = (uint64_t)(uintptr_t)h;
        return 0;
    }

    if (kind == V_MERGE_FILE) {
        char relpath[4096];
        snprintf(relpath, sizeof relpath, "/%s", rest);
        if (is_write) {
            h->write = 1;
            h->sess = ps_begin_merge(ov->ps, relpath);
            if (!h->sess) {
                free(h->relpath);
                free(h);
                return -EIO;
            }
            h->fd = ps_session_fd(h->sess);
            if (fi->flags & O_TRUNC)
                ps_truncate(ov->ps, h->sess, 0);
        } else {
            char blob[4096];
            if (!ps_merge_resolve(ov->ps, relpath, blob, sizeof blob)) {
                free(h->relpath);
                free(h);
                return -ENOENT;
            }
            h->write = 0;
            h->fd = open(blob, O_RDONLY);
            if (h->fd < 0) {
                free(h->relpath);
                free(h);
                return -errno;
            }
        }
        fi->fh = (uint64_t)(uintptr_t)h;
        fi->direct_io = 1;
        return 0;
    }

    /* normal file */
    int exists = 0;
    char blob[4096];
    int64_t seq;
    if (ps_resolve(ov->ps, path, blob, sizeof blob, &seq) != PS_SRC_LOWER) {
        exists = 1;
    } else {
        char lp[4096];
        lower_path_of(ov, path, lp, sizeof lp);
        struct stat st;
        if (lstat(lp, &st) == 0) {
            if (S_ISREG(st.st_mode)) exists = 1;
        } else {
            off_t asize;
            if (ps_active_size(ov->ps, path, &asize)) exists = 1;
        }
    }

    /* The kernel stripped O_CREAT because its LOOKUP saw the file, but a
     * concurrent commit can briefly make it invisible (session removed /
     * patch not yet visible). Absorb that window: retry, then treat the
     * write-open as a create instead of failing with ENOENT. */
    if (is_write && !exists && !(fi->flags & O_CREAT)) {
        for (int i = 0; i < 20 && !exists; i++) {
            usleep(500);
            if (ps_resolve(ov->ps, path, blob, sizeof blob, &seq) != PS_SRC_LOWER) {
                exists = 1;
            } else {
                off_t asize;
                if (ps_active_size(ov->ps, path, &asize)) exists = 1;
            }
        }
        /* still invisible -> proceed as a fresh create (caller had O_CREAT) */
    }

    if (is_write) {
        h->write = 1;
        if (ov->kind == PS_KIND_PATCHVIEW)
            h->sess = ps_begin_merge_seeded(ov->ps, path);
        else
            h->sess = ps_begin(ov->ps, path, exists);
        if (!h->sess) {
            free(h->relpath);
            free(h);
            return -EIO;
        }
        h->fd = ps_session_fd(h->sess);
        if (fi->flags & O_TRUNC)
            ps_truncate(ov->ps, h->sess, 0);
    } else {
        h->write = 0;
        char lp[4096];
        const char *p = blob;
        if (ps_resolve(ov->ps, path, blob, sizeof blob, &seq) == PS_SRC_LOWER) {
            lower_path_of(ov, path, lp, sizeof lp);
            p = lp;
        }
        h->fd = open(p, O_RDONLY);
        if (h->fd < 0) {
            free(h->relpath);
            free(h);
            return -errno;
        }
    }

    fi->fh = (uint64_t)(uintptr_t)h;
    fi->direct_io = 1;
    return 0;
}

static int ov_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
    (void)path;
    struct fh *h = (struct fh *)(uintptr_t)fi->fh;
    ssize_t r = pread(h->fd, buf, size, off);
    if (r < 0) return -errno;
    return (int)r;
}

static int ov_write(const char *path, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
    (void)path;
    struct fh *h = (struct fh *)(uintptr_t)fi->fh;
    if (h->audit) {
        ssize_t w = pwrite(h->fd, buf, size, off);
        if (w < 0) return -errno;
        return (int)w;
    }
    if (!h->write) return -EBADF;
    ssize_t w = ps_write(OVERLAY_DATA->ps, h->sess, buf, size, off);
    if (w < 0) return (int)w;
    return (int)w;
}

static int ov_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    struct fh *h = (struct fh *)(uintptr_t)fi->fh;
    if (!h) return 0;

    if (h->audit) {
        /* accumulated git-diff -> apply it now */
        struct stat st;
        if (fstat(h->fd, &st) == 0 && st.st_size > 0) {
            char *buf = malloc((size_t)st.st_size + 1);
            if (buf) {
                lseek(h->fd, 0, SEEK_SET);
                ssize_t r = read(h->fd, buf, (size_t)st.st_size);
                if (r > 0) {
                    buf[r] = '\0';
                    int applied = ps_apply_gitdiff(OVERLAY_DATA->ps, buf);
                    fprintf(stderr, "[overlay] audit: %d file(s) updated\n", applied);
                }
                free(buf);
            }
        }
        close(h->fd);
        if (h->audit_tmp) {
            unlink(h->audit_tmp);
            free(h->audit_tmp);
        }
        free(h->relpath);
        free(h);
        return 0;
    }

    if (h->write) {
        if (h->sess) {
            if (h->conflicted) {
                /* already reported by flush; just drop the session */
                ps_abort(OVERLAY_DATA->ps, h->sess);
            } else {
                /* flush may not have run the check; commit reports if needed */
                ps_commit(OVERLAY_DATA->ps, h->sess);
            }
            h->sess = NULL;
        }
    } else if (h->fd >= 0) {
        close(h->fd);
    }
    free(h->relpath);
    free(h);
    return 0;
}

static int ov_flush(const char *path, struct fuse_file_info *fi) {
    (void)path;
    struct fh *h = (struct fh *)(uintptr_t)fi->fh;
    if (!h || !h->write || !h->sess) return 0;

    if (!ps_session_dirty(h->sess)) return 0;

    /* a flush with no writes yet is not the end of the edit (e.g. shell
     * redirection dup2/close); keep the session for the writes to come */
    if (!ps_session_written(h->sess)) return 0;

    /* merge (resolution) writes commit here; close() waits for flush */
    if (ps_session_is_merge(h->sess)) {
        ps_commit(OVERLAY_DATA->ps, h->sess);
        h->sess = NULL;
        return 0;
    }

    /* soft-block: report + reject the close, so the writer re-syncs */
    if (ps_check_conflict(OVERLAY_DATA->ps, h->sess)) {
        h->conflicted = 1;
        ps_report_conflict(OVERLAY_DATA->ps, h->sess);
        return -EAGAIN;
    }

    /* commit synchronously: close() waits for flush, so the next open on
     * this mount observes the committed state (no async-release window) */
    ps_commit(OVERLAY_DATA->ps, h->sess);
    h->sess = NULL;
    return 0;
}

static int ov_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)mode;
    struct overlay *ov = OVERLAY_DATA;

    char rest[4096];
    enum vkind kind = classify(path, rest, sizeof rest);

    if (ov_readonly(ov))
        return -EROFS;

    struct fh *h = calloc(1, sizeof *h);
    if (!h) return -ENOMEM;
    h->write = 1;
    h->fd = -1;
    h->relpath = strdup(path);

    if (kind == V_AUDIT_FILE) {
        char tpl[] = "/tmp/overlay-audit-XXXXXX";
        h->fd = mkstemp(tpl);
        if (h->fd < 0) {
            free(h->relpath);
            free(h);
            return -errno;
        }
        h->audit_tmp = strdup(tpl);
        h->audit = 1;
        fi->fh = (uint64_t)(uintptr_t)h;
        fi->direct_io = 1;
        return 0;
    }

    if (kind == V_MERGE_FILE) {
        char relpath[4096];
        snprintf(relpath, sizeof relpath, "/%s", rest);
        h->sess = ps_begin_merge(ov->ps, relpath);
    } else if (ov->kind == PS_KIND_PATCHVIEW) {
        h->sess = ps_begin_merge_seeded(ov->ps, path);
    } else {
        h->sess = ps_begin(ov->ps, path, 0 /* new file */);
    }

    if (!h->sess) {
        free(h->relpath);
        free(h);
        return -EIO;
    }
    h->fd = ps_session_fd(h->sess);
    fi->fh = (uint64_t)(uintptr_t)h;
    fi->direct_io = 1;
    return 0;
}

static int ov_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    struct overlay *ov = OVERLAY_DATA;
    struct fh *h = fi ? (struct fh *)(uintptr_t)fi->fh : NULL;
    if (h && h->write && h->sess)
        return ps_truncate(ov->ps, h->sess, size);

    /* standalone truncate: begin a session, truncate, commit */
    char rest[4096];
    enum vkind kind = classify(path, rest, sizeof rest);

    ps_session *s;
    if (kind == V_MERGE_FILE) {
        char relpath[4096];
        snprintf(relpath, sizeof relpath, "/%s", rest);
        s = ps_begin_merge(ov->ps, relpath);
    } else {
        int exists = 0;
        char blob[4096];
        int64_t seq;
        if (ps_resolve(ov->ps, path, blob, sizeof blob, &seq) != PS_SRC_LOWER) {
            exists = 1;
        } else {
            char lp[4096];
            lower_path_of(ov, path, lp, sizeof lp);
            struct stat st;
            if (lstat(lp, &st) == 0 && S_ISREG(st.st_mode)) exists = 1;
        }
        s = ps_begin(ov->ps, path, exists);
    }
    if (!s) return -EIO;
    ps_truncate(ov->ps, s, size);
    int rc = ps_commit(ov->ps, s);
    if (rc < 0) return -EIO;
    return 0;
}

static int ov_access(const char *path, int mask) {
    (void)mask;
    struct stat st;
    if (ov_getattr(path, &st, NULL) == 0)
        return 0;
    return -errno;
}

static int ov_statfs(const char *path, struct statvfs *st) {
    (void)path;
    struct overlay *ov = OVERLAY_DATA;
    char lp[4096];
    lower_path_of(ov, "/", lp, sizeof lp);
    if (statvfs(lp, st) == 0)
        return 0;
    return -errno;
}

/* remove (reject) a file's patches -> it reverts to origin content */
static int ov_unlink(const char *path) {
    struct overlay *ov = OVERLAY_DATA;
    if (ov_readonly(ov)) return -EROFS;
    char rest[4096];
    if (classify(path, rest, sizeof rest) != V_NONE) return -EACCES;
    ps_reject(ov->ps, path);
    return 0;
}

static void *ov_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    cfg->use_ino = 0;
    /* no dentry/attribute caching: every open re-validates against the
     * store, avoiding stale-lookup races with concurrent commits */
    cfg->entry_timeout = 0;
    cfg->negative_timeout = 0;
    cfg->attr_timeout = 0;
    struct overlay *ov = OVERLAY_DATA;
    ps_mount_register(ov->ps, ov->kind);
    return ov;
}

static void ov_destroy(void *priv) {
    struct overlay *ov = priv;
    if (!ov) return;
    if (ps_mount_unregister(ov->ps, ov->kind)) {
        int n = ps_apply_to_origin(ov->ps);
        fprintf(stderr, "[overlay] last non-merge mount gone: %d file(s) applied to origin\n", n);
    }
}

static struct fuse_operations ops = {
    .getattr  = ov_getattr,
    .readdir  = ov_readdir,
    .open     = ov_open,
    .read     = ov_read,
    .write    = ov_write,
    .release  = ov_release,
    .flush    = ov_flush,
    .create   = ov_create,
    .truncate = ov_truncate,
    .unlink   = ov_unlink,
    .access   = ov_access,
    .statfs   = ov_statfs,
    .init     = ov_init,
    .destroy  = ov_destroy,
};

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s <lower> <store> <mountpoint> [--name=ID] [--merge|--patch-view] [fuse options]\n"
            "\n"
            "  lower        read-only source filesystem (a directory; may itself be a FUSE mount)\n"
            "  store        sidecar directory shared by all mountpoints\n"
            "  mountpoint   where this view is mounted\n"
            "  --name=ID    mountpoint identity (agent name); default 'agent'\n"
            "  --merge      read-only view of the agreed merged result\n"
            "  --patch-view auditor's view: writable (edits update merged state), rm = reject,\n"
            "               /fuse/audit/<n>.patch applies a (multi-file) git-diff\n", prog);
}

int main(int argc, char *argv[]) {
    /* store contents (patches, messages) may hold file data: create
     * everything 0600/0700 regardless of the caller's umask */
    umask(0077);

    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }

    const char *lower = argv[1];
    const char *store = argv[2];
    const char *mount = argv[3];

    const char *name = "agent";
    ps_kind kind = PS_KIND_WRITER;

    /* pull our custom flags out of argv, keep the rest for libfuse */
    int nout = 0;
    char **new_argv = calloc(argc + 1, sizeof *new_argv);
    new_argv[nout++] = argv[0];
    new_argv[nout++] = (char *)mount;
    for (int i = 4; i < argc; i++) {
        if (strncmp(argv[i], "--name=", 7) == 0) {
            name = argv[i] + 7;
        } else if (strcmp(argv[i], "--merge") == 0) {
            kind = PS_KIND_MERGE;
        } else if (strcmp(argv[i], "--patch-view") == 0) {
            kind = PS_KIND_PATCHVIEW;
        } else {
            new_argv[nout++] = argv[i];
        }
    }
    new_argv[nout] = NULL;

    ps_store *ps = ps_open(store, lower);
    if (!ps) {
        perror("ps_open");
        free(new_argv);
        return 1;
    }
    ps_set_owner(ps, kind == PS_KIND_WRITER ? name : kind == PS_KIND_MERGE ? "merge" : "patchview");

    struct overlay ov = { .ps = ps, .lower_dir = (char *)lower, .kind = kind };

    int rc = fuse_main(nout, new_argv, &ops, &ov);

    ps_close(ps);
    free(new_argv);
    return rc;
}
