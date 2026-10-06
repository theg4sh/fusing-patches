#include "patchstore.h"

#define _GNU_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */

struct ps_store {
    char *store_dir;
    char *lower_dir;
    char owner[64];
    pthread_mutex_t lock;
    ps_session *sessions;
    long seq; /* process-local, temp-file naming only */
};

/* byte range [lo, hi) */
struct range {
    off_t lo;
    off_t hi;
};

struct rangelist {
    struct range *r;
    int n;
    int cap;
};

struct ps_session {
    ps_store *ps;
    char *relpath;
    int is_merge;
    int fd;
    char *tmp_path;
    int64_t opened_at;
    int64_t base_seq;
    int dirty;
    struct rangelist ranges; /* byte ranges touched by this edit */
    ps_session *next;
};

struct patchrec {
    char id[64];
    int64_t opened_at;
    int64_t committed_at;
    off_t size;
    char owner[64];
    int64_t seq;
    char state; /* 'A' accepted, 'P' pending (unused now) */
    struct rangelist ranges;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static void make_subdir(const char *store_dir, const char *name, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/%s", store_dir, name);
}

static void encode_path(const char *relpath, char *out, size_t out_sz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t i = 0;
    const char *p = relpath;
    while (*p && i + 4 <= out_sz) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') {
            out[i++] = (char)c;
        } else {
            out[i++] = '%';
            out[i++] = hex[c >> 4];
            out[i++] = hex[c & 0xF];
        }
        p++;
    }
    out[i] = '\0';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void decode_path(const char *enc, char *out, size_t out_sz) {
    size_t i = 0, o = 0;
    while (enc[i] && o + 1 < out_sz) {
        if (enc[i] == '%' && hexval(enc[i + 1]) >= 0 && hexval(enc[i + 2]) >= 0) {
            out[o++] = (char)((hexval(enc[i + 1]) << 4) | hexval(enc[i + 2]));
            i += 3;
        } else {
            out[o++] = enc[i++];
        }
    }
    out[o] = '\0';
}

static int meta_path(ps_store *ps, const char *relpath, char *out, size_t out_sz) {
    char enc[4096];
    encode_path(relpath, enc, sizeof enc);
    snprintf(out, out_sz, "%s/meta/%s", ps->store_dir, enc);
    return 0;
}

static int merge_blob_path(ps_store *ps, const char *relpath, char *out, size_t out_sz) {
    char enc[4096];
    encode_path(relpath, enc, sizeof enc);
    snprintf(out, out_sz, "%s/merge/%s.blob", ps->store_dir, enc);
    return 0;
}

static int merge_meta_path(ps_store *ps, const char *relpath, char *out, size_t out_sz) {
    char enc[4096];
    encode_path(relpath, enc, sizeof enc);
    snprintf(out, out_sz, "%s/merge/%s.meta", ps->store_dir, enc);
    return 0;
}

static int lower_path(ps_store *ps, const char *relpath, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s%s", ps->lower_dir, relpath);
    return 0;
}

static int blob_path(ps_store *ps, const char *id, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/blobs/%s.blob", ps->store_dir, id);
    return 0;
}

static int ranges_path(ps_store *ps, const char *id, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/ranges/%s", ps->store_dir, id);
    return 0;
}

/* ------------------------------------------------------------------ */
/* range list                                                          */
/* ------------------------------------------------------------------ */

static void rl_add(struct rangelist *rl, off_t lo, off_t hi) {
    if (hi <= lo) return;
    if (rl->n == rl->cap) {
        rl->cap = rl->cap ? rl->cap * 2 : 8;
        rl->r = realloc(rl->r, sizeof(*rl->r) * rl->cap);
    }
    rl->r[rl->n].lo = lo;
    rl->r[rl->n].hi = hi;
    rl->n++;
}

static int rl_cmp(const void *a, const void *b) {
    const struct range *ra = a, *rb = b;
    if (ra->lo < rb->lo) return -1;
    if (ra->lo > rb->lo) return 1;
    return 0;
}

/* sort and merge overlapping/adjacent ranges in place */
static void rl_merge(struct rangelist *rl) {
    if (rl->n < 2) return;
    qsort(rl->r, rl->n, sizeof(*rl->r), rl_cmp);
    int out = 0;
    for (int i = 1; i < rl->n; i++) {
        if (rl->r[i].lo <= rl->r[out].hi) {
            if (rl->r[i].hi > rl->r[out].hi) rl->r[out].hi = rl->r[i].hi;
        } else {
            rl->r[++out] = rl->r[i];
        }
    }
    rl->n = out + 1;
}

static void rl_free(struct rangelist *rl) {
    free(rl->r);
    rl->r = NULL;
    rl->n = rl->cap = 0;
}

/* is every range in `a` fully covered by the merged ranges of `b`? */
static int rl_subset(struct rangelist *a, struct rangelist *b) {
    for (int i = 0; i < a->n; i++) {
        off_t lo = a->r[i].lo, hi = a->r[i].hi;
        int covered = 0;
        for (int j = 0; j < b->n && !covered; j++)
            if (b->r[j].lo <= lo && b->r[j].hi >= hi) covered = 1;
        if (!covered) return 0;
    }
    return 1;
}

static void rl_union_into(struct rangelist *dst, struct rangelist *src) {
    for (int i = 0; i < src->n; i++) rl_add(dst, src->r[i].lo, src->r[i].hi);
    rl_merge(dst);
}

/* ------------------------------------------------------------------ */
/* global sequence (flock-guarded, shared across processes)            */
/* ------------------------------------------------------------------ */

static int64_t seq_read(ps_store *ps) {
    char sp[4096];
    snprintf(sp, sizeof sp, "%s/seq", ps->store_dir);
    FILE *f = fopen(sp, "r");
    if (!f) return 0;
    int64_t v = 0;
    fscanf(f, "%lld", (long long *)&v);
    fclose(f);
    return v;
}

static int64_t next_seq(ps_store *ps) {
    char lp[4096], sp[4096];
    snprintf(lp, sizeof lp, "%s/seq.lock", ps->store_dir);
    snprintf(sp, sizeof sp, "%s/seq", ps->store_dir);
    int lfd = open(lp, O_RDWR | O_CREAT, 0644);
    if (lfd < 0) return -1;
    flock(lfd, LOCK_EX);
    int64_t cur = seq_read(ps);
    cur++;
    FILE *f = fopen(sp, "w");
    if (f) {
        fprintf(f, "%lld\n", (long long)cur);
        fclose(f);
    }
    flock(lfd, LOCK_UN);
    close(lfd);
    return cur;
}

/* ------------------------------------------------------------------ */
/* per-mountpoint state                                                */
/* ------------------------------------------------------------------ */

static int64_t owner_read_seq(ps_store *ps, const char *owner) {
    char p[4096];
    snprintf(p, sizeof p, "%s/mounts/%s", ps->store_dir, owner);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int64_t v = 0;
    fscanf(f, "%lld", (long long *)&v);
    fclose(f);
    return v;
}

static void owner_write_seq(ps_store *ps, const char *owner, int64_t v) {
    char p[4096];
    snprintf(p, sizeof p, "%s/mounts/%s", ps->store_dir, owner);
    FILE *f = fopen(p, "w");
    if (f) {
        fprintf(f, "%lld\n", (long long)v);
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */
/* patch meta + ranges                                                 */
/* ------------------------------------------------------------------ */

static void patchrec_free(struct patchrec *recs, int n) {
    for (int i = 0; i < n; i++) rl_free(&recs[i].ranges);
    free(recs);
}

static void load_ranges(ps_store *ps, const char *id, struct rangelist *rl) {
    char rp[4096];
    ranges_path(ps, id, rp, sizeof rp);
    FILE *f = fopen(rp, "r");
    if (!f) return;
    long long lo, hi;
    while (fscanf(f, "%lld %lld", &lo, &hi) == 2)
        rl_add(rl, (off_t)lo, (off_t)hi);
    fclose(f);
    rl_merge(rl);
}

static void save_ranges(ps_store *ps, const char *id, struct rangelist *rl) {
    rl_merge(rl);
    char rp[4096];
    ranges_path(ps, id, rp, sizeof rp);
    FILE *f = fopen(rp, "w");
    if (!f) return;
    for (int i = 0; i < rl->n; i++)
        fprintf(f, "%lld %lld\n", (long long)rl->r[i].lo, (long long)rl->r[i].hi);
    fclose(f);
}

static int meta_load(ps_store *ps, const char *relpath, struct patchrec **out) {
    char path[4096];
    struct patchrec *recs = NULL;
    int n = 0, cap = 0;
    meta_path(ps, relpath, path, sizeof path);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[8192];
    while (fgets(line, sizeof line, f)) {
        char id[64], owner[64], state;
        long long opened, committed, size, seq;
        if (sscanf(line, "P %63s %lld %lld %lld %63s %lld %c",
                   id, &opened, &committed, &size, owner, &seq, &state) == 7) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                recs = realloc(recs, sizeof(*recs) * cap);
            }
            struct patchrec *r = &recs[n];
            memset(r, 0, sizeof *r);
            snprintf(r->id, sizeof r->id, "%s", id);
            r->opened_at = opened;
            r->committed_at = committed;
            r->size = (off_t)size;
            snprintf(r->owner, sizeof r->owner, "%s", owner);
            r->seq = seq;
            r->state = state;
            load_ranges(ps, id, &r->ranges);
            n++;
        }
    }
    fclose(f);
    *out = recs;
    return n;
}

static int merge_meta_read(ps_store *ps, const char *relpath, int64_t *seq, off_t *size) {
    char mp[4096];
    merge_meta_path(ps, relpath, mp, sizeof mp);
    FILE *f = fopen(mp, "r");
    if (!f) return 0;
    long long s, sz;
    char owner[64];
    int got = 0;
    if (fscanf(f, "M %lld %lld %63s", &s, &sz, owner) == 3) {
        *seq = s;
        *size = (off_t)sz;
        got = 1;
    }
    fclose(f);
    return got;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

ps_store *ps_open(const char *store_dir, const char *lower_dir) {
    ps_store *ps = calloc(1, sizeof *ps);
    if (!ps) return NULL;
    ps->store_dir = strdup(store_dir);
    ps->lower_dir = strdup(lower_dir);
    pthread_mutex_init(&ps->lock, NULL);

    const char *subs[] = { "blobs", "meta", "merge", "mounts", "messages", "ranges", "tmp" };
    if (ensure_dir(ps->store_dir) != 0) goto fail;
    for (size_t i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        char sub[4096];
        make_subdir(ps->store_dir, subs[i], sub, sizeof sub);
        if (ensure_dir(sub) != 0) goto fail;
    }
    return ps;

fail:
    ps_close(ps);
    return NULL;
}

void ps_close(ps_store *ps) {
    if (!ps) return;
    while (ps->sessions) {
        ps_session *s = ps->sessions;
        ps->sessions = s->next;
        if (s->fd >= 0) close(s->fd);
        if (s->tmp_path) unlink(s->tmp_path);
        free(s->tmp_path);
        free(s->relpath);
        rl_free(&s->ranges);
        free(s);
    }
    pthread_mutex_destroy(&ps->lock);
    free(ps->store_dir);
    free(ps->lower_dir);
    free(ps);
}

void ps_set_owner(ps_store *ps, const char *owner) {
    snprintf(ps->owner, sizeof ps->owner, "%s", owner);
    if (owner_read_seq(ps, ps->owner) < 0)
        owner_write_seq(ps, ps->owner, seq_read(ps));
}

/* ------------------------------------------------------------------ */
/* sessions                                                            */
/* ------------------------------------------------------------------ */

static int copy_path_into(const char *src, int dst_fd) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return -1;
    char buf[65536];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst_fd, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                close(sfd);
                return -1;
            }
            off += w;
        }
    }
    close(sfd);
    return 0;
}

static int seed_session(ps_store *ps, ps_session *s, int exists) {
    s->base_seq = 0;
    if (!exists) return 0;

    char blob[4096];
    char lp[4096];
    int64_t seq = 0;
    ps_src src = ps_resolve(ps, s->relpath, blob, sizeof blob, &seq);
    const char *path = NULL;
    if (src == PS_SRC_LOWER) {
        lower_path(ps, s->relpath, lp, sizeof lp);
        struct stat st;
        if (stat(lp, &st) == 0 && S_ISREG(st.st_mode)) path = lp;
        else return 0;
    } else {
        path = blob;
    }
    s->base_seq = (src == PS_SRC_LOWER) ? 0 : seq;
    return copy_path_into(path, s->fd);
}

static ps_session *begin_common(ps_store *ps, const char *relpath, int exists, int is_merge) {
    ps_session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ps = ps;
    s->relpath = strdup(relpath);
    s->is_merge = is_merge;
    s->fd = -1;
    s->opened_at = now_ns();

    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s/tmp/%lld-%ld.tmp", ps->store_dir,
             (long long)s->opened_at, ps->seq++);
    s->tmp_path = strdup(tmp);

    s->fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (s->fd < 0) goto fail;

    if (seed_session(ps, s, exists) != 0) goto fail;

    pthread_mutex_lock(&ps->lock);
    s->next = ps->sessions;
    ps->sessions = s;
    pthread_mutex_unlock(&ps->lock);
    return s;

fail:
    if (s->fd >= 0) close(s->fd);
    if (s->tmp_path) {
        unlink(s->tmp_path);
        free(s->tmp_path);
    }
    free(s->relpath);
    rl_free(&s->ranges);
    free(s);
    return NULL;
}

ps_session *ps_begin(ps_store *ps, const char *relpath, int exists) {
    return begin_common(ps, relpath, exists, 0);
}

ps_session *ps_begin_merge(ps_store *ps, const char *relpath) {
    /* a merge write carries the full agreed content, so it starts empty */
    return begin_common(ps, relpath, 0, 1);
}

ps_session *ps_begin_merge_seeded(ps_store *ps, const char *relpath) {
    /* patch-view edit: seed from current content, commit to the merged snapshot */
    return begin_common(ps, relpath, 1, 1);
}

ssize_t ps_write(ps_store *ps, ps_session *s, const char *buf, size_t size, off_t off) {
    (void)ps;
    ssize_t w = pwrite(s->fd, buf, size, off);
    if (w < 0) return -errno;
    s->dirty = 1;
    rl_add(&s->ranges, off, off + (off_t)w);
    return w;
}

int ps_session_fd(ps_session *s) {
    return s->fd;
}

int ps_session_dirty(ps_session *s) {
    return s->dirty;
}

int ps_session_is_merge(ps_session *s) {
    return s->is_merge;
}

int ps_truncate(ps_store *ps, ps_session *s, off_t size) {
    (void)ps;
    struct stat st;
    off_t old = 0;
    if (fstat(s->fd, &st) == 0) old = st.st_size;
    if (ftruncate(s->fd, size) != 0) return -errno;
    s->dirty = 1;
    off_t lo = old < size ? old : size;
    off_t hi = old < size ? size : old;
    if (lo < hi) rl_add(&s->ranges, lo, hi);
    return 0;
}

ssize_t ps_read(ps_store *ps, ps_session *s, char *buf, size_t size, off_t off) {
    (void)ps;
    ssize_t r = pread(s->fd, buf, size, off);
    return r < 0 ? -errno : r;
}

/* ------------------------------------------------------------------ */
/* conflict detection                                                  */
/* ------------------------------------------------------------------ */

/* union of the session's ranges that overlap another writer's new change */
static int compute_conflict(ps_store *ps, ps_session *s, struct rangelist *out) {
    rl_merge(&s->ranges);

    struct patchrec *recs = NULL;
    int n = meta_load(ps, s->relpath, &recs);
    for (int i = 0; i < n; i++) {
        struct patchrec *p = &recs[i];
        if (strcmp(p->owner, ps->owner) == 0) continue;
        if (p->state != 'A') continue;
        if (p->seq <= s->base_seq) continue;
        for (int a = 0; a < s->ranges.n; a++) {
            for (int b = 0; b < p->ranges.n; b++) {
                off_t lo = s->ranges.r[a].lo > p->ranges.r[b].lo ? s->ranges.r[a].lo : p->ranges.r[b].lo;
                off_t hi = s->ranges.r[a].hi < p->ranges.r[b].hi ? s->ranges.r[a].hi : p->ranges.r[b].hi;
                if (lo < hi) rl_add(out, lo, hi);
            }
        }
    }
    patchrec_free(recs, n);

    /* an unseen merge covers the whole file: any write to it conflicts */
    int64_t mseq;
    off_t msize;
    if (merge_meta_read(ps, s->relpath, &mseq, &msize) && mseq > s->base_seq) {
        for (int a = 0; a < s->ranges.n; a++)
            rl_add(out, s->ranges.r[a].lo, s->ranges.r[a].hi);
    }

    rl_merge(out);
    return out->n;
}

int ps_check_conflict(ps_store *ps, ps_session *s) {
    struct rangelist cr = {0};
    int n = compute_conflict(ps, s, &cr);
    rl_free(&cr);
    return n > 0;
}

/* ------------------------------------------------------------------ */
/* conflict messages (stderr + /fuse/messages/, de-duplicated)         */
/* ------------------------------------------------------------------ */

static void run_diff(const char *a, const char *b, const char *label_a,
                     const char *label_b, FILE *out) {
    char cmd[16384];
    snprintf(cmd, sizeof cmd,
             "diff -u --label '%s' --label '%s' '%s' '%s' 2>/dev/null",
             label_a, label_b, a, b);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    char line[4096];
    while (fgets(line, sizeof line, p))
        fputs(line, out);
    int rc = pclose(p);
    if (rc == 0)
        fprintf(out, "(no differences)\n");
}

static int base_content_path(ps_store *ps, const char *relpath, int64_t base_seq,
                             char *out, size_t out_sz) {
    if (base_seq == 0) {
        char lp[4096];
        lower_path(ps, relpath, lp, sizeof lp);
        struct stat st;
        if (stat(lp, &st) == 0) {
            snprintf(out, out_sz, "%s", lp);
            return 1;
        }
        return 0;
    }
    struct patchrec *recs = NULL;
    int n = meta_load(ps, relpath, &recs);
    for (int i = 0; i < n; i++) {
        if (recs[i].seq == base_seq) {
            blob_path(ps, recs[i].id, out, out_sz);
            patchrec_free(recs, n);
            return 1;
        }
    }
    patchrec_free(recs, n);
    int64_t mseq;
    off_t msize;
    if (merge_meta_read(ps, relpath, &mseq, &msize) && mseq == base_seq) {
        merge_blob_path(ps, relpath, out, out_sz);
        return 1;
    }
    return 0;
}

static void reported_path(ps_store *ps, const char *owner, const char *relpath,
                          char *out, size_t out_sz) {
    char enc[4096];
    encode_path(relpath, enc, sizeof enc);
    snprintf(out, out_sz, "%s/messages/%s/.reported/%s", ps->store_dir, owner, enc);
}

static void load_reported(ps_store *ps, const char *owner, const char *relpath,
                          struct rangelist *rl) {
    char p[4096];
    reported_path(ps, owner, relpath, p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    long long lo, hi;
    while (fscanf(f, "%lld %lld", &lo, &hi) == 2)
        rl_add(rl, (off_t)lo, (off_t)hi);
    fclose(f);
    rl_merge(rl);
}

static void save_reported(ps_store *ps, const char *owner, const char *relpath,
                          struct rangelist *rl) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/messages/%s/.reported", ps->store_dir, owner);
    ensure_dir(dir);
    char p[4096];
    reported_path(ps, owner, relpath, p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < rl->n; i++)
        fprintf(f, "%lld %lld\n", (long long)rl->r[i].lo, (long long)rl->r[i].hi);
    fclose(f);
}

static void write_message(ps_store *ps, ps_session *s, int64_t seq) {
    char mdir[4096];
    snprintf(mdir, sizeof mdir, "%s/messages/%s", ps->store_dir, ps->owner);
    ensure_dir(mdir);
    char msg[4096];
    snprintf(msg, sizeof msg, "%s/%lld.msg", mdir, (long long)seq);
    char tmp[4192];
    snprintf(tmp, sizeof tmp, "%s.tmp", msg);

    FILE *out = fopen(tmp, "w");
    if (!out) return;

    char base[4096];
    int have_base = base_content_path(ps, s->relpath, s->base_seq, base, sizeof base);
    if (!have_base) {
        snprintf(base, sizeof base, "%s/tmp/empty-%lld", ps->store_dir, (long long)now_ns());
        int fd = open(base, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    }
    char cur[4096];
    int64_t cseq;
    ps_src src = ps_resolve(ps, s->relpath, cur, sizeof cur, &cseq);
    if (src == PS_SRC_LOWER)
        lower_path(ps, s->relpath, cur, sizeof cur);

    const char *hdr =
        "CONFLICT: your change to %s was soft-blocked (not applied) because it\n"
        "overlaps changes made by another writer:\n\n";
    const char *ftr =
        "\nRe-read the file and retry, or write the resolved content to /fuse/merge/%s\n";

    fprintf(out, hdr, s->relpath);
    fprintf(stderr, hdr, s->relpath);
    run_diff(base, cur, "your base", "other writers", out);
    run_diff(base, cur, "your base", "other writers", stderr);
    fprintf(out, ftr, s->relpath + 1);
    fprintf(stderr, ftr, s->relpath + 1);

    fclose(out);
    rename(tmp, msg);
}

void ps_report_conflict(ps_store *ps, ps_session *s) {
    struct rangelist cr = {0};
    compute_conflict(ps, s, &cr);
    if (cr.n == 0) {
        rl_free(&cr);
        return;
    }

    struct rangelist reported = {0};
    load_reported(ps, ps->owner, s->relpath, &reported);

    /* only emit if this conflict extends what we already reported */
    if (!rl_subset(&cr, &reported)) {
        int64_t seq = next_seq(ps);
        write_message(ps, s, seq);
        rl_union_into(&reported, &cr);
        save_reported(ps, ps->owner, s->relpath, &reported);
    }

    rl_free(&reported);
    rl_free(&cr);
}

/* ------------------------------------------------------------------ */
/* commit                                                              */
/* ------------------------------------------------------------------ */

static void unlink_session(ps_store *ps, ps_session *s) {
    ps_session **pp = &ps->sessions;
    while (*pp) {
        if (*pp == s) {
            *pp = s->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void session_free(ps_session *s) {
    if (s->fd >= 0) close(s->fd);
    if (s->tmp_path) {
        unlink(s->tmp_path);
        free(s->tmp_path);
    }
    free(s->relpath);
    rl_free(&s->ranges);
    free(s);
}

static void mark_pending_accepted(ps_store *ps, const char *relpath) {
    struct patchrec *recs = NULL;
    int n = meta_load(ps, relpath, &recs);
    if (n == 0) return;
    char mp[4096];
    meta_path(ps, relpath, mp, sizeof mp);
    FILE *f = fopen(mp, "w");
    if (!f) {
        patchrec_free(recs, n);
        return;
    }
    for (int i = 0; i < n; i++) {
        struct patchrec *r = &recs[i];
        char state = (r->state == 'P') ? 'A' : r->state;
        fprintf(f, "P %s %lld %lld %lld %s %lld %c\n",
                r->id, (long long)r->opened_at, (long long)r->committed_at,
                (long long)r->size, r->owner, (long long)r->seq, state);
    }
    fclose(f);
    patchrec_free(recs, n);
}

static int commit_merge(ps_store *ps, ps_session *s) {
    int64_t seq = next_seq(ps);
    if (seq < 0) return -1;

    char mb[4096];
    merge_blob_path(ps, s->relpath, mb, sizeof mb);
    if (rename(s->tmp_path, mb) != 0) {
        pthread_mutex_lock(&ps->lock);
        unlink_session(ps, s);
        pthread_mutex_unlock(&ps->lock);
        session_free(s);
        return -1;
    }
    free(s->tmp_path);
    s->tmp_path = NULL;

    struct stat st;
    fstat(s->fd, &st);

    char mm[4096];
    merge_meta_path(ps, s->relpath, mm, sizeof mm);
    FILE *f = fopen(mm, "w");
    if (f) {
        fprintf(f, "M %lld %lld %s\n", (long long)seq, (long long)st.st_size, ps->owner);
        fclose(f);
    }

    mark_pending_accepted(ps, s->relpath);
    owner_write_seq(ps, ps->owner, seq);

    pthread_mutex_lock(&ps->lock);
    unlink_session(ps, s);
    pthread_mutex_unlock(&ps->lock);
    session_free(s);
    return 0;
}

static int session_is_noop(ps_store *ps, ps_session *s) {
    struct stat st;
    if (fstat(s->fd, &st) != 0) return 0;
    off_t fsize = st.st_size;

    char base[4096];
    if (!base_content_path(ps, s->relpath, s->base_seq, base, sizeof base))
        return fsize == 0;

    int bfd = open(base, O_RDONLY);
    if (bfd < 0) return fsize == 0;
    struct stat bst;
    if (fstat(bfd, &bst) != 0 || bst.st_size != fsize) {
        close(bfd);
        return 0;
    }
    char b1[65536], b2[65536];
    off_t off = 0;
    while (off < fsize) {
        size_t n = (size_t)(fsize - off > 65536 ? 65536 : fsize - off);
        ssize_t r1 = pread(s->fd, b1, n, off);
        ssize_t r2 = read(bfd, b2, n);
        if (r1 != (ssize_t)n || r2 != (ssize_t)n || memcmp(b1, b2, n) != 0) {
            close(bfd);
            return 0;
        }
        off += n;
    }
    close(bfd);
    return 1;
}

int ps_commit(ps_store *ps, ps_session *s) {
    if (!s->dirty) {
        pthread_mutex_lock(&ps->lock);
        unlink_session(ps, s);
        pthread_mutex_unlock(&ps->lock);
        session_free(s);
        return 0;
    }

    if (s->is_merge)
        return commit_merge(ps, s);

    /* a write that does not change the final content is not a patch */
    if (session_is_noop(ps, s)) {
        pthread_mutex_lock(&ps->lock);
        unlink_session(ps, s);
        pthread_mutex_unlock(&ps->lock);
        session_free(s);
        return 0;
    }

    /* soft-block: overlapping ranges -> do not commit */
    if (ps_check_conflict(ps, s)) {
        ps_report_conflict(ps, s);
        pthread_mutex_lock(&ps->lock);
        unlink_session(ps, s);
        pthread_mutex_unlock(&ps->lock);
        session_free(s);
        return -2;
    }

    int64_t committed_at = now_ns();
    int64_t seq = next_seq(ps);
    if (seq < 0) return -1;

    char id[64];
    snprintf(id, sizeof id, "p%lld", (long long)seq);
    char bp[4096];
    blob_path(ps, id, bp, sizeof bp);
    if (rename(s->tmp_path, bp) != 0) {
        pthread_mutex_lock(&ps->lock);
        unlink_session(ps, s);
        pthread_mutex_unlock(&ps->lock);
        session_free(s);
        return -1;
    }
    free(s->tmp_path);
    s->tmp_path = NULL;

    struct stat st;
    fstat(s->fd, &st);

    char mp[4096];
    meta_path(ps, s->relpath, mp, sizeof mp);
    FILE *f = fopen(mp, "a");
    if (!f) {
        unlink(bp);
        pthread_mutex_lock(&ps->lock);
        unlink_session(ps, s);
        pthread_mutex_unlock(&ps->lock);
        session_free(s);
        return -1;
    }
    fprintf(f, "P %s %lld %lld %lld %s %lld A\n",
            id, (long long)s->opened_at, (long long)committed_at,
            (long long)st.st_size, ps->owner, (long long)seq);
    fclose(f);

    save_ranges(ps, id, &s->ranges);
    owner_write_seq(ps, ps->owner, seq);

    pthread_mutex_lock(&ps->lock);
    unlink_session(ps, s);
    pthread_mutex_unlock(&ps->lock);
    session_free(s);
    return 0;
}

void ps_abort(ps_store *ps, ps_session *s) {
    if (!s) return;
    pthread_mutex_lock(&ps->lock);
    unlink_session(ps, s);
    pthread_mutex_unlock(&ps->lock);
    session_free(s);
}

/* ------------------------------------------------------------------ */
/* resolution / listing                                                */
/* ------------------------------------------------------------------ */

ps_src ps_resolve(ps_store *ps, const char *relpath, char *blob_out, size_t blob_out_sz,
                  int64_t *seq_out) {
    if (seq_out) *seq_out = 0;

    int64_t best_seq = 0;
    int best_kind = PS_SRC_LOWER;
    char best_blob[4096] = "";

    int64_t mseq;
    off_t msize;
    if (merge_meta_read(ps, relpath, &mseq, &msize)) {
        merge_blob_path(ps, relpath, best_blob, sizeof best_blob);
        best_seq = mseq;
        best_kind = PS_SRC_MERGE;
    }

    struct patchrec *recs = NULL;
    int n = meta_load(ps, relpath, &recs);
    for (int i = 0; i < n; i++) {
        if (recs[i].state != 'A') continue;
        if (recs[i].seq > best_seq) {
            blob_path(ps, recs[i].id, best_blob, sizeof best_blob);
            best_seq = recs[i].seq;
            best_kind = PS_SRC_PATCH;
        }
    }
    patchrec_free(recs, n);

    if (best_kind == PS_SRC_LOWER)
        return PS_SRC_LOWER;
    snprintf(blob_out, blob_out_sz, "%s", best_blob);
    if (seq_out) *seq_out = best_seq;
    return best_kind;
}

int ps_merge_resolve(ps_store *ps, const char *relpath, char *blob_out, size_t blob_out_sz) {
    int64_t mseq;
    off_t msize;
    if (!merge_meta_read(ps, relpath, &mseq, &msize)) return 0;
    merge_blob_path(ps, relpath, blob_out, blob_out_sz);
    return 1;
}

int ps_active_size(ps_store *ps, const char *relpath, off_t *size_out) {
    int found = 0;
    pthread_mutex_lock(&ps->lock);
    for (ps_session *s = ps->sessions; s; s = s->next) {
        if (strcmp(s->relpath, relpath) == 0) {
            struct stat st;
            if (fstat(s->fd, &st) == 0) {
                *size_out = st.st_size;
                found = 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&ps->lock);
    return found;
}

static const char *basename_under(const char *dir, const char *path) {
    if (strcmp(dir, "/") == 0) {
        const char *name = path + 1;
        if (*name == '\0' || strchr(name, '/') != NULL) return NULL;
        return name;
    }
    size_t dlen = strlen(dir);
    if (strncmp(dir, path, dlen) != 0) return NULL;
    if (path[dlen] != '/') return NULL;
    const char *name = path + dlen + 1;
    if (*name == '\0' || strchr(name, '/') != NULL) return NULL;
    return name;
}

static void list_meta_entries(ps_store *ps, const char *dir, ps_list_cb cb, void *ctx) {
    char mdir[4096];
    snprintf(mdir, sizeof mdir, "%s/meta", ps->store_dir);
    DIR *d = opendir(mdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char rel[4096];
        decode_path(e->d_name, rel, sizeof rel);
        const char *name = basename_under(dir, rel);
        if (name) cb(ctx, name);
    }
    closedir(d);
}

static void list_merge_entries(ps_store *ps, const char *dir, ps_list_cb cb, void *ctx) {
    char mg[4096];
    snprintf(mg, sizeof mg, "%s/merge", ps->store_dir);
    DIR *d = opendir(mg);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 5, ".meta") != 0) continue;
        char base[4096];
        snprintf(base, sizeof base, "%.*s", (int)(len - 5), e->d_name);
        char rel[4096];
        decode_path(base, rel, sizeof rel);
        const char *name = basename_under(dir, rel);
        if (name) cb(ctx, name);
    }
    closedir(d);
}

void ps_list_dir(ps_store *ps, const char *dir, ps_list_cb cb, void *ctx) {
    list_meta_entries(ps, dir, cb, ctx);
    list_merge_entries(ps, dir, cb, ctx);

    pthread_mutex_lock(&ps->lock);
    for (ps_session *s = ps->sessions; s; s = s->next) {
        const char *name = basename_under(dir, s->relpath);
        if (name) cb(ctx, name);
    }
    pthread_mutex_unlock(&ps->lock);
}

void ps_list_merge(ps_store *ps, const char *dir, ps_list_cb cb, void *ctx) {
    list_merge_entries(ps, dir, cb, ctx);
}

void ps_list_messages(ps_store *ps, ps_list_cb cb, void *ctx) {
    char mdir[4096];
    snprintf(mdir, sizeof mdir, "%s/messages/%s", ps->store_dir, ps->owner);
    DIR *d = opendir(mdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        cb(ctx, e->d_name);
    }
    closedir(d);
}

int ps_message_path(ps_store *ps, const char *name, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/messages/%s/%s", ps->store_dir, ps->owner, name);
    return 0;
}

/* ------------------------------------------------------------------ */
/* mount registry + apply-to-origin + patch-view                       */
/* ------------------------------------------------------------------ */

static const char *kind_name(ps_kind k) {
    switch (k) {
    case PS_KIND_WRITER: return "writer";
    case PS_KIND_MERGE: return "merge";
    case PS_KIND_PATCHVIEW: return "patchview";
    }
    return "unknown";
}

void ps_mount_register(ps_store *ps, ps_kind kind) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/mounts/active", ps->store_dir);
    ensure_dir(dir);
    char f[4096];
    snprintf(f, sizeof f, "%s/%s.%s.%ld", dir, kind_name(kind), ps->owner, (long)getpid());
    int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}

int ps_mount_unregister(ps_store *ps, ps_kind kind) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/mounts/active", ps->store_dir);
    char f[4096];
    snprintf(f, sizeof f, "%s/%s.%s.%ld", dir, kind_name(kind), ps->owner, (long)getpid());
    unlink(f);

    DIR *d = opendir(dir);
    if (!d) return 1;
    int nonmerge = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        /* merge views do not hold the apply back */
        if (strncmp(e->d_name, "merge.", 6) != 0) nonmerge = 1;
    }
    closedir(d);
    return !nonmerge;
}

/* recursive mkdir -p for a full path's parent directory */
static void ensure_parent_dirs(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    char *p = buf + 1; /* skip leading '/' */
    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            ensure_dir(buf);
            *p = '/';
        }
    }
}

static int copy_file_to_path(const char *src, const char *dst) {
    ensure_parent_dirs(dst);
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int rc = copy_path_into(src, fd);
    close(fd);
    return rc;
}

static void collect_path(char ***arr, int *n, int *cap, const char *rel) {
    for (int i = 0; i < *n; i++)
        if (strcmp((*arr)[i], rel) == 0) return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = realloc(*arr, sizeof(**arr) * *cap);
    }
    (*arr)[*n] = strdup(rel);
    (*n)++;
}

int ps_apply_to_origin(ps_store *ps) {
    char **paths = NULL;
    int n = 0, cap = 0;

    /* collect modified paths from patch index + merge index */
    char mdir[4096];
    snprintf(mdir, sizeof mdir, "%s/meta", ps->store_dir);
    DIR *d = opendir(mdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char rel[4096];
            decode_path(e->d_name, rel, sizeof rel);
            collect_path(&paths, &n, &cap, rel);
        }
        closedir(d);
    }
    char mg[4096];
    snprintf(mg, sizeof mg, "%s/merge", ps->store_dir);
    d = opendir(mg);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            size_t len = strlen(e->d_name);
            if (len < 5 || strcmp(e->d_name + len - 5, ".meta") != 0) continue;
            char base[4096];
            snprintf(base, sizeof base, "%.*s", (int)(len - 5), e->d_name);
            char rel[4096];
            decode_path(base, rel, sizeof rel);
            collect_path(&paths, &n, &cap, rel);
        }
        closedir(d);
    }

    int applied = 0;
    for (int i = 0; i < n; i++) {
        char blob[4096];
        int64_t seq;
        ps_src src = ps_resolve(ps, paths[i], blob, sizeof blob, &seq);
        if (src == PS_SRC_LOWER) continue; /* rejected or untouched */
        char lp[4096];
        lower_path(ps, paths[i], lp, sizeof lp);
        if (copy_file_to_path(blob, lp) == 0) applied++;
    }
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
    return applied;
}

int ps_reject(ps_store *ps, const char *relpath) {
    char mp[4096];
    meta_path(ps, relpath, mp, sizeof mp);
    unlink(mp);
    char mb[4096];
    merge_blob_path(ps, relpath, mb, sizeof mb);
    unlink(mb);
    char mm[4096];
    merge_meta_path(ps, relpath, mm, sizeof mm);
    unlink(mm);
    return 0;
}

static int copy_fd_to_fd(int src, int dst) {
    char buf[65536];
    ssize_t n;
    lseek(src, 0, SEEK_SET);
    while ((n = read(src, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            off += w;
        }
    }
    return 0;
}

/* write a file descriptor's content as the merge snapshot for relpath */
static int merge_put_fd(ps_store *ps, const char *relpath, int content_fd, off_t size) {
    int64_t seq = next_seq(ps);
    if (seq < 0) return -1;
    char mb[4096];
    merge_blob_path(ps, relpath, mb, sizeof mb);
    int fd = open(mb, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (copy_fd_to_fd(content_fd, fd) != 0) {
        close(fd);
        return -1;
    }
    close(fd);

    char mm[4096];
    merge_meta_path(ps, relpath, mm, sizeof mm);
    FILE *f = fopen(mm, "w");
    if (f) {
        fprintf(f, "M %lld %lld %s\n", (long long)seq, (long long)size, ps->owner);
        fclose(f);
    }
    owner_write_seq(ps, ps->owner, seq);
    return 0;
}

/* ------------------------------------------------------------------ */
/* git-diff splitting + conflict-marked application                    */
/* ------------------------------------------------------------------ */

struct diff_seg {
    char *path;
    char *text;
};

static void strip_nl(char *s) {
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r')) s[--l] = '\0';
}

/* split a (possibly multi-file) git/unified diff into per-file segments */
static int split_diff(const char *diff, struct diff_seg **out) {
    FILE *f = fmemopen((void *)diff, strlen(diff), "r");
    if (!f) return 0;

    char *line = NULL;
    size_t cap = 0;
    struct diff_seg *segs = NULL;
    int n = 0, c = 0;
    char *cur_path = NULL;
    char *cur_text = NULL;
    size_t cur_len = 0;

    while (getline(&line, &cap, f) != -1) {
        int new_seg = 0;
        char newpath[4096] = "";

        if (strncmp(line, "diff --git ", 11) == 0) {
            char *b = strstr(line, " b/");
            if (b) {
                snprintf(newpath, sizeof newpath, "%s", b + 3);
                strip_nl(newpath);
                new_seg = 1;
            }
        } else if (strncmp(line, "+++ ", 4) == 0 && cur_path == NULL) {
            const char *np = line + 4;
            if (strncmp(np, "b/", 2) == 0) np += 2;
            else if (strncmp(np, "a/", 2) == 0) np += 2;
            snprintf(newpath, sizeof newpath, "%s", np);
            strip_nl(newpath);
            new_seg = 1;
        }

        if (new_seg && cur_path) {
            segs[n - 1].text = cur_text ? cur_text : strdup("");
            cur_text = NULL;
            cur_len = 0;
            cur_path = NULL;
        }
        if (new_seg && !cur_path && newpath[0]) {
            if (n == c) {
                c = c ? c * 2 : 8;
                segs = realloc(segs, sizeof(*segs) * c);
            }
            segs[n].path = strdup(newpath);
            segs[n].text = NULL;
            cur_path = segs[n].path;
            n++;
        }
        if (cur_path) {
            size_t l = strlen(line);
            cur_text = realloc(cur_text, cur_len + l + 1);
            memcpy(cur_text + cur_len, line, l);
            cur_len += l;
            cur_text[cur_len] = '\0';
        }
    }
    if (cur_path)
        segs[n - 1].text = cur_text ? cur_text : strdup("");

    free(line);
    fclose(f);
    *out = segs;
    return n;
}

/* write a conflict message (real diff-conflict) to stderr + messages/ */
static void print_diff_conflict(ps_store *ps, const char *relpath,
                                const char *theirs, const char *proposed) {
    char mdir[4096];
    snprintf(mdir, sizeof mdir, "%s/messages/%s", ps->store_dir, ps->owner);
    ensure_dir(mdir);
    int64_t seq = next_seq(ps);
    char msg[4096], tmp[4192];
    snprintf(msg, sizeof msg, "%s/%lld.msg", mdir, (long long)seq);
    snprintf(tmp, sizeof tmp, "%s.tmp", msg);

    FILE *out = fopen(tmp, "w");
    if (!out) return;
    const char *hdr = "CONFLICT applying patch to %s (real final diff-conflict):\n\n";
    fprintf(out, hdr, relpath);
    fprintf(stderr, hdr, relpath);
    run_diff(theirs, proposed, "theirs", "proposed (with conflicts)", out);
    run_diff(theirs, proposed, "theirs", "proposed (with conflicts)", stderr);
    fclose(out);
    rename(tmp, msg);
}

/* apply one file's diff segment to its merged state, with conflict markers */
static int apply_file_diff(ps_store *ps, const char *relpath, const char *seg) {
    char diffp[4096], inp[4096];
    snprintf(diffp, sizeof diffp, "%s/tmp/audit.diff", ps->store_dir);
    snprintf(inp, sizeof inp, "%s/tmp/audit.in", ps->store_dir);

    char cur[4096];
    int64_t seq;
    ps_src src = ps_resolve(ps, relpath, cur, sizeof cur, &seq);
    if (src == PS_SRC_LOWER)
        lower_path(ps, relpath, cur, sizeof cur);

    int ifd = open(inp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ifd < 0) return -1;
    if (copy_path_into(cur, ifd) != 0) { close(ifd); return -1; }
    close(ifd);

    FILE *df = fopen(diffp, "w");
    if (!df) return -1;
    fputs(seg, df);
    fclose(df);

    char cmd[16384];
    snprintf(cmd, sizeof cmd, "patch --merge -s -p1 '%s' < '%s' 2>/dev/null", inp, diffp);
    int rc = system(cmd);
    if (rc != 0 && rc != 1) {
        snprintf(cmd, sizeof cmd, "patch --merge -s -p0 '%s' < '%s' 2>/dev/null", inp, diffp);
        rc = system(cmd);
    }
    if (rc != 0 && rc != 1) return -1;

    struct stat st;
    stat(inp, &st);
    int fd = open(inp, O_RDONLY);
    if (fd < 0) return -1;

    if (rc == 1) {
        /* conflicts -> print the real diff-conflict, do not commit */
        print_diff_conflict(ps, relpath, cur, inp);
        close(fd);
        return -2;
    }

    int r = merge_put_fd(ps, relpath, fd, st.st_size);
    close(fd);
    return r;
}

int ps_apply_gitdiff(ps_store *ps, const char *diff_text) {
    struct diff_seg *segs = NULL;
    int n = split_diff(diff_text, &segs);
    int applied = 0;
    for (int i = 0; i < n; i++) {
        char relpath[4096];
        snprintf(relpath, sizeof relpath, "/%s", segs[i].path);
        int rc = apply_file_diff(ps, relpath, segs[i].text);
        if (rc == 0) applied++;
        else if (rc == -2) fprintf(stderr, "[overlay] patch conflict on %s (see above + messages/)\n", relpath);
        free(segs[i].path);
        free(segs[i].text);
    }
    free(segs);
    return applied;
}
