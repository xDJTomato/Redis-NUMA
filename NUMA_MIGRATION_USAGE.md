# NUMA Key迁移模块使用指南

## 快速开始

### 1. 编译

```bash
make clean && make
```

Makefile已自动配置：
- 启用NUMA支持 (`-DHAVE_NUMA`)
- 链接libnuma (`-lnuma`)
- 使用libc分配器 (`MALLOC=libc`)

### 2. 启动Redis

```bash
./src/redis-server
```

启动日志应显示：
```
DEBUG: numa_init()完成
NUMA key migration module initialized
```

### 3. 基本命令

#### 查看NUMA节点信息

```bash
# 查看系统NUMA节点
numactl --hardware

# 在Redis中查看节点统计
127.0.0.1:6379> NUMA.NODESTATS
```

#### 迁移Key

```bash
# 设置一个测试key
127.0.0.1:6379> SET mykey "hello world"
OK

# 将mykey迁移到NUMA节点1
127.0.0.1:6379> NUMA.MIGRATE mykey 1
OK

# 查看key的NUMA信息
127.0.0.1:6379> NUMA.KEYINFO mykey
1) "target_node"
2) (integer) 1
3) "current_node"
4) (integer) 1
5) "access_count"
6) (integer) 0
7) "is_hot"
8) (false)
9) "is_cold"
10) (false)
```

#### 冷热数据标记

```bash
# 标记热数据（迁移到热节点）
127.0.0.1:6379> NUMA.MARKHOT hotkey
OK

# 标记冷数据（迁移到冷节点）
127.0.0.1:6379> NUMA.MARKCOLD coldkey
OK
```

## 架构说明

### 模块组件

```
┌─────────────────────────────────────────────────────────┐
│                   NUMA Key迁移模块                       │
├─────────────────────────────────────────────────────────┤
│  numa_key_migrate.h/c  - 核心迁移逻辑                    │
│  sdsnuma.h/c           - SDS NUMA扩展                   │
│  zmalloc.c/h扩展       - 指定节点分配API                │
└─────────────────────────────────────────────────────────┘
```

### 核心功能

1. **Key级NUMA迁移**
   - 将特定key及其数据迁移到指定NUMA节点
   - 支持String类型（其他类型待扩展）

2. **冷热数据分层**
   - 热数据：高访问频率，放在本地节点（低延迟）
   - 冷数据：低访问频率，放在远程节点（大容量）

3. **访问追踪**
   - 自动记录key访问次数
   - 支持基于阈值的自动分类

## API参考

### Redis命令

#### NUMA.MIGRATE key target_node
迁移指定key到目标NUMA节点。

**参数**：
- `key`: 要迁移的key
- `target_node`: 目标NUMA节点ID（从0开始）

**返回值**：
- `OK`: 迁移成功
- 错误信息: 迁移失败

**示例**：
```bash
127.0.0.1:6379> NUMA.MIGRATE mykey 1
OK
```

#### NUMA.MARKHOT key
标记key为热数据并迁移到热节点。

**参数**：
- `key`: 要标记的key

**返回值**：
- `OK`: 标记成功
- 错误信息: 标记失败

#### NUMA.MARKCOLD key
标记key为冷数据并迁移到冷节点。

**参数**：
- `key`: 要标记的key

**返回值**：
- `OK`: 标记成功
- 错误信息: 标记失败

#### NUMA.KEYINFO key
查看key的NUMA信息。

**参数**：
- `key`: 要查询的key

**返回值**：
- 包含target_node、current_node、access_count、is_hot、is_cold的map

**示例**：
```bash
127.0.0.1:6379> NUMA.KEYINFO mykey
1) "target_node"
2) (integer) 1
3) "current_node"
4) (integer) 1
5) "access_count"
6) (integer) 42
7) "is_hot"
8) (false)
9) "is_cold"
10) (false)
```

#### NUMA.NODESTATS
查看NUMA节点统计信息。

**返回值**：
- 包含迁移次数、字节数、时间等统计信息

### C API

#### 初始化与清理

```c
#include "numa_key_migrate.h"

/* 模块初始化（在server启动时调用） */
int numa_key_migrate_init(void);

/* 模块清理 */
void numa_key_migrate_cleanup(void);
```

#### 基本迁移操作

```c
/* 迁移单个key */
int numa_key_migrate(redisDb *db, sds key, int target_node);

/* 批量迁移 */
int numa_key_migrate_batch(redisDb *db, int source_node, int target_node,
                           int max_keys, uint64_t *migrated_bytes);
```

#### 冷热管理

```c
/* 标记热数据 */
int numa_key_mark_hot(redisDb *db, sds key);

/* 标记冷数据 */
int numa_key_mark_cold(redisDb *db, sds key);

/* 自动分类（基于访问计数） */
int numa_key_auto_classify(redisDb *db, sds key);
```

#### 访问追踪

```c
/* 记录key访问（在命令处理中调用） */
void numa_key_record_access(redisDb *db, sds key);
```

#### 策略配置

```c
/* 迁移策略结构 */
typedef struct numa_migrate_policy {
    int enabled;                  /* 启用自动迁移 */
    int hot_node;                 /* 热数据目标节点 */
    int cold_node;                /* 冷数据目标节点 */
    uint32_t hot_threshold;       /* 热数据访问阈值 */
    uint32_t cold_threshold;      /* 冷数据访问阈值 */
    uint64_t migrate_interval_ms; /* 自动检查间隔 */
} numa_migrate_policy_t;

/* 设置策略 */
void numa_key_set_policy(const numa_migrate_policy_t *policy);

/* 获取策略 */
void numa_key_get_policy(numa_migrate_policy_t *policy);
```

#### 指定节点分配

```c
#include "zmalloc.h"

/* 在指定NUMA节点分配内存 */
void *numa_zmalloc_onnode(size_t size, int node);
void *numa_zcalloc_onnode(size_t size, int node);

/* 设置/获取当前节点 */
void numa_set_current_node(int node);
int numa_get_current_node(void);
```

#### SDS NUMA扩展

```c
#include "sdsnuma.h"

/* 在指定节点创建SDS */
sds sdsnumanew(const char *init, int node);
sds sdsnumaemptylen(size_t initlen, int node);
sds sdsnumadup(const sds s, int node);

/* 在指定节点扩展SDS */
sds sdsnumacatlen(sds s, const void *t, size_t len, int node);
sds sdsnumacat(sds s, const char *t, int node);
```

## 配置示例

### 自动迁移配置

```c
/* 在server.c或配置加载中设置 */
numa_migrate_policy_t policy = {
    .enabled = 1,                    /* 启用自动迁移 */
    .hot_node = 0,                   /* 热数据放在节点0 */
    .cold_node = 1,                  /* 冷数据放在节点1 */
    .hot_threshold = 1000,           /* 访问1000次以上为热数据 */
    .cold_threshold = 100,           /* 访问100次以下为冷数据 */
    .migrate_interval_ms = 60000     /* 每分钟检查一次 */
};
numa_key_set_policy(&policy);
```

### 定期任务

```c
/* 在serverCron中添加 */
void serverCron(void) {
    /* 其他定时任务... */
    
    /* NUMA迁移定时任务 */
    numa_key_migrate_cron();
}
```

## 性能考虑

### 迁移开销

- **内存分配**：触发`numa_alloc_onnode`系统调用
- **数据复制**：`memcpy`数据到新内存
- **元数据更新**：更新key的NUMA元数据

### 优化建议

1. **批量迁移**：使用`numa_key_migrate_batch`减少开销
2. **异步迁移**：考虑后台线程执行迁移
3. **预测迁移**：基于访问模式预测并预迁移

## 限制与已知问题

### 当前限制

1. **数据类型支持**：当前主要支持String类型（RAW编码）
2. **迁移粒度**：整个key迁移，不支持field级别
3. **原子性**：迁移过程非原子，可能短暂不一致

### 未来改进

- [ ] 支持List、Hash、Set、ZSet等类型
- [ ] 异步迁移（后台线程）
- [ ] 迁移进度查询
- [ ] 零拷贝迁移优化

## 调试与故障排除

### 查看NUMA拓扑

```bash
# 系统NUMA信息
numactl --hardware

# 进程NUMA内存分布
numastat -p $(pgrep redis-server)
```

### Redis日志

启动Redis时观察日志：
```
DEBUG: numa_init()完成
NUMA key migration module initialized
```

### 常见问题

**Q: 迁移命令返回错误**
- 检查目标节点ID是否有效（`0 <= node < numa_max_node()`）
- 检查key是否存在

**Q: 编译错误**
- 确保安装了libnuma-dev：`sudo apt-get install libnuma-dev`
- 检查Makefile中`-lnuma`是否正确链接

**Q: 性能没有提升**
- 确认key确实在指定节点分配（使用`NUMA.KEYINFO`）
- 检查系统NUMA拓扑是否正确配置

## 示例代码

### 完整使用示例

```c
#include "server.h"
#include "numa_key_migrate.h"

void example_usage(void) {
    redisDb *db = &server.db[0];
    sds key = sdsnew("mykey");
    
    /* 1. 初始化模块 */
    numa_key_migrate_init();
    
    /* 2. 设置策略 */
    numa_migrate_policy_t policy = {
        .enabled = 1,
        .hot_node = 0,
        .cold_node = 1,
        .hot_threshold = 1000,
        .cold_threshold = 100,
        .migrate_interval_ms = 60000
    };
    numa_key_set_policy(&policy);
    
    /* 3. 手动迁移key */
    if (numa_key_migrate(db, key, 1) == 0) {
        serverLog(LL_NOTICE, "Migration successful");
    }
    
    /* 4. 标记热数据 */
    numa_key_mark_hot(db, key);
    
    /* 5. 记录访问 */
    numa_key_record_access(db, key);
    
    /* 6. 查看统计 */
    numa_migrate_stats_t stats;
    numa_key_get_stats(&stats);
    serverLog(LL_NOTICE, "Total migrations: %llu", stats.total_migrations);
    
    sdsfree(key);
}
```

---

**版本**: 1.0  
**最后更新**: 2025年2月
