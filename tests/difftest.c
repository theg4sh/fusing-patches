/* Unit tests for the in-process diff/merge engine (src/diff.c). */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/diff.h"

static int passes, failures;

static void check(int cond, const char *name) {
    if (cond) {
        passes++;
        printf("PASS: %s\n", name);
    } else {
        failures++;
        printf("FAIL: %s\n", name);
    }
}



int main(void) {
    int rc;

    /* --- diff_unified --- */
    {
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = diff_unified("a\nb\nc\n", 6, "A", "a\nb\nc\n", 6, "B", 3, f);
        fclose(f);
        check(rc == 0, "diff: identical returns 0");
        free(b);
    }
    {
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = diff_unified("a\nb\nc\n", 6, "A", "a\nX\nc\n", 6, "B", 3, f);
        fclose(f);
        check(rc == 1, "diff: changed returns 1");
        check(strstr(b, "-b") != NULL, "diff: contains removed line");
        check(strstr(b, "+X") != NULL, "diff: contains added line");
        check(strstr(b, "@@") != NULL, "diff: contains hunk header");
        check(strstr(b, "--- A") != NULL && strstr(b, "+++ B") != NULL,
              "diff: contains labels");
        free(b);
    }
    {
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = diff_unified("a\n", 2, "A", "a\nb\n", 4, "B", 3, f);
        fclose(f);
        check(rc == 1 && strstr(b, "+b") != NULL, "diff: pure insert");
        free(b);
    }
    {
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = diff_unified("a\nb\n", 4, "A", "a\n", 2, "B", 3, f);
        fclose(f);
        check(rc == 1 && strstr(b, "-b") != NULL, "diff: pure delete");
        free(b);
    }
    {
        /* no trailing newline marker */
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = diff_unified("a\nb", 3, "A", "a\nc", 3, "B", 3, f);
        fclose(f);
        check(rc == 1 && strstr(b, "No newline") != NULL, "diff: no-newline marker");
        free(b);
    }
    {
        /* completely different -> still produces output (fallback or fine diff) */
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = diff_unified("one\ntwo\nthree\n", 14, "A", "x\ny\nz\n", 6, "B", 3, f);
        fclose(f);
        check(rc == 1 && strstr(b, "@@") != NULL, "diff: fully-different files");
        free(b);
    }

    /* --- merge3 --- */
    {
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = merge3("a\nb\nc\nd\n", 8, "a\nB\nc\nd\n", 8, "a\nb\nc\nD\n", 8, f);
        fclose(f);
        check(rc == 0, "merge: non-overlapping is clean");
        check(strstr(b, "B") != NULL && strstr(b, "D") != NULL,
              "merge: both changes kept");
        check(strstr(b, "<<<<<<<") == NULL, "merge: no markers when clean");
        free(b);
    }
    {
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = merge3("a\nb\nc\n", 6, "a\nX\nc\n", 6, "a\nY\nc\n", 6, f);
        fclose(f);
        check(rc == 1, "merge: overlapping is conflict");
        check(strstr(b, "<<<<<<< theirs") != NULL, "merge: theirs marker");
        check(strstr(b, ">>>>>>> ours") != NULL, "merge: ours marker");
        check(strstr(b, "X") != NULL && strstr(b, "Y") != NULL,
              "merge: both sides present in conflict");
        free(b);
    }
    {
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = merge3("a\nb\n", 4, "a\nb\n", 4, "a\nb\n", 4, f);
        fclose(f);
        check(rc == 0 && strcmp(b, "a\nb\n") == 0, "merge: identical base");
        free(b);
    }
    {
        /* both sides append at EOF -> conflict */
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = merge3("a\n", 2, "a\nt\n", 4, "a\no\n", 4, f);
        fclose(f);
        check(rc == 1, "merge: EOF insert on both sides conflicts");
        free(b);
    }
    {
        /* one side changes, other untouched -> clean, change kept */
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        rc = merge3("a\nb\n", 4, "a\nZ\n", 4, "a\nb\n", 4, f);
        fclose(f);
        check(rc == 0 && strstr(b, "Z") != NULL, "merge: one-sided change clean");
        free(b);
    }

    /* --- apply_unified_diff (in-process patch replacement) --- */
    {
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        const char *p = "@@ -2 +2 @@\n-b\n+X\n";
        rc = apply_unified_diff("a\nb\nc\n", 6, p, strlen(p), f);
        fclose(f);
        check(rc == 0 && strcmp(b, "a\nX\nc\n") == 0, "apply: clean modify");
        free(b);
    }
    {
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        const char *p = "@@ -3,0 +4 @@\n+NEW\n";
        rc = apply_unified_diff("a\nb\n", 4, p, strlen(p), f);
        fclose(f);
        check(rc == 0 && strcmp(b, "a\nb\nNEW\n") == 0, "apply: insertion at EOF");
        free(b);
    }
    {
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        const char *p = "@@ -2 +1 @@\n-b\n";
        rc = apply_unified_diff("a\nb\nc\n", 6, p, strlen(p), f);
        fclose(f);
        check(rc == 0 && strcmp(b, "a\nc\n") == 0, "apply: deletion");
        free(b);
    }
    {
        /* context mismatch at the claimed position -> offset search finds it */
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        const char *p = "@@ -1 +1 @@\n-a\n+X\n";
        rc = apply_unified_diff("x\na\nb\nc\n", 8, p, strlen(p), f);
        fclose(f);
        check(rc == 0 && strcmp(b, "x\nX\nb\nc\n") == 0, "apply: offset search");
        free(b);
    }
    {
        /* unmatched hunk -> conflict markers, rc 1 */
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        const char *p = "@@ -9 +9 @@\n-ZZZ\n";
        rc = apply_unified_diff("a\nb\nc\n", 6, p, strlen(p), f);
        fclose(f);
        check(rc == 1, "apply: unmatched hunk is conflict");
        check(strstr(b, "<<<<<<< base") != NULL && strstr(b, ">>>>>>> patch") != NULL,
              "apply: conflict markers");
        free(b);
    }
    {
        /* two hunks */
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        const char *p = "@@ -1 +1 @@\n-a\n+A\n@@ -4 +4 @@\n-d\n+D\n";
        rc = apply_unified_diff("a\nb\nc\nd\n", 8, p, strlen(p), f);
        fclose(f);
        check(rc == 0 && strcmp(b, "A\nb\nc\nD\n") == 0, "apply: multi-hunk");
        free(b);
    }
    {
        /* no trailing newline in base, "\ No newline" marker ignored */
        char *b = NULL;
        size_t l = 0;
        FILE *f = open_memstream(&b, &l);
        const char *p = "@@ -2 +2 @@\n-b\n+B\n\\ No newline at end of file\n";
        rc = apply_unified_diff("a\nb", 3, p, strlen(p), f);
        fclose(f);
        check(rc == 0 && strcmp(b, "a\nB\n") == 0, "apply: no-newline base + marker");
        free(b);
    }

    printf("\n== diff engine: %d passed, %d failed ==\n", passes, failures);
    return failures ? 1 : 0;
}
