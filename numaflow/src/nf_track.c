/* nf_track.c - lightweight cache behavior tracking framework (pure C11). */
#include "nf_track.h"

#include <stdlib.h>
#include <string.h>

/* ---- hashing (FNV-1a + splitmix finalizer) ------------------------------ */
static uint64_t nf_hash64(const char *key, uint64_t seed) {
    uint64_t h = seed;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h ^= (uint64_t)*p;
        h *= UINT64_C(0x100000001b3);
    }
    h ^= h >> 30; h *= UINT64_C(0xbf58476d1ce4e5b9);
    h ^= h >> 27; h *= UINT64_C(0x94d049bb133111eb);
    h ^= h >> 31;
    return h;
}

/* ---- Count-Min Sketch (4-bit packed counters) --------------------------- */
static uint8_t cms_get(const uint8_t *row, uint32_t idx) {
    uint8_t b = row[idx >> 1];
    return (idx & 1) ? (b >> 4) : (b & 0x0F);
}
static void cms_inc(uint8_t *row, uint32_t idx) {
    uint8_t b = row[idx >> 1];
    uint8_t c = (idx & 1) ? (b >> 4) : (b & 0x0F);
    if (c < 15) c++;
    row[idx >> 1] = (idx & 1) ? ((b & 0x0F) | (c << 4)) : ((b & 0xF0) | c);
}
static void cms_halve(uint8_t *row, uint32_t width) {
    for (uint32_t i = 0; i < width / 2; i++) {
        uint8_t b = row[i];
        row[i] = (uint8_t)(((b & 0x0F) >> 1) | (((b >> 4) & 0x0F) << 3));
    }
}

/* ---- doorkeeper bloom --------------------------------------------------- */
static void dk_set(uint8_t *bits, uint32_t idx) { bits[idx >> 3] |= (uint8_t)(1u << (idx & 7)); }
static int  dk_get(const uint8_t *bits, uint32_t idx) { return (bits[idx >> 3] >> (idx & 7)) & 1; }

void nf_tracker_init(nf_tracker_t *t, int node_count, uint32_t cms_width, uint32_t reset_interval) {
    memset(t, 0, sizeof(*t));
    if (cms_width == 0) cms_width = NF_TRACK_CMS_WIDTH_DEFAULT;
    if (reset_interval == 0) reset_interval = NF_TRACK_RESET_DEFAULT;
    /* round width up to a power of two */
    uint32_t w = 1;
    while (w < cms_width && w < (1u << 24)) w <<= 1;
    t->cms_width = w;
    t->cms_mask = w - 1;
    for (int r = 0; r < NF_TRACK_CMS_DEPTH; r++) {
        t->cms_rows[r] = (uint8_t *)calloc((size_t)w / 2, 1);
    }
    t->dk_num_bits = w * NF_TRACK_CMS_DEPTH;
    t->dk_bits = (uint8_t *)calloc((size_t)(t->dk_num_bits + 7) / 8, 1);
    t->reset_interval = reset_interval;
    t->node_count = node_count > 0 ? node_count : 1;
    t->alpha = 0.25;
}

void nf_tracker_free(nf_tracker_t *t) {
    for (int r = 0; r < NF_TRACK_CMS_DEPTH; r++) {
        free(t->cms_rows[r]);
        t->cms_rows[r] = NULL;
    }
    free(t->dk_bits);
    t->dk_bits = NULL;
    t->cms_width = 0;
}

uint32_t nf_tracker_freq(const nf_tracker_t *t, const char *key) {
    if (!t || !key || !t->cms_rows[0]) return 0;
    uint32_t mn = 255;
    for (int r = 0; r < NF_TRACK_CMS_DEPTH; r++) {
        uint64_t h = nf_hash64(key, UINT64_C(0x9e3779b97f4a7c15) + (uint64_t)r * UINT64_C(0x27d4eb2f165667c5));
        uint32_t idx = (uint32_t)(h & t->cms_mask);
        uint8_t c = cms_get(t->cms_rows[r], idx);
        if (c < mn) mn = c;
    }
    return mn;
}

int nf_tracker_doorkeeper(const nf_tracker_t *t, const char *key) {
    if (!t || !key || !t->dk_bits) return 0;
    for (int i = 0; i < 2; i++) {
        uint64_t h = nf_hash64(key, UINT64_C(0xcbf29ce484222325) + (uint64_t)i * UINT64_C(0x100000001b3));
        uint32_t idx = (uint32_t)(h % t->dk_num_bits);
        if (!dk_get(t->dk_bits, idx)) return 0;
    }
    return 1;
}

int nf_tracker_observe(nf_tracker_t *t, const char *key) {
    if (!t || !key) return 0;
    if (t->total_ops && t->reset_interval && (t->total_ops % t->reset_interval) == 0)
        nf_tracker_decay(t);
    t->total_ops++;
    if (nf_tracker_doorkeeper(t, key)) {
        for (int r = 0; r < NF_TRACK_CMS_DEPTH; r++) {
            uint64_t h = nf_hash64(key, UINT64_C(0x9e3779b97f4a7c15) + (uint64_t)r * UINT64_C(0x27d4eb2f165667c5));
            uint32_t idx = (uint32_t)(h & t->cms_mask);
            cms_inc(t->cms_rows[r], idx);
        }
        return 1;
    } else {
        for (int i = 0; i < 2; i++) {
            uint64_t h = nf_hash64(key, UINT64_C(0xcbf29ce484222325) + (uint64_t)i * UINT64_C(0x100000001b3));
            uint32_t idx = (uint32_t)(h % t->dk_num_bits);
            dk_set(t->dk_bits, idx);
        }
        return 0;
    }
}

void nf_tracker_decay(nf_tracker_t *t) {
    if (!t) return;
    for (int r = 0; r < NF_TRACK_CMS_DEPTH; r++)
        if (t->cms_rows[r]) cms_halve(t->cms_rows[r], t->cms_width);
    if (t->dk_bits) memset(t->dk_bits, 0, (size_t)(t->dk_num_bits + 7) / 8);
}

void nf_tracker_record_access(nf_tracker_t *t, int node, int is_local, double cost_ns) {
    if (!t) return;
    t->window_accesses++;
    if (is_local) t->window_local_hits++;
    else t->window_remote_hits++;
    t->window_cost_ns += cost_ns;
    if (node >= 0 && node < t->node_count) t->node_accesses[node]++;
}

void nf_tracker_update_feedback(nf_tracker_t *t) {
    if (!t || t->window_accesses == 0) return;
    double hit = (double)t->window_local_hits / (double)t->window_accesses;
    double avg_cost = t->window_cost_ns / (double)t->window_accesses;
    if (t->ewma_hit_ratio == 0.0 && t->ewma_cost == 0.0) {
        t->ewma_hit_ratio = hit;
        t->ewma_cost = avg_cost;
    } else {
        t->ewma_hit_ratio = t->alpha * hit + (1.0 - t->alpha) * t->ewma_hit_ratio;
        t->ewma_cost = t->alpha * avg_cost + (1.0 - t->alpha) * t->ewma_cost;
    }
    /* feedback score: higher is better.  Blend hit ratio (0..1) with an
     * inverse-cost term normalized by the local DRAM latency baseline. */
    double baseline = 100.0; /* ~1 local DRAM access in ns */
    double cost_term = baseline / (t->ewma_cost > 0 ? t->ewma_cost : baseline);
    t->feedback_score = 0.5 * t->ewma_hit_ratio + 0.5 * cost_term;
    /* reset the window */
    t->window_accesses = 0;
    t->window_local_hits = 0;
    t->window_remote_hits = 0;
    t->window_cost_ns = 0.0;
}

double nf_tracker_get_score(const nf_tracker_t *t) {
    return t ? t->feedback_score : 0.0;
}
