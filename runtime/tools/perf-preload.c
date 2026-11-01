// P1: amostrador de CPU por LD_PRELOAD, para máquinas sem `perf` (e com ptrace_scope=1).
//
// Uso:  NXRT_PERF_OUT=<dir> LD_PRELOAD=build-perf/libperf-preload.so build-perf/nxrt ...
//   - ITIMER_PROF (tempo de CPU do processo, 200 Hz por padrão): cada SIGPROF grava
//     tid + timestamp + backtrace() em um anel; um thread de apoio descarrega em <dir>/samples.bin.
//   - Uma vez por 100 ms o thread de apoio lê /proc/self/task/*/{stat,syscall} e grava
//     <dir>/threads.csv (t, tid, comm, estado, utime, stime, syscall): separa CPU de espera.
//   - <dir>/maps.txt guarda /proc/self/maps (regravado a cada 20 s) para simbolizar com tools/perf-report.py.
//   - Também chama prctl(PR_SET_PTRACER, ANY): eu-stack/gdb -p funcionam de fora.
// Variáveis: NXRT_PERF_HZ (padrão 200), NXRT_PERF_DEPTH (fixo 32).
// Sem NXRT_PERF_OUT a biblioteca não faz nada.
#define _GNU_SOURCE
#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DEPTH 32
#define RING (1u << 15)

typedef struct {
    uint64_t t_ns;
    uint32_t tid;
    uint32_t n;
    uint64_t pc[DEPTH];
} Sample;

static Sample ring[RING];
static _Atomic uint32_t ring_ready[RING];
static _Atomic uint64_t head;
static uint64_t tail;
static _Atomic uint64_t dropped;
static char outdir[512];
static int fd_samples = -1;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_prof(int sig, siginfo_t* si, void* uc) {
    (void)sig; (void)si; (void)uc;
    uint64_t h = atomic_fetch_add(&head, 1);
    uint32_t i = (uint32_t)(h % RING);
    if (h - tail >= RING) { atomic_fetch_add(&dropped, 1); return; }
    Sample* s = &ring[i];
    void* bt[DEPTH + 2];
    int n = backtrace(bt, DEPTH + 2);
    // pula on_prof e o trampolim do sinal
    int skip = n > 2 ? 2 : 0;
    s->n = (uint32_t)(n - skip);
    for (int k = 0; k < (int)s->n; ++k) s->pc[k] = (uint64_t)bt[k + skip];
    s->t_ns = now_ns();
    s->tid = (uint32_t)syscall(SYS_gettid);
    atomic_store_explicit(&ring_ready[i], 1, memory_order_release);
}

static void flush_samples(void) {
    for (;;) {
        uint64_t h = atomic_load(&head);
        if (tail >= h) break;
        uint32_t i = (uint32_t)(tail % RING);
        if (!atomic_load_explicit(&ring_ready[i], memory_order_acquire)) break;
        if (fd_samples >= 0) {
            Sample* s = &ring[i];
            // registro compacto: t, tid, n, pcs[n]
            uint8_t buf[16 + 8 * DEPTH];
            memcpy(buf, &s->t_ns, 8);
            memcpy(buf + 8, &s->tid, 4);
            memcpy(buf + 12, &s->n, 4);
            memcpy(buf + 16, s->pc, 8ull * s->n);
            ssize_t r = write(fd_samples, buf, 16 + 8ull * s->n);
            (void)r;
        }
        atomic_store_explicit(&ring_ready[i], 0, memory_order_relaxed);
        ++tail;
    }
}

static void write_maps(void) {
    char path[640];
    snprintf(path, sizeof path, "%s/maps.txt.tmp", outdir);
    FILE* out = fopen(path, "w");
    FILE* in = fopen("/proc/self/maps", "r");
    if (out && in) {
        char line[1024];
        while (fgets(line, sizeof line, in)) fputs(line, out);
    }
    if (in) fclose(in);
    if (out) fclose(out);
    char fin[640];
    snprintf(fin, sizeof fin, "%s/maps.txt", outdir);
    rename(path, fin);
}

static void* helper(void* arg) {
    (void)arg;
    prctl(PR_SET_NAME, "perf-helper");
    char path[640];
    snprintf(path, sizeof path, "%s/threads.csv", outdir);
    FILE* csv = fopen(path, "w");
    if (csv) fprintf(csv, "t_ns,tid,comm,state,utime,stime,syscall\n");
    uint64_t last_maps = 0;
    const long hz = sysconf(_SC_CLK_TCK);
    (void)hz;
    for (;;) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        flush_samples();
        uint64_t t = now_ns();
        if (t - last_maps > 20ull * 1000000000ull) { write_maps(); last_maps = t; }
        DIR* d = opendir("/proc/self/task");
        if (!d || !csv) { if (d) closedir(d); continue; }
        struct dirent* e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[128], buf[1024];
            snprintf(p, sizeof p, "/proc/self/task/%s/stat", e->d_name);
            int f = open(p, O_RDONLY);
            if (f < 0) continue;
            ssize_t n = read(f, buf, sizeof buf - 1);
            close(f);
            if (n <= 0) continue;
            buf[n] = 0;
            char* lp = strchr(buf, '(');
            char* rp = strrchr(buf, ')');
            if (!lp || !rp) continue;
            *rp = 0;
            char state = 0;
            unsigned long ut = 0, st = 0;
            // depois de ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
            if (sscanf(rp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &ut, &st) < 3) continue;
            char sys[64] = "";
            snprintf(p, sizeof p, "/proc/self/task/%s/syscall", e->d_name);
            f = open(p, O_RDONLY);
            if (f >= 0) {
                ssize_t m = read(f, sys, sizeof sys - 1);
                close(f);
                if (m < 0) m = 0;
                sys[m] = 0;
                char* sp = strchr(sys, ' ');
                if (sp) *sp = 0;
                char* nl = strchr(sys, '\n');
                if (nl) *nl = 0;
            }
            fprintf(csv, "%llu,%s,%s,%c,%lu,%lu,%s\n", (unsigned long long)t, e->d_name, lp + 1, state, ut, st, sys);
        }
        closedir(d);
        fflush(csv);
    }
    return NULL;
}

__attribute__((constructor)) static void perf_init(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    const char* out = getenv("NXRT_PERF_OUT");
    if (!out || !*out) return;
    // só o próprio nxrt (o LD_PRELOAD também chega a env/timeout/filhos)
    char exe[512];
    ssize_t en = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (en <= 0) return;
    exe[en] = 0;
    const char* base = strrchr(exe, '/');
    if (!base || strcmp(base + 1, "nxrt") != 0) return;
    snprintf(outdir, sizeof outdir, "%s", out);
    char path[640];
    snprintf(path, sizeof path, "%s/samples.bin", outdir);
    fd_samples = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_samples < 0) return;
    void* warm[4];
    backtrace(warm, 4); // carrega libgcc fora do handler
    write_maps();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    long hz = getenv("NXRT_PERF_HZ") ? atol(getenv("NXRT_PERF_HZ")) : 200;
    if (hz < 1) hz = 200;
    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = (suseconds_t)(1000000 / hz);
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);
    pthread_t th;
    pthread_create(&th, NULL, helper, NULL);
    pthread_detach(th);
}

__attribute__((destructor)) static void perf_fini(void) {
    if (fd_samples < 0) return;
    struct itimerval off;
    memset(&off, 0, sizeof off);
    setitimer(ITIMER_PROF, &off, NULL);
    flush_samples();
    write_maps();
}
