#ifndef DIFF_H
#define DIFF_H

#include <stddef.h>
#include <stdio.h>

/*
 * In-process line diff / 3-way merge engine (replaces `diff -u` and
 * `diff3 -m`). No external tools, no shell, no fork — safe for use from
 * FUSE worker threads and fully static-linkable.
 */

/*
 * Write a unified diff (like `diff -u`) between two buffers to `out`.
 * `context` is the number of context lines around each change.
 * Returns 0 if the buffers are identical, 1 if they differ, -1 on error.
 */
int diff_unified(const char *a, size_t a_len, const char *label_a,
                 const char *b, size_t b_len, const char *label_b,
                 int context, FILE *out);

/*
 * Git-style 3-way merge of base/theirs/ours into `out`. Non-overlapping
 * changes are combined; overlapping changes are emitted with conflict
 * markers (<<<<<<< theirs / ======= / >>>>>>> ours).
 * Returns 1 if conflicts were emitted, 0 if the merge was clean, -1 on error.
 */
int merge3(const char *base, size_t base_len,
           const char *theirs, size_t theirs_len,
           const char *ours, size_t ours_len,
           FILE *out);

/*
 * Apply a unified diff (the hunk section of a git/diff patch; file headers
 * are not required) to `base`, writing the result to `out`. Hunks are
 * matched by context with a forward/backward offset search; a hunk that
 * cannot be matched is emitted as a git-style conflict block.
 * Returns 0 if applied cleanly, 1 if conflicts were emitted, -1 on error.
 */
int apply_unified_diff(const char *base, size_t base_len,
                       const char *patch, size_t patch_len,
                       FILE *out);

#endif /* DIFF_H */
