/* In-process line diff + 3-way merge. See diff.h. */
#include "diff.h"

#include <stdlib.h>
#include <string.h>

/* a line: content including the trailing '\n' if present */
struct slice {
    const char *ptr;
    size_t len;
};

enum op_type { OP_EQ = 0, OP_DEL = 1, OP_INS = 2 };

struct op {
    int type;
    int a0, a1; /* a (base/old) range */
    int b0, b1; /* b (new) range */
};

/* max total lines for the exact Myers diff; larger inputs fall back to a
 * coarse replace-all edit script (still correct output) */
#define MAX_DIFF_LINES 2048

static int split_lines(const char *buf, size_t len, struct slice **out) {
    struct slice *lines = NULL;
    int n = 0, cap = 0;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            if (n == cap) {
                size_t ncap = cap ? (size_t)cap * 2 : 256;
                struct slice *tmp = realloc(lines, ncap * sizeof *lines);
                if (!tmp) {
                    free(lines);
                    return -1;
                }
                lines = tmp;
                cap = (int)ncap;
            }
            lines[n].ptr = buf + start;
            lines[n].len = i - start + 1; /* include '\n' */
            n++;
            start = i + 1;
        }
    }
    if (start < len) { /* last line without trailing newline */
        if (n == cap) {
            size_t ncap = cap ? (size_t)cap * 2 : 256;
            struct slice *tmp = realloc(lines, ncap * sizeof *lines);
            if (!tmp) {
                free(lines);
                return -1;
            }
            lines = tmp;
            cap = (int)ncap;
        }
        lines[n].ptr = buf + start;
        lines[n].len = len - start;
        n++;
    }
    *out = lines;
    return n;
}

static int slice_eq(const struct slice *a, const struct slice *b) {
    return a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
}

static void emit_line(FILE *out, char prefix, const struct slice *l) {
    int has_nl = l->len > 0 && l->ptr[l->len - 1] == '\n';
    fprintf(out, "%c", prefix);
    fwrite(l->ptr, 1, l->len, out);
    if (!has_nl) {
        fputc('\n', out);
        fprintf(out, "\\ No newline at end of file\n");
    }
}

/* raw line, no diff prefix (for merge output) */
static void emit_raw(FILE *out, const struct slice *l) {
    fwrite(l->ptr, 1, l->len, out);
    if (l->len == 0 || l->ptr[l->len - 1] != '\n')
        fputc('\n', out);
}

/* ------------------------------------------------------------------ */
/* Myers O(ND) diff with backtracking trace                            */
/* ------------------------------------------------------------------ */

struct ops_list {
    struct op *ops;
    int n, cap;
};

static int ops_add(struct ops_list *L, int type, int a0, int a1, int b0, int b1) {
    if (L->n == L->cap) {
        size_t ncap = L->cap ? (size_t)L->cap * 2 : 64;
        struct op *tmp = realloc(L->ops, ncap * sizeof *L->ops);
        if (!tmp) return -1;
        L->ops = tmp;
        L->cap = (int)ncap;
    }
    L->ops[L->n++] = (struct op){ type, a0, a1, b0, b1 };
    return 0;
}

/* returns number of ops, or -1 on error */
static int myers(struct slice *a, int n, struct slice *b, int m, struct op **out) {
    if (n + m > MAX_DIFF_LINES) {
        /* coarse fallback: replace all */
        struct op *ops = malloc(2 * sizeof *ops);
        if (!ops) return -1;
        ops[0] = (struct op){ OP_DEL, 0, n, 0, 0 };
        ops[1] = (struct op){ OP_INS, 0, 0, 0, m };
        *out = ops;
        return (n || m) ? 2 : 0;
    }

    int max = n + m;
    int offset = max + 1;
    int vlen = 2 * max + 3;
    int **trace = calloc((size_t)max + 1, sizeof *trace);
    if (!trace) return -1;

    int D = max;
    for (int d = 0; d <= max; d++) {
        trace[d] = malloc((size_t)vlen * sizeof(int));
        if (!trace[d]) {
            for (int k = 0; k < d; k++) free(trace[k]);
            free(trace);
            return -1;
        }
        int *V = trace[d];
        if (d == 0) {
            for (int i = 0; i < vlen; i++) V[i] = -1;
            V[offset + 1] = 0;
        } else {
            memcpy(V, trace[d - 1], (size_t)vlen * sizeof(int));
        }
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && V[offset + k - 1] < V[offset + k + 1]))
                x = V[offset + k + 1];
            else
                x = V[offset + k - 1] + 1;
            int y = x - k;
            while (x < n && y < m && slice_eq(&a[x], &b[y])) {
                x++;
                y++;
            }
            V[offset + k] = x;
            if (x >= n && y >= m) {
                D = d;
                goto traced;
            }
        }
    }

traced:
    /* backtrack (ops come out in reverse) */
    struct ops_list L = { NULL, 0, 0 };
    int x = n, y = m;
    for (int d = D; d >= 0; d--) {
        int *V = trace[d];
        int k = x - y;
        int pk;
        if (k == -d || (k != d && V[offset + k - 1] < V[offset + k + 1]))
            pk = k + 1;
        else
            pk = k - 1;
        int px = V[offset + pk];
        int py = px - pk;
        while (x > px && y > py) {
            if (ops_add(&L, OP_EQ, x - 1, x, y - 1, y) != 0) goto oom;
            x--;
            y--;
        }
        if (d > 0) {
            if (x == px) {
                if (ops_add(&L, OP_INS, px, px, y - 1, y) != 0) goto oom;
                y--;
            } else {
                if (ops_add(&L, OP_DEL, x - 1, x, py, py) != 0) goto oom;
                x--;
            }
        }
    }
    while (x > 0 && y > 0) {
        if (ops_add(&L, OP_EQ, x - 1, x, y - 1, y) != 0) goto oom;
        x--;
        y--;
    }
    while (x > 0) {
        if (ops_add(&L, OP_DEL, x - 1, x, y, y) != 0) goto oom;
        x--;
    }
    while (y > 0) {
        if (ops_add(&L, OP_INS, x, x, y - 1, y) != 0) goto oom;
        y--;
    }

    for (int i = 0; i < L.n / 2; i++) {
        struct op t = L.ops[i];
        L.ops[i] = L.ops[L.n - 1 - i];
        L.ops[L.n - 1 - i] = t;
    }

    for (int d = 0; d <= D; d++) free(trace[d]);
    free(trace);
    *out = L.ops;
    return L.n;

oom:
    for (int d = 0; d <= D; d++) free(trace[d]);
    free(trace);
    free(L.ops);
    return -1;
}

/* ------------------------------------------------------------------ */
/* unified diff output                                                 */
/* ------------------------------------------------------------------ */

static void print_hunk_header(FILE *out, int a0, int a_cnt, int b0, int b_cnt) {
    fprintf(out, "@@ -");
    if (a_cnt == 0)
        fprintf(out, "0,0");
    else if (a_cnt == 1)
        fprintf(out, "%d", a0 + 1);
    else
        fprintf(out, "%d,%d", a0 + 1, a_cnt);
    fprintf(out, " +");
    if (b_cnt == 0)
        fprintf(out, "0,0");
    else if (b_cnt == 1)
        fprintf(out, "%d", b0 + 1);
    else
        fprintf(out, "%d,%d", b0 + 1, b_cnt);
    fprintf(out, " @@\n");
}

int diff_unified(const char *a, size_t a_len, const char *label_a,
                 const char *b, size_t b_len, const char *label_b,
                 int context, FILE *out) {
    if (context < 0) context = 3;

    struct slice *sa = NULL, *sb = NULL;
    int n = split_lines(a, a_len, &sa);
    int m = split_lines(b, b_len, &sb);
    if (n < 0 || m < 0) {
        free(sa);
        free(sb);
        return -1;
    }

    struct op *ops = NULL;
    int nops = myers(sa, n, sb, m, &ops);
    if (nops < 0) {
        free(sa);
        free(sb);
        return -1;
    }

    int changes = 0;
    for (int i = 0; i < nops; i++)
        if (ops[i].type != OP_EQ) changes = 1;
    if (!changes) {
        free(ops);
        free(sa);
        free(sb);
        return 0;
    }

    fprintf(out, "--- %s\n", label_a);
    fprintf(out, "+++ %s\n", label_b);

    int i = 0;
    while (i < nops) {
        while (i < nops && ops[i].type == OP_EQ) i++;
        if (i >= nops) break;
        int first = i;

        /* walk back up to `context` EQ ops */
        int h_start = first;
        for (int k = 1; k <= context && first - k >= 0; k++) {
            if (ops[first - k].type == OP_EQ)
                h_start = first - k;
            else
                break;
        }

        /* scan forward: changes + up to `context` trailing EQ ops */
        int j = first;
        int eqrun = 0;
        while (j < nops) {
            if (ops[j].type == OP_EQ) {
                if (eqrun >= context) break;
                eqrun++;
            } else {
                eqrun = 0;
            }
            j++;
        }

        int a0 = ops[h_start].a0;
        int b0 = ops[h_start].b0;
        int a1 = ops[j - 1].a1;
        int b1 = ops[j - 1].b1;
        print_hunk_header(out, a0, a1 - a0, b0, b1 - b0);

        for (int k = h_start; k < j; k++) {
            struct op *o = &ops[k];
            if (o->type == OP_EQ)
                emit_line(out, ' ', &sa[o->a0]);
            else if (o->type == OP_DEL)
                emit_line(out, '-', &sa[o->a0]);
            else
                emit_line(out, '+', &sb[o->b0]);
        }
        i = j;
    }

    free(ops);
    free(sa);
    free(sb);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 3-way merge                                                         */
/* ------------------------------------------------------------------ */

struct region {
    int start, end;     /* base coords */
    struct slice *lines;
    int nlines, cap;
};

static int region_add_lines(struct region *r, const struct slice *src, int b0, int b1) {
    for (int k = b0; k < b1; k++) {
        if (r->nlines == r->cap) {
            size_t ncap = r->cap ? (size_t)r->cap * 2 : 16;
            struct slice *tmp = realloc(r->lines, ncap * sizeof *r->lines);
            if (!tmp) return -1;
            r->lines = tmp;
            r->cap = (int)ncap;
        }
        r->lines[r->nlines++] = src[k];
    }
    return 0;
}

static int build_regions(struct op *ops, int nops, struct slice *b,
                         struct region **out) {
    struct region *regions = NULL;
    int nr = 0;
    struct region *cur = NULL;

    for (int i = 0; i < nops; i++) {
        struct op *o = &ops[i];
        if (o->type == OP_EQ) {
            cur = NULL;
            continue;
        }
        if (!cur || o->a0 > cur->end) {
            struct region *tmp = realloc(regions, (size_t)(nr + 1) * sizeof *regions);
            if (!tmp) return -1;
            regions = tmp;
            cur = &regions[nr++];
            memset(cur, 0, sizeof *cur);
            cur->start = o->a0;
            cur->end = o->a1;
        } else if (o->a1 > cur->end) {
            cur->end = o->a1;
        }
        if (region_add_lines(cur, b, o->b0, o->b1) != 0) return -1;
    }
    *out = regions;
    return nr;
}

static void emit_slices(FILE *out, struct slice *lines, int nlines) {
    for (int i = 0; i < nlines; i++)
        emit_raw(out, &lines[i]);
}

int merge3(const char *base, size_t base_len,
           const char *theirs, size_t theirs_len,
           const char *ours, size_t ours_len,
           FILE *out) {
    struct slice *sb = NULL, *st = NULL, *so = NULL;
    int n = split_lines(base, base_len, &sb);
    int nt = split_lines(theirs, theirs_len, &st);
    int no = split_lines(ours, ours_len, &so);
    if (n < 0 || nt < 0 || no < 0) {
        free(sb); free(st); free(so);
        return -1;
    }

    struct op *ops_t = NULL, *ops_o = NULL;
    int nops_t = myers(sb, n, st, nt, &ops_t);
    int nops_o = myers(sb, n, so, no, &ops_o);
    if (nops_t < 0 || nops_o < 0) {
        free(sb); free(st); free(so);
        free(ops_t); free(ops_o);
        return -1;
    }

    struct region *rt = NULL, *ro = NULL;
    int nrt = build_regions(ops_t, nops_t, st, &rt);
    int nro = build_regions(ops_o, nops_o, so, &ro);
    if (nrt < 0 || nro < 0) {
        free(sb); free(st); free(so);
        free(ops_t); free(ops_o); free(rt); free(ro);
        return -1;
    }

    int conflicts = 0;
    int pt = 0, po = 0;
    int i = 0;

    while (i < n) {
        int t_here = pt < nrt && rt[pt].start <= i;
        int o_here = po < nro && ro[po].start <= i;

        if (!t_here && !o_here) {
            emit_raw(out, &sb[i]);
            i++;
            continue;
        }

        if (t_here && o_here) {
            conflicts = 1;
            fprintf(out, "<<<<<<< theirs\n");
            emit_slices(out, rt[pt].lines, rt[pt].nlines);
            fprintf(out, "=======\n");
            emit_slices(out, ro[po].lines, ro[po].nlines);
            fprintf(out, ">>>>>>> ours\n");
            int adv = rt[pt].end > ro[po].end ? rt[pt].end : ro[po].end;
            if (adv < i) adv = i;
            i = adv;
            pt++;
            po++;
            continue;
        }

        if (t_here) {
            emit_slices(out, rt[pt].lines, rt[pt].nlines);
            i = rt[pt].end > i ? rt[pt].end : i;
            pt++;
        } else {
            emit_slices(out, ro[po].lines, ro[po].nlines);
            i = ro[po].end > i ? ro[po].end : i;
            po++;
        }
    }

    /* trailing insertions at EOF */
    int t_trail = pt < nrt, o_trail = po < nro;
    if (t_trail && o_trail) {
        conflicts = 1;
        fprintf(out, "<<<<<<< theirs\n");
        emit_slices(out, rt[pt].lines, rt[pt].nlines);
        fprintf(out, "=======\n");
        emit_slices(out, ro[po].lines, ro[po].nlines);
        fprintf(out, ">>>>>>> ours\n");
    } else if (t_trail) {
        emit_slices(out, rt[pt].lines, rt[pt].nlines);
    } else if (o_trail) {
        emit_slices(out, ro[po].lines, ro[po].nlines);
    }

    for (int k = 0; k < nrt; k++) free(rt[k].lines);
    for (int k = 0; k < nro; k++) free(ro[k].lines);
    free(rt);
    free(ro);
    free(ops_t);
    free(ops_o);
    free(sb);
    free(st);
    free(so);
    return conflicts;
}

/* ------------------------------------------------------------------ */
/* unified diff application (in-process `patch` replacement)           */
/* ------------------------------------------------------------------ */

/* parse "@@ -a[,b] +c[,d] @@" — returns 0 on success */
static int parse_hunk_header(const char *line, long *a, long *b, long *c, long *d) {
    const char *p = line;
    char *end;
    if (strncmp(p, "@@ -", 4) != 0) return -1;
    p += 4;
    *a = strtol(p, &end, 10);
    if (end == p) return -1;
    p = end;
    *b = 1;
    if (*p == ',') {
        p++;
        *b = strtol(p, &end, 10);
        if (end == p) return -1;
        p = end;
    }
    while (*p == ' ') p++;
    if (*p != '+') return -1;
    p++;
    *c = strtol(p, &end, 10);
    if (end == p) return -1;
    p = end;
    *d = 1;
    if (*p == ',') {
        p++;
        *d = strtol(p, &end, 10);
        if (end == p) return -1;
        p = end;
    }
    (void)b;
    (void)d;
    return 0;
}

/* compare a patch line against a base line, ignoring trailing-newline/CR */
static int patch_line_match(const struct slice *base, const char *pl, size_t plen) {
    size_t blen = base->len;
    if (blen && base->ptr[blen - 1] == '\n') blen--;
    if (blen && base->ptr[blen - 1] == '\r') blen--;
    if (plen && pl[plen - 1] == '\n') plen--;
    if (plen && pl[plen - 1] == '\r') plen--;
    return blen == plen && memcmp(base->ptr, pl, blen) == 0;
}

struct hunk_line {
    char kind;       /* ' ', '-', '+' */
    struct slice s;  /* content WITHOUT the leading kind char */
};

static int hunk_append(struct hunk_line **h, int *n, int *cap, char kind, struct slice s) {
    if (*n == *cap) {
        size_t ncap = *cap ? (size_t)*cap * 2 : 64;
        struct hunk_line *tmp = realloc(*h, ncap * sizeof *tmp);
        if (!tmp) return -1;
        *h = tmp;
        *cap = (int)ncap;
    }
    (*h)[*n].kind = kind;
    (*h)[*n].s = s;
    (*n)++;
    return 0;
}

/* emit a patch line's content as a result line (always newline-terminated) */
static void emit_patch_line(FILE *out, const struct slice *s) {
    fwrite(s->ptr, 1, s->len, out);
    if (s->len == 0 || s->ptr[s->len - 1] != '\n')
        fputc('\n', out);
}

int apply_unified_diff(const char *base, size_t base_len,
                       const char *patch, size_t patch_len,
                       FILE *out) {
    struct slice *bl = NULL;
    int bn = split_lines(base, base_len, &bl);
    if (bn < 0) return -1;

    struct slice *pl = NULL;
    int pn = split_lines(patch, patch_len, &pl);
    if (pn < 0) {
        free(bl);
        return -1;
    }

    int conflicts = 0;
    long base_pos = 0; /* next unchanged base line to emit */
    long delta = 0;    /* applied line-count offset (inserts - deletes) */

    for (int i = 0; i < pn;) {
        long ha, hb, hc, hd;
        if (pl[i].len < 4 || strncmp(pl[i].ptr, "@@ -", 4) != 0 ||
            parse_hunk_header(pl[i].ptr, &ha, &hb, &hc, &hd) != 0) {
            i++;
            continue;
        }
        i++;

        /* collect the hunk body */
        struct hunk_line *body = NULL;
        int nbody = 0, capbody = 0;
        int n_before = 0, n_plus = 0, n_minus = 0;
        while (i < pn) {
            struct slice *s = &pl[i];
            if (s->len == 0) break;
            char k = s->ptr[0];
            if (k != ' ' && k != '-' && k != '+') break; /* next hunk / junk */
            struct slice cs = { s->ptr + 1, s->len - 1 };
            if (hunk_append(&body, &nbody, &capbody, k, cs) != 0) {
                free(body);
                free(pl);
                free(bl);
                return -1;
            }
            if (k == '+') n_plus++;
            else {
                n_before++;
                if (k == '-') n_minus++;
            }
            i++;
        }

        /* find the position where the before-side matches */
        long expected = (ha - 1) + delta;
        if (expected < 0) expected = 0;
        if (expected > bn) expected = bn;

        long pos = -1;
        for (long off = 0; pos < 0 && off <= bn; off++) {
            long cands[2] = { expected + off, expected - off };
            for (int t = 0; t < 2 && pos < 0; t++) {
                long cand = cands[t];
                if (cand < 0 || cand + n_before > bn) continue;
                int ok = 1;
                int j = 0;
                for (int k = 0; k < nbody && ok; k++) {
                    if (body[k].kind == '+') continue;
                    if (!patch_line_match(&bl[cand + j], body[k].s.ptr, body[k].s.len))
                        ok = 0;
                    j++;
                }
                if (ok) pos = cand;
            }
        }

        if (pos >= 0) {
            /* emit unchanged lines before the hunk */
            long from = base_pos > pos ? pos : base_pos;
            for (long k = from; k < pos && k < bn; k++)
                emit_raw(out, &bl[k]);

            /* apply: context lines from base, insertions from the patch */
            int j = 0;
            for (int k = 0; k < nbody; k++) {
                if (body[k].kind == ' ') {
                    if (pos + j < bn) emit_raw(out, &bl[pos + j]);
                    j++;
                } else if (body[k].kind == '-') {
                    j++;
                } else {
                    emit_patch_line(out, &body[k].s);
                }
            }
            delta += n_plus - n_minus;
            base_pos = pos + n_before;
        } else {
            /* unmatched hunk -> conflict block */
            conflicts = 1;
            fprintf(out, "<<<<<<< base\n");
            for (int k = 0; k < nbody; k++)
                if (body[k].kind != '+') emit_patch_line(out, &body[k].s);
            fprintf(out, "=======\n");
            for (int k = 0; k < nbody; k++)
                if (body[k].kind != '-') emit_patch_line(out, &body[k].s);
            fprintf(out, ">>>>>>> patch\n");
        }

        free(body);
    }

    for (long k = base_pos; k < bn; k++)
        emit_raw(out, &bl[k]);

    free(pl);
    free(bl);
    return conflicts;
}
