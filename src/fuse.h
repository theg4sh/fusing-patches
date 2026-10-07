#ifndef FUSE_H
#define FUSE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/*
 * Minimal FUSE (userspace) layer — our own implementation of the kernel FUSE
 * protocol over /dev/fuse, replacing libfuse3. Single-threaded request loop,
 * no kernel entry/attr caching (every open re-validates), no external tools.
 */

struct fuse_conn_info {
    unsigned proto_major;
    unsigned proto_minor;
    unsigned max_read;
    unsigned max_write;
    unsigned max_readahead;
};

struct fuse_config {
    int use_ino;
    double entry_timeout;
    double negative_timeout;
    double attr_timeout;
};

struct fuse_file_info {
    uint64_t fh;
    int flags;
};

struct fuse_context {
    uint32_t uid;
    uint32_t gid;
    uint32_t pid;
    void *private_data;
};

typedef int (*fuse_fill_dir_t)(void *buf, const char *name, const struct stat *stbuf,
                               off_t off, int flags);

struct fuse_operations {
    void *(*init)(struct fuse_conn_info *conn, struct fuse_config *cfg);
    void (*destroy)(void *private_data);
    int (*getattr)(const char *, struct stat *, struct fuse_file_info *);
    int (*readdir)(const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info *, int flags);
    int (*open)(const char *, struct fuse_file_info *);
    int (*read)(const char *, char *, size_t, off_t, struct fuse_file_info *);
    int (*write)(const char *, const char *, size_t, off_t, struct fuse_file_info *);
    int (*release)(const char *, struct fuse_file_info *);
    int (*flush)(const char *, struct fuse_file_info *);
    int (*create)(const char *, mode_t, struct fuse_file_info *);
    int (*truncate)(const char *, off_t, struct fuse_file_info *);
    int (*unlink)(const char *);
    int (*access)(const char *, int);
    int (*statfs)(const char *, struct statvfs *);
};

const struct fuse_context *fuse_get_context(void);

/* argv: [0]=program name, [1]=mountpoint. Returns 0 on clean exit. */
int fuse_main(int argc, char *argv[], const struct fuse_operations *ops, void *private_data);

#endif /* FUSE_H */
