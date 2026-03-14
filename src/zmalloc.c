/* zmalloc - 带内存用量统计的malloc封装
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>
#include <sched.h>

/* 提供对原始libc free()的访问。用于释放backtrace_symbols()等返回的结果。
 * 必须在包含zmalloc.h之前定义此函数，因为zmalloc.h可能会在使用jemalloc等
 * 非标准分配器时覆盖free实现。 */
void zlibc_free(void *ptr)
{
    free(ptr);
}

#include <string.h>
#include <pthread.h>
#include "config.h"
#include "zmalloc.h"
#include "atomicvar.h"

#ifdef HAVE_NUMA
#include <numa.h>
#include <sched.h>
#include <unistd.h>
#include "numa_pool.h"

/* NUMA全局上下文 - 保留用于兼容性和未来扩展 */
static struct {
    int numa_available;
    int num_nodes;
    int current_node;
    int allocation_strategy;
    int *node_distance_order;
} numa_ctx = {0};

/* 线程局部存储：当前线程绑定的NUMA节点 */
static __thread int tls_current_node = -1;

/* 初始化NUMA支持：初始化内存池、Slab分配器并按距离排序节点 */
void numa_init(void)
{
    /* 初始化内存池模块 */
    if (numa_pool_init() != 0) {
        numa_ctx.numa_available = 0;
        return;
    }
    
    /* P2 Optimization: Initialize Slab allocator */
    if (numa_slab_init() != 0) {
        numa_ctx.numa_available = 0;
        return;
    }
    
    numa_ctx.numa_available = numa_pool_available();
    if (!numa_ctx.numa_available) {
        return;
    }

    numa_ctx.num_nodes = numa_pool_num_nodes();
    numa_ctx.current_node = numa_pool_get_node();
    tls_current_node = numa_ctx.current_node;
    /* 改为交错分配策略，实现跨节点负载均衡 */
    numa_ctx.allocation_strategy = NUMA_STRATEGY_INTERLEAVE;

    /* 初始化节点距离顺序 */
    numa_ctx.node_distance_order = malloc(numa_ctx.num_nodes * sizeof(int));
    if (!numa_ctx.node_distance_order) {
        numa_ctx.numa_available = 0;
        return;
    }

    for (int i = 0; i < numa_ctx.num_nodes; i++) {
        numa_ctx.node_distance_order[i] = i;
    }

    /* 按距离排序 */
    for (int i = 0; i < numa_ctx.num_nodes - 1; i++) {
        for (int j = 0; j < numa_ctx.num_nodes - i - 1; j++) {
            int dist1 = numa_distance(numa_ctx.current_node, numa_ctx.node_distance_order[j]);
            int dist2 = numa_distance(numa_ctx.current_node, numa_ctx.node_distance_order[j + 1]);
            if (dist1 > dist2) {
                int temp = numa_ctx.node_distance_order[j];
                numa_ctx.node_distance_order[j] = numa_ctx.node_distance_order[j + 1];
                numa_ctx.node_distance_order[j + 1] = temp;
            }
        }
    }
}

/* 清理NUMA资源：释放内存池和节点距离排序数组 */
void numa_cleanup(void)
{
    numa_pool_cleanup();
    
    if (numa_ctx.node_distance_order) {
        free(numa_ctx.node_distance_order);
        numa_ctx.node_distance_order = NULL;
    }
}

/* 设置NUMA分配策略（LOCAL_FIRST=本地优先 / INTERLEAVE=交错分配） */
int numa_set_strategy(int strategy)
{
    if (strategy != NUMA_STRATEGY_LOCAL_FIRST && strategy != NUMA_STRATEGY_INTERLEAVE)
    {
        return -1;
    }
    numa_ctx.allocation_strategy = strategy;
    return 0;
}

/* 获取当前NUMA分配策略 */
int numa_get_strategy(void)
{
    return numa_ctx.allocation_strategy;
}

#endif /* HAVE_NUMA */

/* NUMA分配器必须使用PREFIX_SIZE策略（即使定义了HAVE_MALLOC_SIZE），
 * 因为libNUMA无法查询已分配内存的大小。
 * 同时利用前缀标志字段区分池分配和直接分配。 */
#ifdef HAVE_NUMA
/* NUMA分配器需要PREFIX_SIZE追踪大小并记录分配来源标志 */
typedef struct {
    size_t size;           /* 8字节 - 实际分配内存大小 */
    char from_pool;        /* 1字节 - 分配来源：0=直接分配, 1=Pool, 2=Slab */
    char node_id;          /* 1字节 - 分配所在NUMA节点ID（P2修复：确保归还到正确节点） */
    /* Heat tracking fields (reused from padding) */
    uint8_t hotness;       /* 1字节 - 热度级别（0-7），0=冷，7=热 */
    uint8_t access_count;  /* 1字节 - 访问计数（循环计数器） */
    uint16_t last_access;  /* 2字节 - LRU时钟低16位（上次访问时间） */
    char reserved[2];      /* 2字节 - 保留字段，供未来扩展使用 */
} numa_alloc_prefix_t;

/* 热度追踪常量 */
#define NUMA_HOTNESS_MAX     7
#define NUMA_HOTNESS_MIN     0
#define NUMA_HOTNESS_DEFAULT 1

#define PREFIX_SIZE (sizeof(numa_alloc_prefix_t))
#define ASSERT_NO_SIZE_OVERFLOW(sz) assert((sz) <= SIZE_MAX - PREFIX_SIZE)
#else
/* Standard allocator can use HAVE_MALLOC_SIZE if available */
#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#define ASSERT_NO_SIZE_OVERFLOW(sz)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#define ASSERT_NO_SIZE_OVERFLOW(sz) assert((sz) <= SIZE_MAX - PREFIX_SIZE)
#endif
#endif

/* When using the libc allocator, use a minimum allocation size to match the
 * jemalloc behavior that doesn't return NULL in this case.
 */
#define MALLOC_MIN_SIZE(x) ((x) > 0 ? (x) : sizeof(long))

/* Explicitly override malloc/free etc when using tcmalloc. */
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count, size) tc_calloc(count, size)
#define realloc(ptr, size) tc_realloc(ptr, size)
#define free(ptr) tc_free(ptr)
#elif defined(USE_JEMALLOC)
#define malloc(size) je_malloc(size)
#define calloc(count, size) je_calloc(count, size)
#define realloc(ptr, size) je_realloc(ptr, size)
#define free(ptr) je_free(ptr)
#define mallocx(size, flags) je_mallocx(size, flags)
#define dallocx(ptr, flags) je_dallocx(ptr, flags)
#endif

#define update_zmalloc_stat_alloc(__n) atomicIncr(used_memory, (__n))
#define update_zmalloc_stat_free(__n) atomicDecr(used_memory, (__n))

static redisAtomic size_t used_memory = 0;

static void zmalloc_default_oom(size_t size)
{
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %lu bytes\n",
            (unsigned long)size);
    fflush(stderr);
    abort();
}

static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;

#ifdef HAVE_NUMA
/* 辅助函数：初始化分配内存的PREFIX元数据（大小、来源、节点ID、热度） */
static inline void numa_init_prefix(void *ptr, size_t size, int from_pool, int node_id)
{
    numa_alloc_prefix_t *prefix = (numa_alloc_prefix_t *)ptr;
    prefix->size = size;
    prefix->from_pool = from_pool;
    prefix->node_id = (char)node_id;  /* P2修复：记录分配节点，确保释放时路由到正确节点 */
    /* 初始化热度追踪字段 */
    prefix->hotness = NUMA_HOTNESS_DEFAULT;  /* 设置默认热度 */
    prefix->access_count = 0;
    prefix->last_access = 0;
}

/* 辅助函数：从用户指针反推PREFIX指针 */
static inline numa_alloc_prefix_t *numa_get_prefix(void *user_ptr)
{
    return (numa_alloc_prefix_t *)((char *)user_ptr - PREFIX_SIZE);
}

/* 辅助函数：将raw指针（含PREFIX）转为用户可见指针 */
static inline void *numa_to_user_ptr(void *raw_ptr)
{
    return (char *)raw_ptr + PREFIX_SIZE;
}

/* NUMA感知内存分配（含大小追踪）：优先走Slab（≤128B）或Pool路径 */
static void *numa_alloc_with_size(size_t size)
{
    ASSERT_NO_SIZE_OVERFLOW(size);

    size_t total_size = size + PREFIX_SIZE;
    size_t alloc_size;
    
    /* 快速路径：单节点时跳过轮询，直接使用节点0 */
    int target_node;
    if (numa_ctx.num_nodes == 1) {
        target_node = 0;
    } else {
        static __thread int round_robin_index = 0;
        target_node = round_robin_index % numa_ctx.num_nodes;
        round_robin_index++;
    }
    
    void *raw_ptr = NULL;
    
    /* P2优化：≤128B的小对象走Slab快速路径 */
    if (should_use_slab(size)) {
        raw_ptr = numa_slab_alloc(size, target_node, &alloc_size);
    }
    
    /* 回退：大对象或Slab分配失败时走Pool路径 */
    if (!raw_ptr) {
        raw_ptr = numa_pool_alloc(total_size, target_node, &alloc_size);
    }
    
    if (!raw_ptr)
        return NULL;
    
    /* 根据大小判断是否来自内存池（用于free路由） */
    int from_pool = (total_size <= NUMA_POOL_MAX_ALLOC) ? 1 : 0;

    numa_init_prefix(raw_ptr, size, from_pool, target_node);  /* P2修复：传入node_id写入PREFIX */
    update_zmalloc_stat_alloc(total_size);
    return numa_to_user_ptr(raw_ptr);
}

/* NUMA感知内存释放（含大小追踪）：根据PREFIX路由到Slab或Pool */
static void numa_free_with_size(void *user_ptr)
{
    if (user_ptr == NULL)
        return;

    numa_alloc_prefix_t *prefix = numa_get_prefix(user_ptr);
    size_t total_size = prefix->size + PREFIX_SIZE;
    size_t size = prefix->size;
    int node_id = (int)prefix->node_id;  /* P2修复：从PREFIX读取正确的分配节点ID */

    update_zmalloc_stat_free(total_size);

    void *raw_ptr = (char *)user_ptr - PREFIX_SIZE;
    
    /* P2优化：小对象归还Slab */
    if (should_use_slab(size) && prefix->from_pool) {
        /* P2修复：使用存储的node_id，而非轮询值 */
        numa_slab_free(raw_ptr, total_size, node_id);
    } else {
        /* 大对象归还Pool */
        numa_pool_free(raw_ptr, total_size, prefix->from_pool);
    }
}

/* NUMA感知版zmalloc：分配失败时触发OOM处理器 */
void *numa_zmalloc(size_t size)
{
    void *ptr = numa_alloc_with_size(size);
    if (!ptr && size > 0)
        zmalloc_oom_handler(size);
    return ptr;
}

/* NUMA感知版zcalloc：分配并清零 */
void *numa_zcalloc(size_t size)
{
    ASSERT_NO_SIZE_OVERFLOW(size);

    void *ptr = numa_alloc_with_size(size);
    if (!ptr)
    {
        if (size > 0)
            zmalloc_oom_handler(size);
        return NULL;
    }

    memset(ptr, 0, size);
    return ptr;
}

/* NUMA感知版zrealloc：重新分配内存并保留原有数据 */
void *numa_zrealloc(void *ptr, size_t size)
{
    /* 处理边界情况 */
    if (ptr == NULL)
        return numa_zmalloc(size);
    if (size == 0)
    {
        numa_zfree(ptr);
        return NULL;
    }

    /* 从PREFIX读取旧内存大小 */
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    size_t old_size = prefix->size;

    /* 分配新内存 */
    void *new_ptr = numa_alloc_with_size(size);
    if (!new_ptr)
    {
        zmalloc_oom_handler(size);
        return NULL;
    }

    /* 拷贝数据并释放旧内存 */
    size_t copy_size = (old_size < size) ? old_size : size;
    memcpy(new_ptr, ptr, copy_size);
    numa_free_with_size(ptr);

    return new_ptr;
}

/* NUMA感知版zfree */
void numa_zfree(void *ptr)
{
    numa_free_with_size(ptr);
}

/* 设置当前分配使用的NUMA节点 */
void numa_set_current_node(int node)
{
    if (node >= 0 && node < numa_ctx.num_nodes) {
        numa_ctx.current_node = node;
        numa_pool_set_node(node);
    }
}

/* 获取当前NUMA节点 */
int numa_get_current_node(void)
{
    return numa_pool_get_node();
}

/* 在指定NUMA节点上分配内存（用于Key迁移，绕过Pool/Slab直接分配） */
static void *numa_alloc_on_specific_node(size_t size, int node)
{
    ASSERT_NO_SIZE_OVERFLOW(size);

    size_t total_size = size + PREFIX_SIZE;
    
    /* 指定节点请求始终使用直接分配，确保物理位置精确 */
    void *raw_ptr = numa_alloc_onnode(total_size, node);
    if (!raw_ptr)
        return NULL;

    numa_init_prefix(raw_ptr, size, 0, node);  /* 标记为直接分配并记录节点ID */
    update_zmalloc_stat_alloc(total_size);
    return numa_to_user_ptr(raw_ptr);
}

/* 在指定NUMA节点上分配内存（对外接口） */
void *numa_zmalloc_onnode(size_t size, int node)
{
    if (node < 0 || node >= numa_ctx.num_nodes)
        return NULL;

    void *ptr = numa_alloc_on_specific_node(size, node);
    if (!ptr && size > 0)
        zmalloc_oom_handler(size);
    return ptr;
}

/* 在指定NUMA节点上分配并清零内存 */
void *numa_zcalloc_onnode(size_t size, int node)
{
    ASSERT_NO_SIZE_OVERFLOW(size);

    void *ptr = numa_alloc_on_specific_node(size, node);
    if (!ptr)
    {
        if (size > 0)
            zmalloc_oom_handler(size);
        return NULL;
    }

    memset(ptr, 0, size);
    return ptr;
}

/* ========== NUMA热度追踪API ========== */

/* 从用户指针读取热度级别 */
uint8_t numa_get_hotness(void *ptr)
{
    if (!ptr) return NUMA_HOTNESS_MIN;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    return prefix->hotness;
}

/* 设置用户指针对应内存的热度级别 */
void numa_set_hotness(void *ptr, uint8_t hotness)
{
    if (!ptr) return;
    if (hotness > NUMA_HOTNESS_MAX) hotness = NUMA_HOTNESS_MAX;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    prefix->hotness = hotness;
}

/* 获取访问计数 */
uint8_t numa_get_access_count(void *ptr)
{
    if (!ptr) return 0;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    return prefix->access_count;
}

/* 递增访问计数 */
void numa_increment_access_count(void *ptr)
{
    if (!ptr) return;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    prefix->access_count++;
}

/* 获取上次访问时间（LRU时钟） */
uint16_t numa_get_last_access(void *ptr)
{
    if (!ptr) return 0;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    return prefix->last_access;
}

/* 设置上次访问时间 */
void numa_set_last_access(void *ptr, uint16_t lru_clock)
{
    if (!ptr) return;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    prefix->last_access = lru_clock;
}

/* 获取分配时所在NUMA节点ID */
int numa_get_node_id(void *ptr)
{
    if (!ptr) return -1;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    return (int)prefix->node_id;
}

#endif /* HAVE_NUMA */

/* 尝试分配内存，失败返回NULL。若usable非空，写入实际可用大小。 */
void *ztrymalloc_usable(size_t size, size_t *usable)
{
    ASSERT_NO_SIZE_OVERFLOW(size);

#ifdef HAVE_NUMA
    /* NUMA可用时使用NUMA分配器 */
    if (numa_ctx.numa_available)
    {
        void *ptr = numa_alloc_with_size(size);
        if (!ptr)
            return NULL;
        if (usable)
            *usable = size;
        return ptr;
    }
#endif

    /* 回退到标准分配器 */
    void *ptr = malloc(MALLOC_MIN_SIZE(size) + PREFIX_SIZE);
    if (!ptr)
        return NULL;

#ifdef HAVE_MALLOC_SIZE
    size = zmalloc_size(ptr);
    update_zmalloc_stat_alloc(size);
    if (usable)
        *usable = size;
    return ptr;
#else
    *((size_t *)ptr) = size;
    update_zmalloc_stat_alloc(size + PREFIX_SIZE);
    if (usable)
        *usable = size;
    return (char *)ptr + PREFIX_SIZE;
#endif
}

/* 分配内存，失败时触发OOM处理器（不返回NULL） */
void *zmalloc(size_t size)
{
    void *ptr = ztrymalloc_usable(size, NULL);
    if (!ptr)
        zmalloc_oom_handler(size);
    return ptr;
}

/* 尝试分配内存，失败返回NULL */
void *ztrymalloc(size_t size)
{
    return ztrymalloc_usable(size, NULL);
}

/* 分配内存，失败触发OOM处理器；若usable非空，写入实际可用大小 */
void *zmalloc_usable(size_t size, size_t *usable)
{
    void *ptr = ztrymalloc_usable(size, usable);
    if (!ptr)
        zmalloc_oom_handler(size);
    return ptr;
}

/* 绕过线程缓存直接操作arena的分配/释放函数。
 * 目前仅jemalloc实现，用于在线碎片整理。 */
#ifdef HAVE_DEFRAG
void *zmalloc_no_tcache(size_t size)
{
    ASSERT_NO_SIZE_OVERFLOW(size);
    void *ptr = mallocx(size + PREFIX_SIZE, MALLOCX_TCACHE_NONE);
    if (!ptr)
        zmalloc_oom_handler(size);
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
}

void zfree_no_tcache(void *ptr)
{
    if (ptr == NULL)
        return;
    update_zmalloc_stat_free(zmalloc_size(ptr));
    dallocx(ptr, MALLOCX_TCACHE_NONE);
}
#endif

/* 尝试分配并清零内存，失败返回NULL；若usable非空写入实际可用大小 */
void *ztrycalloc_usable(size_t size, size_t *usable)
{
    ASSERT_NO_SIZE_OVERFLOW(size);

#ifdef HAVE_NUMA
    /* NUMA可用时使用NUMA分配器 */
    if (numa_ctx.numa_available)
    {
        void *ptr = numa_alloc_with_size(size);
        if (!ptr)
            return NULL;
        memset(ptr, 0, size);
        if (usable)
            *usable = size;
        return ptr;
    }
#endif

    void *ptr = calloc(1, MALLOC_MIN_SIZE(size) + PREFIX_SIZE);
    if (ptr == NULL)
        return NULL;

#ifdef HAVE_MALLOC_SIZE
    size = zmalloc_size(ptr);
    update_zmalloc_stat_alloc(size);
    if (usable)
        *usable = size;
    return ptr;
#else
    *((size_t *)ptr) = size;
    update_zmalloc_stat_alloc(size + PREFIX_SIZE);
    if (usable)
        *usable = size;
    return (char *)ptr + PREFIX_SIZE;
#endif
}

/* 分配并清零内存，失败触发OOM处理器 */
void *zcalloc(size_t size)
{
    void *ptr = ztrycalloc_usable(size, NULL);
    if (!ptr)
        zmalloc_oom_handler(size);
    return ptr;
}

/* 尝试分配内存，失败返回NULL */
void *ztrycalloc(size_t size)
{
    return ztrycalloc_usable(size, NULL);
}

/* 分配内存，失败触发OOM处理器；若usable非空，写入实际可用大小 */
void *zcalloc_usable(size_t size, size_t *usable)
{
    void *ptr = ztrycalloc_usable(size, usable);
    if (!ptr)
        zmalloc_oom_handler(size);
    return ptr;
}

/* 重新分配内存，失败触发OOM处理器 */
void *zrealloc(void *ptr, size_t size)
{
    ptr = ztryrealloc_usable(ptr, size, NULL);
    if (!ptr && size != 0)
        zmalloc_oom_handler(size);
    return ptr;
}

/* 重新分配内存，失败触发OOM处理器；若usable非空写入实际可用大小 */
void *zrealloc_usable(void *ptr, size_t size, size_t *usable)
{
    ptr = ztryrealloc_usable(ptr, size, usable);
    if (!ptr && size != 0)
        zmalloc_oom_handler(size);
    return ptr;
}

/* 尝试重新分配内存，失败返回NULL */
void *ztryrealloc(void *ptr, size_t size)
{
    return ztryrealloc_usable(ptr, size, NULL);
}

/* 尝试重新分配内存，失败返回NULL；若usable非空写入实际可用大小 */
void *ztryrealloc_usable(void *ptr, size_t size, size_t *usable)
{
    if (ptr == NULL)
        return ztrymalloc_usable(size, usable);
    if (size == 0)
    {
        zfree(ptr);
        if (usable)
            *usable = 0;
        return NULL;
    }

#ifdef HAVE_NUMA
    /* NUMA可用时使用NUMA realloc */
    if (numa_ctx.numa_available)
    {
        void *result = numa_zrealloc(ptr, size);
        if (result && usable)
            *usable = size;  /* NUMA allocator returns exact requested size */
        return result;
    }
#endif

    /* 回退到标准realloc */
    ASSERT_NO_SIZE_OVERFLOW(size);

#ifdef HAVE_MALLOC_SIZE
    void *realptr = ptr;
    size_t oldsize = zmalloc_size(realptr);
    void *newptr = realloc(realptr, size);
#else
    void *realptr = (char *)ptr - PREFIX_SIZE;
    size_t oldsize = *((size_t *)realptr);
    void *newptr = realloc(realptr, size + PREFIX_SIZE);
#endif

    if (newptr == NULL)
        return NULL;

#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(oldsize);
    size_t newsize = zmalloc_size(newptr);
    update_zmalloc_stat_alloc(newsize);
    if (usable)
        *usable = newsize;
    return newptr;
#else
    *((size_t *)newptr) = size;
    update_zmalloc_stat_free(oldsize + PREFIX_SIZE);
    update_zmalloc_stat_alloc(size + PREFIX_SIZE);
    if (usable)
        *usable = size;
    return (char *)newptr + PREFIX_SIZE;
#endif
}

void zfree(void *ptr)
{
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL)
        return;

#ifdef HAVE_NUMA
    /* NUMA可用时使用NUMA free路径 */
    if (numa_ctx.numa_available)
    {
        numa_zfree(ptr);
        return;
    }
#endif

#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char *)ptr - PREFIX_SIZE;
    oldsize = *((size_t *)realptr);
    update_zmalloc_stat_free(oldsize + PREFIX_SIZE);
    free(realptr);
#endif
}

/* 类似zfree，同时通过usable返回被释放内存的实际大小 */
void zfree_usable(void *ptr, size_t *usable)
{
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL)
        return;
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(*usable = zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char *)ptr - PREFIX_SIZE;
    *usable = oldsize = *((size_t *)realptr);
    update_zmalloc_stat_free(oldsize + PREFIX_SIZE);
    free(realptr);
#endif
}

char *zstrdup(const char *s)
{
    size_t l = strlen(s) + 1;
    char *p = zmalloc(l);

    memcpy(p, s, l);
    return p;
}

size_t zmalloc_used_memory(void)
{
    size_t um;
    atomicGet(used_memory, um);
    return um;
}

void zmalloc_set_oom_handler(void (*oom_handler)(size_t))
{
    zmalloc_oom_handler = oom_handler;
}

/* 以操作系统特定方式获取RSS（常驻内存大小）。
 *
 * 警告：此函数设计上不追求速度，不应在Redis逐出/换出对象的热路径中调用。
 * 快速RSS估算请使用 RedisEstimateRSS()（速度更快但精度较低）。 */

#if defined(HAVE_PROC_STAT)
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

size_t zmalloc_get_rss(void)
{
    int page = sysconf(_SC_PAGESIZE);
    size_t rss;
    char buf[4096];
    char filename[256];
    int fd, count;
    char *p, *x;

    snprintf(filename, 256, "/proc/%ld/stat", (long)getpid());
    if ((fd = open(filename, O_RDONLY)) == -1)
        return 0;
    if (read(fd, buf, 4096) <= 0)
    {
        close(fd);
        return 0;
    }
    close(fd);

    p = buf;
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while (p && count--)
    {
        p = strchr(p, ' ');
        if (p)
            p++;
    }
    if (!p)
        return 0;
    x = strchr(p, ' ');
    if (!x)
        return 0;
    *x = '\0';

    rss = strtoll(p, NULL, 10);
    rss *= page;
    return rss;
}
#elif defined(HAVE_TASKINFO)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/task.h>
#include <mach/mach_init.h>

size_t zmalloc_get_rss(void)
{
    task_t task = MACH_PORT_NULL;
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

    if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS)
        return 0;
    task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

    return t_info.resident_size;
}
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

size_t zmalloc_get_rss(void)
{
    struct kinfo_proc info;
    size_t infolen = sizeof(info);
    int mib[4];
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    if (sysctl(mib, 4, &info, &infolen, NULL, 0) == 0)
#if defined(__FreeBSD__)
        return (size_t)info.ki_rssize * getpagesize();
#else
        return (size_t)info.kp_vm_rssize * getpagesize();
#endif

    return 0L;
}
#elif defined(__NetBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>

size_t zmalloc_get_rss(void)
{
    struct kinfo_proc2 info;
    size_t infolen = sizeof(info);
    int mib[6];
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC2;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();
    mib[4] = sizeof(info);
    mib[5] = 1;
    if (sysctl(mib, __arraycount(mib), &info, &infolen, NULL, 0) == 0)
        return (size_t)info.p_vm_rssize * getpagesize();

    return 0L;
}
#elif defined(HAVE_PSINFO)
#include <unistd.h>
#include <sys/procfs.h>
#include <fcntl.h>

size_t zmalloc_get_rss(void)
{
    struct prpsinfo info;
    char filename[256];
    int fd;

    snprintf(filename, 256, "/proc/%ld/psinfo", (long)getpid());

    if ((fd = open(filename, O_RDONLY)) == -1)
        return 0;
    if (ioctl(fd, PIOCPSINFO, &info) == -1)
    {
        close(fd);
        return 0;
    }

    close(fd);
    return info.pr_rssize;
}
#else
size_t zmalloc_get_rss(void)
{
    /* If we can't get the RSS in an OS-specific way for this system just
     * return the memory usage we estimated in zmalloc()..
     *
     * Fragmentation will appear to be always 1 (no fragmentation)
     * of course... */
    return zmalloc_used_memory();
}
#endif

#if defined(USE_JEMALLOC)

int zmalloc_get_allocator_info(size_t *allocated,
                               size_t *active,
                               size_t *resident)
{
    uint64_t epoch = 1;
    size_t sz;
    *allocated = *resident = *active = 0;
    /* Update the statistics cached by mallctl. */
    sz = sizeof(epoch);
    je_mallctl("epoch", &epoch, &sz, &epoch, sz);
    sz = sizeof(size_t);
    /* Unlike RSS, this does not include RSS from shared libraries and other non
     * heap mappings. */
    je_mallctl("stats.resident", resident, &sz, NULL, 0);
    /* Unlike resident, this doesn't not include the pages jemalloc reserves
     * for re-use (purge will clean that). */
    je_mallctl("stats.active", active, &sz, NULL, 0);
    /* Unlike zmalloc_used_memory, this matches the stats.resident by taking
     * into account all allocations done by this process (not only zmalloc). */
    je_mallctl("stats.allocated", allocated, &sz, NULL, 0);
    return 1;
}

void set_jemalloc_bg_thread(int enable)
{
    /* let jemalloc do purging asynchronously, required when there's no traffic
     * after flushdb */
    char val = !!enable;
    je_mallctl("background_thread", NULL, 0, &val, 1);
}

int jemalloc_purge()
{
    /* return all unused (reserved) pages to the OS */
    char tmp[32];
    unsigned narenas = 0;
    size_t sz = sizeof(unsigned);
    if (!je_mallctl("arenas.narenas", &narenas, &sz, NULL, 0))
    {
        sprintf(tmp, "arena.%d.purge", narenas);
        if (!je_mallctl(tmp, NULL, 0, NULL, 0))
            return 0;
    }
    return -1;
}

#else

int zmalloc_get_allocator_info(size_t *allocated,
                               size_t *active,
                               size_t *resident)
{
    *allocated = *resident = *active = 0;
    return 1;
}

void set_jemalloc_bg_thread(int enable)
{
    ((void)(enable));
}

int jemalloc_purge()
{
    return 0;
}

#endif

#if defined(__APPLE__)
/* For proc_pidinfo() used later in zmalloc_get_smap_bytes_by_field().
 * Note that this file cannot be included in zmalloc.h because it includes
 * a Darwin queue.h file where there is a "LIST_HEAD" macro (!) defined
 * conficting with Redis user code. */
#include <libproc.h>
#endif

/* 从 /proc/self/smaps 读取指定字段的总字节数（原始为KB，自动转换为字节）。
 * 字段名必须带冒号后缀，如smaps中的格式。
 * 若pid为-1则读取当前进程，否则读取指定pid的信息。
 * 示例：zmalloc_get_smap_bytes_by_field("Rss:",-1) */
#if defined(HAVE_PROC_SMAPS)
size_t zmalloc_get_smap_bytes_by_field(char *field, long pid)
{
    char line[1024];
    size_t bytes = 0;
    int flen = strlen(field);
    FILE *fp;

    if (pid == -1)
    {
        fp = fopen("/proc/self/smaps", "r");
    }
    else
    {
        char filename[128];
        snprintf(filename, sizeof(filename), "/proc/%ld/smaps", pid);
        fp = fopen(filename, "r");
    }

    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        if (strncmp(line, field, flen) == 0)
        {
            char *p = strchr(line, 'k');
            if (p)
            {
                *p = '\0';
                bytes += strtol(line + flen, NULL, 10) * 1024;
            }
        }
    }
    fclose(fp);
    return bytes;
}
#else
/* Get sum of the specified field from libproc api call.
 * As there are per page value basis we need to convert
 * them accordingly.
 *
 * Note that AnonHugePages is a no-op as THP feature
 * is not supported in this platform
 */
size_t zmalloc_get_smap_bytes_by_field(char *field, long pid)
{
#if defined(__APPLE__)
    struct proc_regioninfo pri;
    if (pid == -1)
        pid = getpid();
    if (proc_pidinfo(pid, PROC_PIDREGIONINFO, 0, &pri,
                     PROC_PIDREGIONINFO_SIZE) == PROC_PIDREGIONINFO_SIZE)
    {
        int pagesize = getpagesize();
        if (!strcmp(field, "Private_Dirty:"))
        {
            return (size_t)pri.pri_pages_dirtied * pagesize;
        }
        else if (!strcmp(field, "Rss:"))
        {
            return (size_t)pri.pri_pages_resident * pagesize;
        }
        else if (!strcmp(field, "AnonHugePages:"))
        {
            return 0;
        }
    }
    return 0;
#endif
    ((void)field);
    ((void)pid);
    return 0;
}
#endif

/* 获取所有标记为Private Dirty的页面总字节数。
 *
 * 注意：根据平台和进程内存占用情况，此调用可能很慢，耗时超过1000ms！ */
size_t zmalloc_get_private_dirty(long pid)
{
    return zmalloc_get_smap_bytes_by_field("Private_Dirty:", pid);
}

/* 获取物理内存（RAM）大小（字节）。
 * 跨平台实现，参考：
 * http://nadeausoftware.com/articles/2012/09/c_c_tip_how_get_physical_memory_size_system
 *
 * 版权说明：
 * 1) 以 CC Attribution 协议发布（http://creativecommons.org/licenses/by/3.0/deed.en_US）
 * 2) 原作者：David Robert Nadeau
 * 3) Redis版本由 Matt Stancliff 修改
 * 4) 本注释保留以遵守原始协议要求
 */
size_t zmalloc_get_memory_size(void)
{
#if defined(__unix__) || defined(__unix) || defined(unix) || \
    (defined(__APPLE__) && defined(__MACH__))
#if defined(CTL_HW) && (defined(HW_MEMSIZE) || defined(HW_PHYSMEM64))
    int mib[2];
    mib[0] = CTL_HW;
#if defined(HW_MEMSIZE)
    mib[1] = HW_MEMSIZE; /* OSX. --------------------- */
#elif defined(HW_PHYSMEM64)
    mib[1] = HW_PHYSMEM64; /* NetBSD, OpenBSD. --------- */
#endif
    int64_t size = 0; /* 64-bit */
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, NULL, 0) == 0)
        return (size_t)size;
    return 0L; /* Failed? */

#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    /* FreeBSD, Linux, OpenBSD, and Solaris. -------------------- */
    return (size_t)sysconf(_SC_PHYS_PAGES) * (size_t)sysconf(_SC_PAGESIZE);

#elif defined(CTL_HW) && (defined(HW_PHYSMEM) || defined(HW_REALMEM))
    /* DragonFly BSD, FreeBSD, NetBSD, OpenBSD, and OSX. -------- */
    int mib[2];
    mib[0] = CTL_HW;
#if defined(HW_REALMEM)
    mib[1] = HW_REALMEM; /* FreeBSD. ----------------- */
#elif defined(HW_PHYSMEM)
    mib[1] = HW_PHYSMEM; /* Others. ------------------ */
#endif
    unsigned int size = 0; /* 32-bit */
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, NULL, 0) == 0)
        return (size_t)size;
    return 0L; /* Failed? */
#else
    return 0L; /* Unknown method to get the data. */
#endif
#else
    return 0L; /* Unknown OS. */
#endif
}

/* NUMA分配器需要自行实现zmalloc_size（通过PREFIX读取大小） */
#ifdef HAVE_NUMA
size_t zmalloc_size(void *ptr)
{
    if (ptr == NULL)
        return 0;

    void *orig_ptr = (char *)ptr - PREFIX_SIZE;
    numa_alloc_prefix_t *prefix = (numa_alloc_prefix_t *)orig_ptr;
    return prefix->size;
}

size_t zmalloc_usable_size(void *ptr)
{
    return zmalloc_size(ptr);
}
#elif !defined(HAVE_MALLOC_SIZE)
size_t zmalloc_size(void *ptr)
{
    if (ptr == NULL)
        return 0;

    void *realptr = (char *)ptr - PREFIX_SIZE;
    size_t size = *((size_t *)realptr);
    return size;
}

size_t zmalloc_usable_size(void *ptr)
{
    return zmalloc_size(ptr);
}
#endif

#ifdef REDIS_TEST
#define UNUSED(x) ((void)(x))
int zmalloc_test(int argc, char **argv, int accurate)
{
    void *ptr;

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    printf("Malloc prefix size: %d\n", (int)PREFIX_SIZE);
    printf("Initial used memory: %lu\n", (unsigned long)zmalloc_used_memory());
    ptr = zmalloc(123);
    printf("Allocated 123 bytes; used: %lu\n", (unsigned long)zmalloc_used_memory());
    ptr = zrealloc(ptr, 456);
    printf("Reallocated to 456 bytes; used: %lu\n", (unsigned long)zmalloc_used_memory());
    zfree(ptr);
    printf("Freed pointer; used: %lu\n", (unsigned long)zmalloc_used_memory());
    return 0;
}
#endif