/* Minimal FUSE userspace layer over /dev/fuse (no libfuse).
 *
 * Single-threaded request loop: requests are processed to completion one at a
 * time, so no intra-daemon races exist by construction (cross-agent
 * concurrency comes from separate processes). No kernel entry/attr caching is
 * requested (entry_valid/attr_valid = 0), so every open re-validates against
 * the store. */
#define _GNU_SOURCE 1
#include "fuse.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define HT_PUBDEF static
#define HT_IMPLEMENTATION 1
#include "hash.h"
#pragma GCC diagnostic pop

static int g_fd = -1;
static const struct fuse_operations *g_ops;
static struct fuse_context g_ctx;
static volatile sig_atomic_t g_stop;

const struct fuse_context *fuse_get_context(void) {
    return &g_ctx;
}

static const char *g_mnt;
static uint32_t g_max_write = 1024 * 1024;

/* ---- inode table: nodeid <-> path ---- */
static struct node {
    char *path;
} *g_nodes;
static int g_nnodes, g_nnodes_cap;

typedef Ht(const char *, uint64_t) inode_ht;
static inode_ht g_path2ino = { .hasheq = str_hasheq };

static uint64_t nodeid_for_path(const char *path) {
    uint64_t *p = ht_find(&g_path2ino, path);
    if (p) return *p;
    if (g_nnodes + 1 >= g_nnodes_cap) {
        g_nnodes_cap = g_nnodes_cap ? g_nnodes_cap * 2 : 64;
        g_nodes = realloc(g_nodes, sizeof(*g_nodes) * (size_t)g_nnodes_cap);
    }
    g_nnodes++;
    g_nodes[g_nnodes].path = strdup(path);
    uint64_t *v = ht_put(&g_path2ino, g_nodes[g_nnodes].path);
    if (v) *v = (uint64_t)g_nnodes;
    return (uint64_t)g_nnodes;
}

static char *path_by_nodeid(uint64_t nodeid) {
    if (nodeid <= (uint64_t)g_nnodes && nodeid >= 1)
        return g_nodes[nodeid].path;
    return NULL;
}

static char *child_path(uint64_t parent, const char *name) {
    const char *pp = path_by_nodeid(parent);
    if (!pp) pp = "/";
    if (strcmp(name, ".") == 0) return strdup(pp);
    if (strcmp(name, "..") == 0) {
        if (strcmp(pp, "/") == 0) return strdup("/");
        const char *slash = strrchr(pp, '/');
        size_t len = (size_t)(slash - pp);
        char *d = malloc(len + 2);
        memcpy(d, pp, len);
        if (len == 0) {
            d[0] = '/';
            d[1] = '\0';
        } else {
            d[len] = '\0';
        }
        return d;
    }
    size_t pl = strlen(pp), nl = strlen(name);
    char *r = malloc(pl + nl + 2);
    if (strcmp(pp, "/") == 0)
        sprintf(r, "/%s", name);
    else
        sprintf(r, "%s/%s", pp, name);
    return r;
}

/* ---- reply helpers ---- */

static int reply_err(uint64_t unique, int err) {
    struct fuse_out_header oh = {
        .len = sizeof oh,
        .error = -err,
        .unique = unique,
    };
    return write(g_fd, &oh, sizeof oh);
}

static int reply_raw(uint64_t unique, int err, const void *data, size_t len) {
    struct fuse_out_header oh = {
        .len = sizeof oh + (uint32_t)len,
        .error = -err,
        .unique = unique,
    };
    struct iovec iov[2] = { { &oh, sizeof oh }, { (void *)data, len } };
    return writev(g_fd, iov, 2);
}

static void fill_attr(struct fuse_attr *fa, const struct stat *st) {
    memset(fa, 0, sizeof *fa);
    fa->ino = st->st_ino;
    fa->size = st->st_size;
    fa->blocks = st->st_blocks;
    fa->atime = st->st_atime;
    fa->mtime = st->st_mtime;
    fa->ctime = st->st_ctime;
    fa->atimensec = (uint32_t)st->st_atim.tv_nsec;
    fa->mtimensec = (uint32_t)st->st_mtim.tv_nsec;
    fa->ctimensec = (uint32_t)st->st_ctim.tv_nsec;
    fa->mode = st->st_mode;
    fa->nlink = st->st_nlink;
    fa->uid = st->st_uid;
    fa->gid = st->st_gid;
    fa->rdev = st->st_rdev;
    fa->blksize = st->st_blksize;
}

static int attr_reply(uint64_t unique, const char *path, uint64_t fh) {
    struct fuse_attr_out out;
    memset(&out, 0, sizeof out);
    struct fuse_file_info fi = { .fh = fh, .flags = 0 };
    struct stat st;
    int rc = g_ops->getattr(path, &st, fh ? &fi : NULL);
    if (rc != 0) return reply_err(unique, -rc);
    fill_attr(&out.attr, &st);
    return reply_raw(unique, 0, &out, sizeof out);
}

/* ---- readdir ---- */

struct dirbuf {
    char *buf;
    size_t size;
    size_t used;
    uint64_t pos;   /* sequential entry index (also the "offset" space) */
    uint64_t start; /* skip entries with index < start */
};

static int fill_dir(void *vdb, const char *name, const struct stat *st, off_t off, int flags) {
    (void)off;
    (void)flags;
    struct dirbuf *db = vdb;
    db->pos++;
    if (db->pos <= db->start) return 0; /* already returned */
    size_t namelen = strlen(name);
    size_t entsize = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + namelen);
    if (db->used + entsize > db->size) return 1; /* buffer full */
    struct fuse_dirent *de = (struct fuse_dirent *)(db->buf + db->used);
    memset(de, 0, entsize);
    de->ino = st ? st->st_ino : 0;
    de->off = db->pos + 1;
    de->namelen = (uint32_t)namelen;
    de->type = st ? (st->st_mode >> 12) : DT_UNKNOWN;
    memcpy(de->name, name, namelen);
    db->used += entsize;
    return 0;
}

/* ---- mount / unmount via the fusermount3 helper ---- */

static int receive_fd(int sock) {
    char buf[16];
    struct iovec iov = { buf, sizeof buf };
    char ccmsg[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ccmsg;
    msg.msg_controllen = sizeof ccmsg;
    if (recvmsg(sock, &msg, 0) < 0) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) return -1;
    int fd = -1;
    memcpy(&fd, CMSG_DATA(c), sizeof fd);
    return fd;
}

static int mount_fuse(const char *mnt) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[1]);
        char env[64];
        snprintf(env, sizeof env, "_FUSE_COMMFD=%d", sv[0]);
        char *e[] = { env, NULL };
        char *argv[] = { "fusermount3", "-o", "rw,nosuid,nodev", "--", (char *)mnt, NULL };
        execvpe("fusermount3", argv, e);
        _exit(127);
    }
    close(sv[0]);
    int fd = receive_fd(sv[1]);
    close(sv[1]);
    waitpid(pid, NULL, 0);
    return fd;
}

static void unmount_fuse(const char *mnt) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("fusermount3", "fusermount3", "-u", "-q", "-z", "--", mnt, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ---- dispatcher ---- */

static void dispatch(const char *buf, size_t len) {
    (void)len;
    const struct fuse_in_header *in = (const struct fuse_in_header *)buf;
    const char *payload = buf + sizeof *in;

    g_ctx.uid = in->uid;
    g_ctx.gid = in->gid;
    g_ctx.pid = in->pid;

    switch (in->opcode) {
    case FUSE_INIT: {
        const struct fuse_init_in *arg = (const struct fuse_init_in *)payload;
        struct fuse_init_out out;
        memset(&out, 0, sizeof out);
        out.major = FUSE_KERNEL_VERSION;
        out.minor = arg->minor < FUSE_KERNEL_MINOR_VERSION ? arg->minor : FUSE_KERNEL_MINOR_VERSION;
        out.max_readahead = g_max_write;
        out.max_write = g_max_write;
        out.max_background = 1;
        out.congestion_threshold = 1;
        out.time_gran = 1;
        out.max_pages = 256;
        out.flags = FUSE_ATOMIC_O_TRUNC |
                    FUSE_MAX_PAGES | FUSE_NO_OPENDIR_SUPPORT;
        reply_raw(in->unique, 0, &out, sizeof out);

        struct fuse_conn_info ci = {
            .proto_major = FUSE_KERNEL_VERSION,
            .proto_minor = out.minor,
            .max_write = out.max_write,
            .max_readahead = out.max_readahead,
        };
        struct fuse_config cfg;
        memset(&cfg, 0, sizeof cfg);
        if (g_ops->init)
            g_ctx.private_data = g_ops->init(&ci, &cfg);
        nodeid_for_path("/"); /* root */
        break;
    }

    case FUSE_DESTROY:
        g_stop = 1;
        break; /* no reply */

    case FUSE_FORGET:
    case FUSE_BATCH_FORGET:
    case FUSE_INTERRUPT:
        break; /* no reply */

    case FUSE_LOOKUP: {
        const char *name = payload;
        char *path = child_path(in->nodeid, name);
        struct fuse_entry_out out;
        memset(&out, 0, sizeof out); /* entry_valid/attr_valid = 0: no caching */
        out.nodeid = nodeid_for_path(path);
        struct stat st;
        int rc = g_ops->getattr(path, &st, NULL);
        if (rc == 0) {
            fill_attr(&out.attr, &st);
            reply_raw(in->unique, 0, &out, sizeof out);
        } else {
            reply_err(in->unique, -rc);
        }
        free(path);
        break;
    }

    case FUSE_GETATTR: {
        const struct fuse_getattr_in *arg = (const struct fuse_getattr_in *)payload;
        const char *path = path_by_nodeid(in->nodeid);
        if (!path) {
            reply_err(in->unique, ENOENT);
            break;
        }
        attr_reply(in->unique, path, arg->fh);
        break;
    }

    case FUSE_SETATTR: {
        const struct fuse_setattr_in *arg = (const struct fuse_setattr_in *)payload;
        const char *path = path_by_nodeid(in->nodeid);
        if (!path) {
            reply_err(in->unique, ENOENT);
            break;
        }
        if (arg->valid & FATTR_SIZE) {
            struct fuse_file_info fi = { .fh = arg->fh, .flags = 0 };
            int rc = g_ops->truncate(path, (off_t)arg->size, arg->fh ? &fi : NULL);
            if (rc != 0) {
                reply_err(in->unique, -rc);
                break;
            }
        }
        attr_reply(in->unique, path, arg->fh);
        break;
    }

    case FUSE_OPEN: {
        const struct fuse_open_in *arg = (const struct fuse_open_in *)payload;
        const char *path = path_by_nodeid(in->nodeid);
        if (!path) {
            reply_err(in->unique, ENOENT);
            break;
        }
        struct fuse_file_info fi = { .fh = 0, .flags = (int)arg->flags };
        int rc = g_ops->open(path, &fi);
        if (rc != 0) {
            reply_err(in->unique, -rc);
            break;
        }
        struct fuse_open_out out = { .fh = fi.fh, .open_flags = FOPEN_DIRECT_IO };
        reply_raw(in->unique, 0, &out, sizeof out);
        break;
    }

    case FUSE_CREATE: {
        const struct fuse_create_in *arg = (const struct fuse_create_in *)payload;
        const char *name = payload + sizeof *arg;
        char *path = child_path(in->nodeid, name);
        struct fuse_file_info fi = { .fh = 0, .flags = (int)arg->flags };
        int rc = g_ops->create(path, arg->mode, &fi);
        if (rc != 0) {
            reply_err(in->unique, -rc);
            free(path);
            break;
        }
        struct fuse_entry_out eo;
        memset(&eo, 0, sizeof eo);
        eo.nodeid = nodeid_for_path(path);
        struct stat st;
        if (g_ops->getattr(path, &st, NULL) == 0)
            fill_attr(&eo.attr, &st);
        struct fuse_open_out oo = { .fh = fi.fh, .open_flags = FOPEN_DIRECT_IO };
        char out[sizeof eo + sizeof oo];
        memcpy(out, &eo, sizeof eo);
        memcpy(out + sizeof eo, &oo, sizeof oo);
        reply_raw(in->unique, 0, out, sizeof out);
        free(path);
        break;
    }

    case FUSE_READ: {
        const struct fuse_read_in *arg = (const struct fuse_read_in *)payload;
        const char *path = path_by_nodeid(in->nodeid);
        if (!path) {
            reply_err(in->unique, ENOENT);
            break;
        }
        struct fuse_file_info fi = { .fh = arg->fh, .flags = 0 };
        struct fuse_out_header oh = {
            .unique = in->unique,
            .len = sizeof oh,
            .error = 0,
        };
        char *data = malloc(arg->size);
        int rc = g_ops->read(path, data, arg->size, (off_t)arg->offset, &fi);
        if (rc < 0) {
            free(data);
            reply_err(in->unique, -rc);
            break;
        }
        oh.len = sizeof oh + (uint32_t)rc;
        struct iovec iov[2] = { { &oh, sizeof oh }, { data, (size_t)rc } };
        writev(g_fd, iov, 2);
        free(data);
        break;
    }

    case FUSE_WRITE: {
        const struct fuse_write_in *arg = (const struct fuse_write_in *)payload;
        const char *path = path_by_nodeid(in->nodeid);
        if (!path) {
            reply_err(in->unique, ENOENT);
            break;
        }
        const char *data = buf + in->len - arg->size;
        struct fuse_file_info fi = { .fh = arg->fh, .flags = 0 };
        int rc = g_ops->write(path, data, arg->size, (off_t)arg->offset, &fi);
        if (rc < 0) {
            reply_err(in->unique, -rc);
            break;
        }
        struct fuse_write_out out = { .size = (uint32_t)rc };
        reply_raw(in->unique, 0, &out, sizeof out);
        break;
    }

    case FUSE_FLUSH: {
        const struct fuse_flush_in *arg = (const struct fuse_flush_in *)payload;
        const char *path = path_by_nodeid(in->nodeid);
        if (!path) {
            reply_err(in->unique, ENOENT);
            break;
        }
        struct fuse_file_info fi = { .fh = arg->fh, .flags = 0 };
        int rc = g_ops->flush(path, &fi);
        reply_err(in->unique, -rc);
        break;
    }

    case FUSE_RELEASE: {
        const struct fuse_release_in *arg = (const struct fuse_release_in *)payload;
        const char *path = path_by_nodeid(in->nodeid);
        if (!path) {
            reply_err(in->unique, ENOENT);
            break;
        }
        struct fuse_file_info fi = { .fh = arg->fh, .flags = 0 };
        int rc = g_ops->release(path, &fi);
        reply_err(in->unique, -rc);
        break;
    }

    case FUSE_READDIR: {
        /* 7.40+: the readdir request carries struct fuse_read_in */
        const struct fuse_read_in *arg = (const struct fuse_read_in *)payload;
        const char *path = path_by_nodeid(in->nodeid);
        if (!path) {
            reply_err(in->unique, ENOENT);
            break;
        }
        struct dirbuf db;
        db.size = arg->size;
        db.buf = malloc(arg->size);
        db.used = 0;
        db.pos = 0;
        db.start = arg->offset;
        struct fuse_file_info fi = { .fh = arg->fh, .flags = 0 };
        int rc = g_ops->readdir(path, &db, fill_dir, (off_t)arg->offset, &fi, 0);
        if (rc != 0) {
            free(db.buf);
            reply_err(in->unique, -rc);
            break;
        }
        reply_raw(in->unique, 0, db.buf, db.used);
        free(db.buf);
        break;
    }

    case FUSE_UNLINK: {
        const char *name = payload;
        char *path = child_path(in->nodeid, name);
        int rc = g_ops->unlink(path);
        free(path);
        reply_err(in->unique, -rc);
        break;
    }

    case FUSE_ACCESS: {
        const struct fuse_access_in *arg = (const struct fuse_access_in *)payload;
        const char *path = path_by_nodeid(in->nodeid);
        if (!path) {
            reply_err(in->unique, ENOENT);
            break;
        }
        int rc = g_ops->access(path, arg->mask);
        reply_err(in->unique, -rc);
        break;
    }

    case FUSE_STATFS: {
        const char *path = path_by_nodeid(in->nodeid);
        if (!path) {
            reply_err(in->unique, ENOENT);
            break;
        }
        struct fuse_statfs_out out;
        memset(&out, 0, sizeof out);
        struct statvfs sv;
        int rc = g_ops->statfs(path, &sv);
        if (rc != 0) {
            reply_err(in->unique, -rc);
            break;
        }
        out.st.blocks = sv.f_blocks;
        out.st.bfree = sv.f_bfree;
        out.st.bavail = sv.f_bavail;
        out.st.files = sv.f_files;
        out.st.ffree = sv.f_ffree;
        out.st.bsize = sv.f_bsize;
        out.st.frsize = sv.f_frsize;
        out.st.namelen = (uint32_t)sv.f_namemax;
        reply_raw(in->unique, 0, &out, sizeof out);
        break;
    }

    case FUSE_OPENDIR:
    case FUSE_RELEASEDIR:
    case FUSE_READDIRPLUS:
    case FUSE_GETXATTR:
    case FUSE_LISTXATTR:
    case FUSE_SETXATTR:
    case FUSE_REMOVEXATTR:
    case FUSE_IOCTL:
    case FUSE_POLL:
    case FUSE_LSEEK:
    case FUSE_READLINK:
        reply_err(in->unique, ENOSYS);
        break;

    default:
        reply_err(in->unique, ENOSYS);
        break;
    }
}

/* ---- main ---- */

int fuse_main(int argc, char *argv[], const struct fuse_operations *ops, void *private_data) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <mountpoint>\n", argv[0]);
        return 2;
    }
    g_mnt = argv[1];
    g_ops = ops;
    g_ctx.private_data = private_data;

    g_fd = mount_fuse(g_mnt);
    if (g_fd < 0) {
        fprintf(stderr, "fuse: mount of %s failed\n", g_mnt);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    size_t bufsize = g_max_write + 4096;
    char *buf = malloc(bufsize);

    for (;;) {
        struct pollfd pfd = { .fd = g_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (g_stop) break;
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        ssize_t n = read(g_fd, buf, bufsize);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break; /* ENODEV after unmount */
        }
        if (n < (ssize_t)sizeof(struct fuse_in_header)) break;
        dispatch(buf, (size_t)n);
    }

    free(buf);

    unmount_fuse(g_mnt);
    if (g_ops->destroy)
        g_ops->destroy(private_data);

    close(g_fd);
    for (int i = 1; i <= g_nnodes; i++) free(g_nodes[i].path);
    free(g_nodes);
    ht_free(&g_path2ino);
    return 0;
}
