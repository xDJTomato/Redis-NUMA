/* =============================================================================
 * nf_track.h - lightweight cache behavior tracking framework (pure C11).
 *
 * Maintains TinyLFU-style estimators (Count-Min Sketch + Doorkeeper Bloom
 * filter) for frequency discovery, plus a sliding-window feedback loop that
 * produces a single effectiveness score an adaptive strategy can optimize.
 * Fixed memory, O(1) per access.  No external dependencies.
 * ========================================================================== */
#ifndef NF_TRACK_H
#define NF_TRACK_H

#include "nf_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NF_TRACK_CMS_DEPTH 4
#define NF_TRACK_CMS_WIDTH_DEFAULT 16384
#define NF_TRACK_RESET_DEFAULT 50000

typedef struct nf_tracker {
    /* TinyLFU estimators */
    uint8_t  *cms_rows[NF_TRACK_CMS_DEPTH]; /* 4-bit packed counters */
    uint32_t  cms_width;
    uint32_t  cms_mask;
    uint8_t  *dk_bits;         /* doorkeeper bloom filter */
    uint32_t  dk_num_bits;
    uint64_t  total_ops;
    uint32_t  reset_interval;

    /* sliding-window feedback */
    uint64_t  window_accesses;
    uint64_t  window_local_hits;
    uint64_t  window_remote_hits;
    double    window_cost_ns;
    double    ewma_hit_ratio;
    double    ewma_cost;
    double    feedback_score;
    double    alpha;

    /* per-node access counters (visualization) */
    uint64_t  node_accesses[NF_MAX_NODES];
    int       node_count;
} nf_tracker_t;

void     nf_tracker_init(nf_tracker_t *t, int node_count, uint32_t cms_width, uint32_t reset_interval);
void     nf_tracker_free(nf_tracker_t *t);

/* Count-Min Sketch frequency estimate for a key (0..15). */
uint32_t nf_tracker_freq(const nf_tracker_t *t, const char *key);
/* Doorkeeper membership (1 = seen before). */
int      nf_tracker_doorkeeper(const nf_tracker_t *t, const char *key);
/* Observe one access: doorkeeper admit + CMS increment. Returns 1 if the key
 * passed the doorkeeper (i.e. is a repeat access worth counting). */
int      nf_tracker_observe(nf_tracker_t *t, const char *key);
/* Global decay: halve all counters, clear doorkeeper, advance epoch. */
void     nf_tracker_decay(nf_tracker_t *t);

/* Record one modeled access outcome into the sliding window. */
void     nf_tracker_record_access(nf_tracker_t *t, int node, int is_local, double cost_ns);
/* Fold the current window into EWMA metrics and refresh the feedback score. */
void     nf_tracker_update_feedback(nf_tracker_t *t);
double   nf_tracker_get_score(const nf_tracker_t *t);

#ifdef __cplusplus
}
#endif

#endif /* NF_TRACK_H */
