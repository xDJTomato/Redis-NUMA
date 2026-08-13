/* =============================================================================
 * redis8_compat.h - Redis 6.2 -> 8 compatibility shims for the NUMA modules.
 *
 * Redis 7.0+ removed the atomicvar.h wrapper macros (redisAtomic / atomicGet /
 * atomicSet / atomicIncr) in favour of direct C11 <stdatomic.h>, removed the
 * server.lruclock field in favour of getLRUClock(), and deprecated
 * dictGetIterator in favour of dictGetSafeIterator.  This header keeps the
 * NUMA modules compiling against both 6.2 and 8 with zero source changes.
 *
 * It is a *bridge*: the recommended end state is to migrate the modules to the
 * native Redis 8 API (see docs/redis8-migration.md), but this header lets the
 * tree build immediately after a version bump while the manual migration lands.
 * ========================================================================== */
#ifndef REDIS8_COMPAT_H
#define REDIS8_COMPAT_H

#include "version.h"
#include <stdatomic.h>

#if (REDIS_VERSION_NUM >= 0x00070000)

/* ---- atomic wrapper shims (Redis 6.x API on top of C11 <stdatomic.h>) ---- */
#define redisAtomic(t)        _Atomic t
#define atomicGet(v, d)       do { (d) = atomic_load_explicit(&(v), memory_order_relaxed); } while (0)
#define atomicSet(v, x)       atomic_store_explicit(&(v), (x), memory_order_relaxed)
#define atomicIncr(v, i)      atomic_fetch_add_explicit(&(v), (i), memory_order_relaxed)
#define atomicDecr(v, i)      atomic_fetch_sub_explicit(&(v), (i), memory_order_relaxed)
#define atomicGetIncr(v, d, i) do { (d) = atomic_fetch_add_explicit(&(v), (i), memory_order_relaxed); } while (0)

/* ---- LRU clock ---- */
/* Composite LRU stores the low 16 bits of the LRU clock. */
#ifndef NUMA_LRU_CLOCK
#define NUMA_LRU_CLOCK()      getLRUClock()
#endif

/* ---- dict iterator ---- */
#ifndef dictGetIterator
#define dictGetIterator       dictGetSafeIterator
#endif

/* ---- serverLog alias ---- */
#ifndef _serverLog
#define _serverLog             serverLog
#endif

#else /* Redis 6.x */

/* 6.x already provides redisAtomic / atomicGet / ... via atomicvar.h, and the
 * server.lruclock field + LRU_CLOCK() macro.  No shims needed; only alias the
 * LRU clock helper so call sites can be written once. */
#ifndef NUMA_LRU_CLOCK
#define NUMA_LRU_CLOCK()      LRU_CLOCK()
#endif

#endif /* REDIS_VERSION_NUM */

#endif /* REDIS8_COMPAT_H */
