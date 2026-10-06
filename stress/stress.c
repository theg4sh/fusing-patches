/* stress/stress.c — native C stress harness (mounts agents, hammers them with
 * multiple pthreads per mount, verifies, unmounts). Replaces the shell stress
 * scripts; run via `make stress-test` / `make stress-long`.
 *
 * usage: stress <fusing-patches binary> [options]
 *   --duration=SECS   sustained mode (default: burst)
 *   --agents=A        overlay mountpoints to drive   (default 4)
 *   --threads=T       worker threads per agent       (default 2)
 *   --files=F         disjoint files per thread      (default 20)
 *   --iters=N         burst: iterations per file     (default 20)
 *   --noop=N          thread 0 also does N identical rewrites
 *   --shared          add shared-file contention (soft-block = expected)
 *   --verify          verify final content (default on in burst mode)
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char overlay_bin[4096];
static char base[4096], lower[4096], store[4096];
static char mounts[64][4096];
static pid_t agents[64];
static int nagent;

static int duration, threads = 2, files = 20, iters = 20, noop, shared, verify = 1;

struct worker {
    const char *mount;
    int gid;
    volatile int *stop;
    long writes, errors, blocks;
    char last_err[128];
};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void write_one(struct worker *w, const char *path, const char *content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        w->errors++;
        snprintf(w->last_err, sizeof w->last_err, "open: %s", strerror(errno));
        return;
    }
    ssize_t n = write(fd, content, strlen(content));
    if (n < 0) {
        w->errors++;
        snprintf(w->last_err, sizeof w->last_err, "write: %s", strerror(errno));
    } else {
        w->writes++;
    }
    if (close(fd) < 0) {
        if (errno == EAGAIN) w->blocks++; /* soft-blocked: expected under contention */
        else {
            w->errors++;
            snprintf(w->last_err, sizeof w->last_err, "close: %s", strerror(errno));
        }
    }
}

static void *worker_main(void *arg) {
    struct worker *w = arg;
    char path[4096], content[256];

    if (duration > 0) {
        int k = w->gid;
        while (!*w->stop) {
            int f = k % files;
            snprintf(path, sizeof path, "%s/stress-%d-%d.txt", w->mount, w->gid, f);
            snprintf(content, sizeof content, "agent%d iter%d\n", w->gid, k);
            write_one(w, path, content);
            k++;
        }
    } else {
        for (int f = 0; f < files; f++)
            for (int i = 0; i < iters; i++) {
                snprintf(path, sizeof path, "%s/stress-%d-%d.txt", w->mount, w->gid, f);
                snprintf(content, sizeof content, "agent%d file%d iter%d\n", w->gid, f, i);
                write_one(w, path, content);
            }
    }

    if (w->gid == 0 && noop > 0) {
        snprintf(path, sizeof path, "%s/noop.txt", w->mount);
        write_one(w, path, "constant content\n");
        usleep(300000);
        for (int i = 0; i < noop; i++)
            write_one(w, path, "constant content\n");
    }
    return NULL;
}

/* shared-file contention: soft-blocks (EAGAIN) are expected, not errors */
static void *shared_main(void *arg) {
    struct worker *w = arg;
    char path[4096], content[256];
    int k = 0;
    while (!*w->stop) {
        snprintf(path, sizeof path, "%s/shared-%d.txt", w->mount, k % 4);
        snprintf(content, sizeof content, "agent%d shared iter%d\n", w->gid, k);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            w->errors++;
            snprintf(w->last_err, sizeof w->last_err, "%s", strerror(errno));
        } else {
            ssize_t n = write(fd, content, strlen(content));
            if (n < 0) w->errors++;
            else w->writes++;
            if (close(fd) < 0 && errno == EAGAIN) w->blocks++;
        }
        k++;
    }
    return NULL;
}

static void spawn_agents(void) {
    for (int i = 0; i < nagent; i++) {
        agents[i] = fork();
        if (agents[i] == 0) {
            char log[4096];
            snprintf(log, sizeof log, "%s/agent%d.log", base, i);
            int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, 2);
                close(fd);
            }
            char name[32];
            snprintf(name, sizeof name, "--name=agent%d", i);
            execl(overlay_bin, overlay_bin, lower, store, mounts[i], name, "-f", (char *)NULL);
            _exit(127);
        }
    }
    sleep(1); /* let mounts come up */
}

static void stop_agents(void) {
    for (int i = 0; i < nagent; i++) {
        kill(agents[i], SIGTERM);
        waitpid(agents[i], NULL, 0);
    }
}

static int read_file(const char *path, char *buf, size_t sz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sz - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

/* read until the content matches (async commits settle) or timeout */
static int read_until(const char *path, const char *expected, char *got, size_t sz, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        if (read_file(path, got, sz) == 0 && strcmp(got, expected) == 0) return 1;
        usleep(20000);
    }
    read_file(path, got, sz);
    return 0;
}

/* recursive delete using unlink/rmdir (no shell) */
static void rmtree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    char child[4096];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue; /* skip "." and ".." */
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        rmtree(child);
    }
    closedir(d);
    rmdir(path);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: stress <fusing-patches binary> [options]\n");
        return 2;
    }
    snprintf(overlay_bin, sizeof overlay_bin, "%s", argv[1]);
    nagent = 4;

    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--duration=", 11) == 0) duration = atoi(argv[i] + 11);
        else if (strncmp(argv[i], "--agents=", 9) == 0) nagent = atoi(argv[i] + 9);
        else if (strncmp(argv[i], "--threads=", 10) == 0) threads = atoi(argv[i] + 10);
        else if (strncmp(argv[i], "--files=", 8) == 0) files = atoi(argv[i] + 8);
        else if (strncmp(argv[i], "--iters=", 8) == 0) iters = atoi(argv[i] + 8);
        else if (strncmp(argv[i], "--noop=", 7) == 0) noop = atoi(argv[i] + 7);
        else if (strcmp(argv[i], "--shared") == 0) shared = 1;
        else if (strcmp(argv[i], "--verify") == 0) verify = 1;
    }

    char tmpl[] = "/tmp/overlay-stress-XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
    snprintf(base, sizeof base, "%s", tmpl);
    snprintf(lower, sizeof lower, "%s/lower", base);
    snprintf(store, sizeof store, "%s/store", base);
    mkdir(lower, 0755);
    mkdir(store, 0755);
    for (int i = 0; i < nagent; i++) {
        snprintf(mounts[i], sizeof mounts[i], "%s/mnt%d", base, i);
        mkdir(mounts[i], 0755);
    }
    char seed[4096];
    snprintf(seed, sizeof seed, "%s/seed.txt", lower);
    int sfd = open(seed, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sfd >= 0) { write(sfd, "seed\n", 5); close(sfd); }

    spawn_agents();

    int total = nagent * threads;
    struct worker *ws = calloc(total, sizeof *ws);
    pthread_t *ts = calloc(total, sizeof *ts);
    volatile int stop = 0;
    for (int mi = 0; mi < nagent; mi++)
        for (int t = 0; t < threads; t++) {
            int gid = mi * threads + t;
            ws[gid].mount = mounts[mi];
            ws[gid].gid = gid;
            ws[gid].stop = &stop;
        }

    double t0 = now_s();
    for (int i = 0; i < total; i++)
        pthread_create(&ts[i], NULL, worker_main, &ws[i]);

    struct worker *sw = NULL;
    pthread_t *sts = NULL;
    if (shared) {
        sw = calloc(nagent, sizeof *sw);
        sts = calloc(nagent, sizeof *sts);
        for (int i = 0; i < nagent; i++) {
            sw[i].mount = mounts[i];
            sw[i].gid = 1000 + i;
            sw[i].stop = &stop;
            pthread_create(&sts[i], NULL, shared_main, &sw[i]);
        }
    }

    if (duration > 0) {
        struct timespec sl = { duration, 0 };
        nanosleep(&sl, NULL);
        stop = 1;
    }

    for (int i = 0; i < total; i++) pthread_join(ts[i], NULL);
    if (shared) for (int i = 0; i < nagent; i++) pthread_join(sts[i], NULL);
    double elapsed = now_s() - t0;

    long writes = 0, errors = 0, blocks = 0;
    for (int i = 0; i < total; i++) { writes += ws[i].writes; errors += ws[i].errors; blocks += ws[i].blocks; }
    if (shared) for (int i = 0; i < nagent; i++) { writes += sw[i].writes; errors += sw[i].errors; blocks += sw[i].blocks; }

    printf("writes=%ld errors=%ld blocks=%ld elapsed=%.2fs rate=%.0f w/s\n",
           writes, errors, blocks, elapsed, elapsed > 0 ? writes / elapsed : 0);

    int rc = 0;
    if (duration == 0 && verify) {
        char path[4096], expected[256], got[4096];
        snprintf(path, sizeof path, "%s/stress-0-5.txt", mounts[0]);
        snprintf(expected, sizeof expected, "agent0 file5 iter%d\n", iters - 1);
        if (read_until(path, expected, got, sizeof got, 5000))
            printf("PASS: content agent0/stress-0-5.txt\n");
        else { printf("FAIL: content agent0/stress-0-5.txt (got '%s')\n", got); rc = 1; }

        if (noop > 0) {
            snprintf(path, sizeof path, "%s/noop.txt", mounts[0]);
            if (read_until(path, "constant content\n", got, sizeof got, 5000))
                printf("PASS: noop.txt content\n");
            else { printf("FAIL: noop.txt content (got '%s')\n", got); rc = 1; }
        }
    }

    if (errors == 0) printf("PASS: no errors\n");
    else {
        printf("FAIL: %ld errors\n", errors);
        for (int i = 0; i < total; i++)
            if (ws[i].errors) printf("  worker %d: %ld errors, last: %s\n", i, ws[i].errors, ws[i].last_err);
        if (shared)
            for (int i = 0; i < nagent; i++)
                if (sw[i].errors) printf("  shared worker %d: %ld errors, last: %s\n", i, sw[i].errors, sw[i].last_err);
        rc = 1;
    }

    for (int i = 0; i < nagent; i++)
        if (kill(agents[i], 0) == 0) { /* alive */ }
        else { printf("FAIL: agent%d died\n", i); rc = 1; }

    stop_agents();

    if (rc != 0)
        printf("KEEP %s\n", base); /* inspect agent logs on failure */
    else
        rmtree(base);
    free(ws); free(ts); free(sw); free(sts);
    return rc;
}
