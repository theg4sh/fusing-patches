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
    int merge_view; /* read-only view of the agreed merged result */
};

/* per-open file handle */
struct fh {
    ps_session *sess; /* non-NULL for write handles */
    int fd;           /* lower/blob/merge/message fd (read) or session temp fd (write) */
    int write;
    int conflicted;   /* set by flush when the write was soft-blocked */
    char *relpath;
};

#define OVERLAY_DATA ((struct overlay *)fuse_get_context()->private_data)

static void lower_path_of(struct overlay *ov, const char *relpath, char *out, size_t sz) {
    snprintf(out, sz, "%s%s", ov->lower_dir, relpath);
}

/* ---- virtual /fuse namespace ---- */

enum vkind { V_NONE = 0, V_FUSE, V_MERGE_DIR, V_MSG_DIR, V_MERGE_FILE, V_MSG_FILE };

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
        dir_stat(st);
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
        return 0;
    case V_MERGE_DIR:
        ps_list_merge(ov->ps, "/", list_add_cb, &fc);
        return 0;
    case V_MSG_DIR:
        ps_list_messages(ov->ps, list_add_cb, &fc);
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

    if (ov->merge_view && is_write)
        return -EROFS;

    struct fh *h = calloc(1, sizeof *h);
    if (!h) return -ENOMEM;
    h->fd = -1;
    h->relpath = strdup(path);

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

    if (is_write && !exists && !(fi->flags & O_CREAT)) {
        free(h->relpath);
        free(h);
        return -ENOENT;
    }

    if (is_write) {
        h->write = 1;
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
    if (!h->write) return -EBADF;
    ssize_t w = ps_write(OVERLAY_DATA->ps, h->sess, buf, size, off);
    if (w < 0) return (int)w;
    return (int)w;
}

static int ov_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    struct fh *h = (struct fh *)(uintptr_t)fi->fh;
    if (!h) return 0;
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

    /* merge (resolution) writes are never soft-blocked */
    if (ps_session_is_merge(h->sess)) return 0;

    /* soft-block: report + reject the close, so the writer re-syncs */
    if (ps_check_conflict(OVERLAY_DATA->ps, h->sess)) {
        h->conflicted = 1;
        ps_report_conflict(OVERLAY_DATA->ps, h->sess);
        return -EAGAIN;
    }
    return 0;
}

static int ov_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)mode;
    struct overlay *ov = OVERLAY_DATA;

    char rest[4096];
    enum vkind kind = classify(path, rest, sizeof rest);

    if (ov->merge_view)
        return -EROFS;

    struct fh *h = calloc(1, sizeof *h);
    if (!h) return -ENOMEM;
    h->write = 1;
    h->relpath = strdup(path);

    if (kind == V_MERGE_FILE) {
        char relpath[4096];
        snprintf(relpath, sizeof relpath, "/%s", rest);
        h->sess = ps_begin_merge(ov->ps, relpath);
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

static void *ov_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    cfg->use_ino = 0;
    return OVERLAY_DATA;
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
    .access   = ov_access,
    .statfs   = ov_statfs,
    .init     = ov_init,
};

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s <lower> <store> <mountpoint> [--name=ID] [--merge] [fuse options]\n"
            "\n"
            "  lower        read-only source filesystem (a directory; may itself be a FUSE mount)\n"
            "  store        sidecar directory shared by all mountpoints\n"
            "  mountpoint   where this view is mounted\n"
            "  --name=ID    mountpoint identity (agent name); default 'agent'\n"
            "  --merge      mount a read-only view of the agreed merged result\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }

    const char *lower = argv[1];
    const char *store = argv[2];
    const char *mount = argv[3];

    const char *name = "agent";
    int merge_view = 0;

    /* pull our custom flags out of argv, keep the rest for libfuse */
    int nout = 0;
    char **new_argv = calloc(argc + 1, sizeof *new_argv);
    new_argv[nout++] = argv[0];
    new_argv[nout++] = (char *)mount;
    for (int i = 4; i < argc; i++) {
        if (strncmp(argv[i], "--name=", 7) == 0) {
            name = argv[i] + 7;
        } else if (strcmp(argv[i], "--merge") == 0) {
            merge_view = 1;
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
    ps_set_owner(ps, merge_view ? "merge" : name);

    struct overlay ov = { .ps = ps, .lower_dir = (char *)lower, .merge_view = merge_view };

    int rc = fuse_main(nout, new_argv, &ops, &ov);

    ps_close(ps);
    free(new_argv);
    return rc;
}
