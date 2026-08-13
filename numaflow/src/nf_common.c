/* nf_common.c - implementation of NUMAflow common utilities. */
#include "nf_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#endif

/* ---- logging ------------------------------------------------------------ */
static void nf_default_logger(int level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

nf_log_fn nf_logger = nf_default_logger;

void nf_log(int level, const char *fmt, ...) {
    if (!nf_logger) return;
    va_list ap;
    va_start(ap, fmt);
    /* re-dispatch through the assigned logger by formatting once */
    char buf[NF_STR_MAX];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    nf_logger(level, "%s", buf);
}

/* ---- monotonic-ish tick (ms) ------------------------------------------- */
uint64_t nf_tick_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* ---- params -------------------------------------------------------------- */
void nf_params_init(nf_params_t *p) {
    p->items = NULL;
    p->count = 0;
    p->cap = 0;
}

void nf_params_free(nf_params_t *p) {
    free(p->items);
    nf_params_init(p);
}

int nf_params_set(nf_params_t *p, const char *key, const char *value) {
    if (!p || !key || !value) return NF_EINVAL;
    for (int i = 0; i < p->count; i++) {
        if (strcmp(p->items[i].key, key) == 0) {
            strncpy(p->items[i].value, value, NF_VALUE_MAX - 1);
            p->items[i].value[NF_VALUE_MAX - 1] = '\0';
            return NF_OK;
        }
    }
    if (p->count == p->cap) {
        int ncap = p->cap ? p->cap * 2 : 8;
        nf_kv_t *tmp = (nf_kv_t *)realloc(p->items, (size_t)ncap * sizeof(nf_kv_t));
        if (!tmp) return NF_ENOMEM;
        p->items = tmp;
        p->cap = ncap;
    }
    nf_kv_t *kv = &p->items[p->count];
    memset(kv, 0, sizeof(*kv));
    strncpy(kv->key, key, NF_PARAM_MAX - 1);
    strncpy(kv->value, value, NF_VALUE_MAX - 1);
    p->count++;
    return NF_OK;
}

const char *nf_params_get(const nf_params_t *p, const char *key) {
    if (!p || !key) return NULL;
    for (int i = 0; i < p->count; i++) {
        if (strcmp(p->items[i].key, key) == 0) return p->items[i].value;
    }
    return NULL;
}

int nf_params_get_int(const nf_params_t *p, const char *key, int dflt) {
    const char *v = nf_params_get(p, key);
    return v ? atoi(v) : dflt;
}

double nf_params_get_double(const nf_params_t *p, const char *key, double dflt) {
    const char *v = nf_params_get(p, key);
    return v ? atof(v) : dflt;
}

int nf_params_parse_assign(nf_params_t *p, const char *assign) {
    if (!p || !assign) return NF_EINVAL;
    const char *eq = strchr(assign, '=');
    if (!eq) return NF_EINVAL;
    char key[NF_PARAM_MAX];
    size_t klen = (size_t)(eq - assign);
    if (klen >= NF_PARAM_MAX) klen = NF_PARAM_MAX - 1;
    memcpy(key, assign, klen);
    key[klen] = '\0';
    /* trim trailing spaces of key */
    while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) key[--klen] = '\0';
    const char *val = eq + 1;
    while (*val == ' ' || *val == '\t') val++;
    return nf_params_set(p, key, val);
}

/* ---- items array ---------------------------------------------------------- */
void nf_items_init(nf_items_t *a) {
    a->items = NULL;
    a->count = 0;
    a->cap = 0;
}

void nf_items_free(nf_items_t *a) {
    free(a->items);
    nf_items_init(a);
}

int nf_items_reserve(nf_items_t *a, size_t n) {
    if (n <= a->cap) return NF_OK;
    size_t ncap = a->cap ? a->cap : 16;
    while (ncap < n) {
        if (ncap > NF_MAX_ITEMS / 2) { ncap = n; break; }
        ncap *= 2;
    }
    nf_item_t *tmp = (nf_item_t *)realloc(a->items, ncap * sizeof(nf_item_t));
    if (!tmp) return NF_ENOMEM;
    a->items = tmp;
    a->cap = ncap;
    return NF_OK;
}

int nf_items_push(nf_items_t *a, const nf_item_t *it) {
    if (nf_items_reserve(a, a->count + 1) != NF_OK) return NF_ENOMEM;
    a->items[a->count++] = *it;
    return NF_OK;
}

int nf_items_copy(nf_items_t *dst, const nf_items_t *src) {
    nf_items_clear(dst);
    return nf_items_append(dst, src);
}

int nf_items_append(nf_items_t *dst, const nf_items_t *src) {
    if (nf_items_reserve(dst, dst->count + src->count) != NF_OK) return NF_ENOMEM;
    if (src->count) memcpy(dst->items + dst->count, src->items, src->count * sizeof(nf_item_t));
    dst->count += src->count;
    return NF_OK;
}

void nf_items_clear(nf_items_t *a) {
    a->count = 0;
}

void nf_items_filter_keep(nf_items_t *a) {
    size_t w = 0;
    for (size_t i = 0; i < a->count; i++) {
        if (a->items[i].keep) a->items[w++] = a->items[i];
    }
    a->count = w;
}

/* merge sort (stable) over items */
static void nf_merge(nf_item_t *a, nf_item_t *tmp, size_t lo, size_t mid, size_t hi, nf_item_cmp_fn cmp) {
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) tmp[k++] = cmp(&a[i], &a[j]) <= 0 ? a[i++] : a[j++];
    while (i < mid) tmp[k++] = a[i++];
    while (j < hi) tmp[k++] = a[j++];
    for (size_t m = lo; m < hi; m++) a[m] = tmp[m];
}

static void nf_msort(nf_item_t *a, nf_item_t *tmp, size_t lo, size_t hi, nf_item_cmp_fn cmp) {
    if (hi - lo < 2) return;
    size_t mid = lo + (hi - lo) / 2;
    nf_msort(a, tmp, lo, mid, cmp);
    nf_msort(a, tmp, mid, hi, cmp);
    nf_merge(a, tmp, lo, mid, hi, cmp);
}

void nf_items_sort(nf_items_t *a, nf_item_cmp_fn cmp) {
    if (a->count < 2) return;
    nf_item_t *tmp = (nf_item_t *)malloc(a->count * sizeof(nf_item_t));
    if (!tmp) return;
    nf_msort(a->items, tmp, 0, a->count, cmp);
    free(tmp);
}
