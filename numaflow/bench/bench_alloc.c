/* bench_alloc.c - synthetic benchmark: nf_alloc vs system malloc.
 * Measures single/multi-thread throughput, internal fragmentation and memory
 * overhead under a Redis-like object-size distribution. */
#include "nf_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
static double now_ms(void) { return (double)GetTickCount(); }
#else
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
#endif

static size_t rand_size(nf_rng_t *r) {
    double d = nf_rng_double(r);
    if (d < 0.55) return 16 + nf_rng_int(r, 96);
    if (d < 0.85) return 128 + nf_rng_int(r, 896);
    if (d < 0.97) return 1024 + nf_rng_int(r, 15360);
    return 16384 + nf_rng_int(r, 65536);
}

#define N 200000
#define ROUNDS 3
static size_t g_sizes[N];
static void **g_ptrs;

static double run_nf(nf_alloc_t *a, int rounds) {
    double t0 = now_ms();
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < N; i++) g_ptrs[i] = nf_alloc_malloc(a, g_sizes[i]);
        for (int i = 0; i < N; i++) nf_alloc_free(a, g_ptrs[i]);
    }
    return now_ms() - t0;
}
static double run_libc(int rounds) {
    double t0 = now_ms();
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < N; i++) g_ptrs[i] = malloc(g_sizes[i]);
        for (int i = 0; i < N; i++) free(g_ptrs[i]);
    }
    return now_ms() - t0;
}

/* multi-thread: T threads each do per-thread alloc/free loops */
typedef struct { nf_alloc_t *a; int use_nf; int per_thread; double *elapsed; } mthr_arg_t;
static void *mthr_fn(void *ud) {
    mthr_arg_t *m = (mthr_arg_t *)ud;
    void **p = (void **)malloc((size_t)m->per_thread * sizeof(void *));
    nf_rng_t r = nf_rng_seed(7);
    double t0 = now_ms();
    for (int rep = 0; rep < 3; rep++) {
        for (int i = 0; i < m->per_thread; i++) {
            size_t s = rand_size(&r);
            p[i] = m->use_nf ? nf_alloc_malloc(m->a, s) : malloc(s);
        }
        for (int i = 0; i < m->per_thread; i++) { if (m->use_nf) nf_alloc_free(m->a, p[i]); else free(p[i]); }
    }
    *m->elapsed = now_ms() - t0;
    free(p);
    return NULL;
}

int main(void) {
    nf_rng_t rng = nf_rng_seed(42);
    for (int i = 0; i < N; i++) g_sizes[i] = rand_size(&rng);
    g_ptrs = (void **)malloc(N * sizeof(void *));

    nf_alloc_t *a = nf_alloc_create(NULL);
    printf("== nf_alloc vs malloc  (%d allocs/round)\n\n", N);

    run_nf(a, 1); run_libc(1); /* warmup */
    double t_nf = run_nf(a, ROUNDS);
    double t_lc = run_libc(ROUNDS);
    printf("single-thread throughput (alloc+free ops/s):\n");
    printf("  nf_alloc : %.1f M ops/s\n", (double)N * ROUNDS * 2 / (t_nf / 1000.0) / 1e6);
    printf("  malloc   : %.1f M ops/s\n", (double)N * ROUNDS * 2 / (t_lc / 1000.0) / 1e6);
    printf("  speedup  : %.2fx\n\n", t_lc / t_nf);

    printf("multi-thread phase...\n"); fflush(stdout);
    for (int T = 2; T <= 8; T *= 2) {
        printf("  T=%d\n", T); fflush(stdout);
        pthread_t th[8]; mthr_arg_t arg[8]; double el[8];
        int per = N / T;
        for (int i = 0; i < T; i++) { arg[i].a = a; arg[i].use_nf = 1; arg[i].per_thread = per; arg[i].elapsed = &el[i]; pthread_create(&th[i], NULL, mthr_fn, &arg[i]); }
        for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
        double tnf = 0; for (int i = 0; i < T; i++) tnf += el[i];
        for (int i = 0; i < T; i++) { arg[i].use_nf = 0; pthread_create(&th[i], NULL, mthr_fn, &arg[i]); }
        for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
        double tlc = 0; for (int i = 0; i < T; i++) tlc += el[i];
        printf("%d threads : nf_alloc %.1fM ops/s  malloc %.1fM ops/s  speedup %.2fx\n",
            T, (double)N * 3 * 2 / (tnf / 1000.0) / 1e6, (double)N * 3 * 2 / (tlc / 1000.0) / 1e6, tlc / tnf);
    }
    printf("\n");

    printf("fragmentation phase...\n"); fflush(stdout);
    nf_alloc_t *fa = nf_alloc_create(NULL);
    for (int i = 0; i < N; i++) { g_ptrs[i] = nf_alloc_malloc(fa, g_sizes[i]); if ((i % 50000) == 0) { printf("  alloc %d/%d\n", i, N); fflush(stdout); } }
    nf_alloc_stats_t st; nf_alloc_get_stats(fa, &st);
    printf("memory (fresh allocator, %d live objects):\n", N);
    printf("  requested  : %.1f MB\n", (double)st.requested_bytes / 1e6);
    printf("  usable     : %.1f MB (internal frag %.2f%%)\n", (double)st.usable_bytes / 1e6, nf_alloc_internal_frag(fa) * 100.0);
    printf("  backend    : %.1f MB (external overhead %.2f%%)\n", (double)st.backend_bytes / 1e6, nf_alloc_external_overhead(fa) * 100.0);
    printf("  peak       : %.1f MB\n", (double)st.peak_backend_bytes / 1e6);
    for (int i = 0; i < N; i++) nf_alloc_free(fa, g_ptrs[i]);
    nf_alloc_destroy(fa);

#ifdef _WIN32
    { size_t req = 0, usb = 0;
      for (int i = 0; i < N; i++) { void *p = malloc(g_sizes[i]); req += g_sizes[i]; usb += _msize(p); free(p); }
      printf("  malloc internal frag: %.2f%% (via _msize)\n", (double)(usb - req) / (double)req * 100.0); }
#endif

    free(g_ptrs); nf_alloc_destroy(a);
    return 0;
}
