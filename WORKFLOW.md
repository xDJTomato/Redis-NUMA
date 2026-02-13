# Redis NUMA 项目开发工作流规范

> **版本**: v1.0  
> **更新时间**: 2026-02-13  
> **适用对象**: AI开发助手自我约束规范

---

## 📋 目录

- [核心开发流程](#核心开发流程)
- [关键原则](#关键原则)
- [开发Checklist](#开发checklist)
- [常见问题处理](#常见问题处理)
- [项目知识库](#项目知识库)

---

## 🔄 核心开发流程

### 阶段1: 需求理解与文档检查 🔍

#### 1.1 需求确认
- [ ] 明确用户要实现的具体功能
- [ ] 检查是否与现有模块冲突
- [ ] 确认依赖关系和前置条件
- [ ] **如有任何疑问，立即询问用户**

#### 1.2 文档审查
```bash
# 优先查看的文档顺序
1. NUMA_DEVELOPMENT_LOG.md          # 了解最新进度
2. docs/modules/*.md                # 查看设计文档
3. README.md                        # 确认项目状态
4. 使用 search_codebase 查找相关代码
```

**关键检查点**:
- ✅ 待实现功能是否在文档中有设计
- ✅ 相关模块的实现状态（✅已实现/⚠️规划中/❌未实现）
- ✅ 依赖的其他模块是否就绪

#### 1.3 设计决策点识别

**🚨 重要规则**: 遇到以下情况**必须停下来询问用户**：

- ❓ 插槽编号分配（0号还是1号？）
- ❓ 执行方式（serverCron自动 or 主动调用 or 混合？）
- ❓ 数据结构设计有多种可选方案
- ❓ 接口命名和参数定义不确定
- ❓ 性能权衡需要用户决策

**询问模板**:
```
我在实现[功能名]时遇到一个设计决策点：

[问题描述]

可选方案：
A. [方案A] - 优点: ... 缺点: ...
B. [方案B] - 优点: ... 缺点: ...
C. [方案C] - 优点: ... 缺点: ...

建议采用方案[X]，因为[理由]。请确认或选择其他方案。
```

---

### 阶段2: 代码实现 💻

#### 2.1 文件操作规范

**🚨 关键规则**:

```
⚠️ 务必先 read_file 获取最新内容，再使用 search_replace
⚠️ 使用 search_replace 修改现有文件
⚠️ 仅在必要时创建新文件
⚠️ 避免主动创建 .md 文档（除非用户明确要求）
```

**正确的修改流程**:
```python
# 1. 先读取
read_file("src/server.c")

# 2. 再修改
search_replace(
    file_path="src/server.c",
    replacements=[{
        "original_text": "...",  # 必须完全匹配
        "new_text": "..."
    }]
)
```

#### 2.2 代码编写顺序

```
1️⃣ 创建头文件 (.h)
   └─ 定义接口、数据结构、宏常量

2️⃣ 实现源文件 (.c)
   └─ 实现核心功能

3️⃣ 更新 Makefile
   └─ 添加编译目标 (numa_xxx.o)

4️⃣ 集成到 server.h
   └─ #include "numa_xxx.h"

5️⃣ 集成到 server.c
   └─ 调用初始化函数

6️⃣ 编译测试
   └─ make clean && make -j4

7️⃣ 修复编译错误

8️⃣ 运行测试
```

#### 2.3 关键注意事项 ⚠️

**Redis内部符号**:
```c
// ❌ 错误
serverLog(LL_NOTICE, "message");

// ✅ 正确
extern void _serverLog(int level, const char *fmt, ...);
_serverLog(LL_NOTICE, "message");
```

**初始化时序**:
```c
int main() {
    // ❌ 错误顺序
    numa_strategy_init();  // 过早调用
    initServer();
    
    // ✅ 正确顺序
    initServer();
    numa_strategy_init();  // 在 initServer() 之后
}
```

**Makefile链接顺序**:
```makefile
# NUMA相关 .o 文件必须放在最后
REDIS_SERVER_OBJ=... server.o ... numa_strategy_slots.o numa_key_migrate.o
```

**模块共存原则**:
```
✅ 保留 numa_migrate.c（基础内存块迁移）
✅ 新增 numa_key_migrate.c（Key级别迁移）
✅ 两者独立运行，各司其职
```

**热度结构不可修改**:
```c
// ❌ 禁止修改 Redis 原生 LRU 结构
typedef struct redisObject {
    unsigned lru:LRU_BITS;  // 不要改这里
} robj;

// ✅ 可以复用 LRU 机制
uint16_t timestamp = LRU_CLOCK() & 0xFFFF;
```

---

### 阶段3: 测试验证 🧪

#### 3.1 编译测试

```bash
# 清理并重新编译
cd src
make clean
make -j4 2>&1 | tee /tmp/build.log

# 检查编译结果
echo $?  # 应该为 0

# 检查警告（可忽略 variadic macro 警告）
grep -i warning /tmp/build.log | grep -v "variadic macro"
```

**常见编译错误处理**:
- `undefined reference to 'serverLog'` → 使用 `_serverLog`
- `comparison of different signedness` → 添加类型转换
- 链接错误 → 调整 Makefile 中 .o 文件顺序

#### 3.2 功能测试

```bash
# 1. 启动Redis（前台模式，观察日志）
./src/redis-server --loglevel verbose

# 观察关键日志：
# ✅ [NUMA Strategy] Strategy slot framework initialized
# ✅ [NUMA Key Migrate] Module initialized successfully
# ✅ [NUMA Strategy Slot 0] No-op strategy executed

# 2. 基础功能测试
./src/redis-cli SET test "Hello NUMA"
./src/redis-cli GET test

# 3. 运行综合测试
./test_numa.sh
```

#### 3.3 性能验证

```bash
# 性能基准测试
./src/redis-benchmark -t set,get -n 100000 -q

# 期望结果：
# SET: 169,000+ req/s
# GET: 188,000+ req/s
# p50: 0.031ms
```

#### 3.4 日志验证

```bash
# 检查模块日志
grep "NUMA Strategy" /tmp/redis_test.log
grep "NUMA Key Migrate" /tmp/redis_test.log

# 确认无错误
grep -i error /tmp/redis_test.log | grep -v "make test"
```

---

### 阶段4: 文档更新 📚

#### 4.1 更新开发日志（NUMA_DEVELOPMENT_LOG.md）

**必须包含的内容**:

```markdown
## 更新记录

**最新版本**: v2.x (2026-XX-XX)
**上次更新**: v2.y (2026-XX-XX)

### v2.x 更新内容 (2026-XX-XX)
- ✅ **[功能名]**: 简短描述
- ✅ **[子功能1]**: 详细说明
- ✅ **[子功能2]**: 详细说明
- ⚠️ **待实现**: 列出待完成功能

---

## v2.x [模块名]实现 (2026-XX-XX)

### 实现目标
[描述本次实现的目标]

### 核心数据结构
[关键数据结构定义]

### 实现步骤
#### 第1步: [步骤名]
[详细描述]

#### 第2步: [步骤名]
[详细描述]

### 编译与测试
[测试结果]

### 已实现功能
- ✅ [功能1]
- ✅ [功能2]

### 待实现功能
- ⚠️ [功能3]
- ⚠️ [功能4]

### 设计亮点
[技术亮点]

### 关键决策
[记录重要的设计决策和理由]

### 小结
[总结本次实现的成果和意义]
```

#### 4.2 更新模块文档（docs/modules/*.md）

**状态标注规范**:
```markdown
## 模块概述

**状态**: ✅ **已实现** / ⚠️ **部分实现** / ❌ **规划中**

**已实现功能**:
- ✅ 功能1
- ✅ 功能2

**待实现功能**:
- ⚠️ 功能3（需要xx）
- ⚠️ 功能4（规划中）
```

#### 4.3 更新README.md（仅必要时）

**更新触发条件**:
- ✅ 新增主要功能模块
- ✅ 性能数据有显著变化
- ✅ 架构设计有重大调整
- ❌ 不要频繁修改

**更新内容**:
- 功能清单（已完成/进行中/计划中）
- 性能数据
- 路线图

---

### 阶段5: Git提交与推送 📤

#### 5.1 提交准备

```bash
# 1. 检查修改
git status

# 2. 添加相关文件
git add NUMA_DEVELOPMENT_LOG.md
git add docs/modules/*.md
git add src/numa_*.h src/numa_*.c
git add src/Makefile src/server.c src/server.h

# 3. 排除不必要的文件
# 不要添加: build/, *.o, *.d, test_migrate, dump.rdb
```

#### 5.2 提交信息格式

```
<type>: <简短描述> (<version>)

✨ 新增功能:
- [模块名] (<files>)
  * 功能点1
  * 功能点2

📚 文档更新:
- [文档名]: 更新内容

🔧 系统集成:
- 集成方式说明

✅ 测试验证:
- 测试结果摘要

🎯 技术亮点:
- 关键技术点1
- 关键技术点2
```

**Type类型**:
- `feat`: 新功能
- `docs`: 文档更新
- `fix`: Bug修复
- `refactor`: 重构
- `test`: 测试相关
- `chore`: 构建/工具

**示例**:
```
feat: 实现NUMA Key级别迁移模块 (v2.4)

✨ 新增功能:
- Key级别迁移模块 (numa_key_migrate.h/c)
  * Key粒度迁移框架
  * LRU集成热度追踪
  * STRING类型迁移

📚 文档更新:
- 更新05-numa-key-migrate.md标注实现状态
- 在NUMA_DEVELOPMENT_LOG.md中添加v2.4记录

🔧 系统集成:
- 在server.h中引入模块头文件
- 在server.c的initServer()之后初始化

✅ 测试验证:
- 所有测试通过
- 性能: 169K-188K req/s

🎯 技术亮点:
- Key作为迁移基本单位
- 与numa_migrate模块和谐共存
```

#### 5.3 推送验证

```bash
# 推送到远程仓库
git push origin main

# 验证推送成功
git log --oneline -3
```

---

## ✅ 关键原则

### DO（必须做）

| # | 原则 | 说明 |
|---|------|------|
| 1 | **先读后写** | 使用 search_replace 前必须 read_file |
| 2 | **询问优先** | 设计不明确时停下来问用户 |
| 3 | **保持一致** | 代码实现与文档设计保持同步 |
| 4 | **测试驱动** | 每次实现后立即测试验证 |
| 5 | **增量开发** | 复杂功能分步实现，逐步验证 |
| 6 | **日志详细** | 开发日志记录决策过程和实现细节 |
| 7 | **保留兼容** | 新旧模块和谐共存（如numa_migrate） |

### DON'T（禁止做）

| # | 禁止行为 | 说明 |
|---|----------|------|
| 1 | **猜测设计** | 不确定时不要自行决定 |
| 2 | **跳过测试** | 实现后必须测试 |
| 3 | **创建冗余文档** | 避免主动创建 .md 文件 |
| 4 | **修改热度结构** | 不改变 Redis 原生 LRU 机制 |
| 5 | **并行修改** | 文件修改必须顺序执行 |
| 6 | **忽略警告** | 注意编译器警告（C99 variadic macro除外） |
| 7 | **删除旧代码** | 保留功能正常的旧模块 |

---

## 📋 开发Checklist

### 新模块开发Checklist

- [ ] **阶段1: 准备**
  - [ ] 1. 阅读 docs/modules/ 设计文档
  - [ ] 2. 询问用户确认设计细节
  - [ ] 3. 确认依赖模块已就绪

- [ ] **阶段2: 实现**
  - [ ] 4. 创建头文件定义接口
  - [ ] 5. 实现源文件核心功能
  - [ ] 6. 更新 Makefile
  - [ ] 7. 集成到 server.h/server.c

- [ ] **阶段3: 测试**
  - [ ] 8. 编译测试（make clean && make -j4）
  - [ ] 9. 功能测试（启动Redis检查日志）
  - [ ] 10. 性能测试（redis-benchmark）

- [ ] **阶段4: 文档**
  - [ ] 11. 更新 NUMA_DEVELOPMENT_LOG.md
  - [ ] 12. 更新模块文档实现状态
  - [ ] 13. 必要时更新 README.md

- [ ] **阶段5: 提交**
  - [ ] 14. Git 提交（符合格式规范）
  - [ ] 15. Git 推送到远程仓库

### 文档更新Checklist

- [ ] **版本信息**
  - [ ] 1. 更新版本号（v2.x）
  - [ ] 2. 更新日期
  - [ ] 3. 更新"上次更新"字段

- [ ] **内容更新**
  - [ ] 4. 列出新增功能（✅）
  - [ ] 5. 列出待实现功能（⚠️）
  - [ ] 6. 记录关键决策和理由

- [ ] **技术细节**
  - [ ] 7. 更新调用链说明
  - [ ] 8. 添加代码示例
  - [ ] 9. 更新性能数据

- [ ] **一致性检查**
  - [ ] 10. 检查文档与代码一致
  - [ ] 11. 检查链接有效性

---

## 🔧 常见问题处理

### 编译错误

#### 1. 符号未定义

**错误**: `undefined reference to 'serverLog'`

**原因**: Redis内部使用 `_serverLog`（带下划线）

**解决**:
```c
// 添加前向声明
extern void _serverLog(int level, const char *fmt, ...);

// 使用宏包装
#define STRATEGY_LOG(level, fmt, ...) _serverLog(level, fmt, ##__VA_ARGS__)
```

#### 2. 链接顺序错误

**错误**: `undefined reference to '_serverLog'` (即使已声明)

**原因**: .o 文件链接顺序不对

**解决**:
```makefile
# 将 NUMA 相关 .o 放在最后
REDIS_SERVER_OBJ=... server.o ... numa_strategy_slots.o numa_key_migrate.o
```

#### 3. 类型不匹配

**错误**: `comparison of integer expressions of different signedness`

**解决**:
```c
// 使用显式类型转换
for (int priority = (int)STRATEGY_PRIORITY_HIGH; 
     priority >= (int)STRATEGY_PRIORITY_LOW; 
     priority--)
```

### 运行时错误

#### 1. 段错误（SIGSEGV）

**症状**: Redis启动时崩溃，退出码139

**可能原因**:
- 初始化时序错误
- PREFIX结构不匹配
- 访问未初始化的结构

**调试方法**:
```bash
# 使用 gdb 调试
gdb ./src/redis-server
(gdb) run
(gdb) bt  # 查看调用栈
```

**常见解决方案**:
```c
// ✅ 确保在 initServer() 之后初始化
int main() {
    initServer();
    numa_strategy_init();  // 正确位置
}
```

#### 2. 模块未初始化

**症状**: 模块功能不工作，日志显示"not initialized"

**检查清单**:
- [ ] `#ifdef HAVE_NUMA` 宏是否正确
- [ ] 初始化函数是否被调用
- [ ] 初始化函数是否返回成功

### 性能问题

#### 1. 吞吐量低

**期望**: 169K-188K req/s  
**实际**: < 100K req/s

**排查步骤**:
```bash
# 1. 检查内存池配置
grep POOL_SIZE src/numa_pool.c

# 2. 验证 NUMA 分配
numastat -p $(pidof redis-server)

# 3. 运行性能测试
./src/redis-benchmark -t set,get -n 100000 -q

# 4. 检查是否有频繁的内存分配
perf record -g ./src/redis-server
perf report
```

#### 2. 延迟高

**期望**: p50 < 0.05ms  
**实际**: p50 > 0.1ms

**可能原因**:
- 跨NUMA节点访问
- 内存池不足，频繁分配
- 策略调度开销过大

---

## 📚 项目知识库

### 文件组织

```
redis-CXL-in-v6.2.21/
│
├── src/                           # 源代码
│   ├── numa_pool.h/c              # v2.1 NUMA内存池
│   ├── numa_migrate.h/c           # v2.2 基础内存迁移
│   ├── numa_strategy_slots.h/c    # v2.3 策略插槽框架
│   ├── numa_key_migrate.h/c       # v2.4 Key级别迁移
│   ├── zmalloc.h/c                # 内存分配器集成点
│   ├── server.h/c                 # Redis核心（集成点）
│   └── Makefile                   # 构建配置
│
├── docs/                          # 文档
│   ├── modules/                   # 模块设计文档
│   │   ├── 05-numa-key-migrate.md
│   │   ├── 06-numa-strategy-slots.md
│   │   └── 07-numa-composite-lru.md
│   └── original/                  # 原版Redis文档备份
│       ├── REDIS_ORIGINAL_README.md
│       └── TLS.md
│
├── README.md                      # 项目入口（708行）
├── NUMA_DEVELOPMENT_LOG.md        # 开发日志（完整记录）
├── WORKFLOW.md                    # 工作流规范（本文档）
└── test_numa.sh                   # 综合测试脚本
```

### 关键接口

#### 内存分配接口
```c
// NUMA感知的内存分配（在指定节点分配）
void* numa_zmalloc_onnode(size_t size, int node);

// 标准内存分配（自动选择节点）
void* zmalloc(size_t size);

// 内存释放
void zfree(void *ptr);
```

#### 内存迁移接口
```c
// 基础内存块迁移
void* numa_migrate_memory(void *ptr, size_t size, int target_node);

// Key级别迁移
int numa_migrate_single_key(redisDb *db, robj *key, int target_node);
int numa_migrate_multiple_keys(redisDb *db, list *key_list, int target_node);
int numa_migrate_entire_database(redisDb *db, int target_node);
```

#### 热度追踪接口
```c
// 记录Key访问（在lookupKey等处调用）
void numa_record_key_access(robj *key, robj *val);

// 热度衰减（周期性调用）
void numa_perform_heat_decay(void);

// 获取Key元数据
key_numa_metadata_t* numa_get_key_metadata(robj *key);
```

#### 策略框架接口
```c
// 策略注册
int numa_strategy_register_factory(const numa_strategy_factory_t *factory);

// 策略创建
numa_strategy_t* numa_strategy_create(const char *name);

// 插槽操作
int numa_strategy_slot_insert(int slot_id, const char *strategy_name);
int numa_strategy_slot_enable(int slot_id);

// 策略执行
void numa_strategy_run_all(void);
int numa_strategy_run_slot(int slot_id);
```

### 关键数据结构

#### 1. 内存池结构
```c
typedef struct {
    void *base;           // 池起始地址
    size_t size;          // 池大小
    size_t used;          // 已使用大小
    int node_id;          // NUMA节点ID
} numa_pool_t;
```

#### 2. 策略实例
```c
typedef struct {
    int slot_id;                         // 插槽ID
    const char *name;                    // 策略名称
    numa_strategy_type_t type;           // 策略类型
    numa_strategy_priority_t priority;   // 优先级
    const numa_strategy_vtable_t *vtable; // 虚函数表
    void *private_data;                  // 私有数据
    uint64_t total_executions;           // 执行次数
} numa_strategy_t;
```

#### 3. Key元数据
```c
typedef struct {
    int current_node;           // 当前NUMA节点
    uint8_t hotness_level;      // 热度等级(0-7)
    uint16_t last_access_time;  // LRU时间戳
    uint64_t access_count;      // 访问计数
} key_numa_metadata_t;
```

### 性能基线

| 指标 | 值 | 说明 |
|------|-----|------|
| SET吞吐 | 169,491 req/s | 单线程 |
| GET吞吐 | 188,679 req/s | 单线程 |
| p50延迟 | 0.031 ms | SET/GET平均 |
| p99延迟 | 0.079 ms | SET操作 |
| 内存开销 | ~32 bytes/key | 元数据开销 |
| NUMA本地率 | >95% | 内存访问 |

### 重要约定

#### 插槽编号
- **插槽0**: No-op兜底策略（已实现）
- **插槽1**: 复合LRU默认策略（规划中）
- **插槽2-15**: 用户自定义策略

#### 日志级别
```c
#define LL_DEBUG    0
#define LL_VERBOSE  1
#define LL_NOTICE   2
#define LL_WARNING  3
```

#### 返回值约定
```c
#define NUMA_OK       0
#define NUMA_ERR     -1
#define NUMA_ENOENT  -2  // 不存在
#define NUMA_EINVAL  -3  // 参数无效
#define NUMA_ENOMEM  -4  // 内存不足
#define NUMA_ETYPE   -5  // 类型不支持
```

---

## 📝 工作流使用指南

### 开始新任务时

1. **阅读本文档**，回顾工作流程
2. **检查记忆系统**，回顾历史经验
3. **查阅设计文档**，理解需求背景
4. **询问用户确认**，避免误解需求

### 开发过程中

1. **遵循Checklist**，逐步推进
2. **遇到疑问立即询问**，不要猜测
3. **编译后立即测试**，尽早发现问题
4. **及时更新文档**，保持同步

### 完成任务后

1. **运行综合测试**，确保质量
2. **更新开发日志**，记录过程
3. **提交代码**，遵循规范
4. **推送到远程**，完成闭环

---

## 🔄 持续改进

本工作流规范会根据实践经验不断优化。如发现：

- ❓ 流程不合理之处
- 🐛 反复出现的问题
- 💡 更好的实践方式

请及时更新本文档，保持规范的时效性和实用性。

---

**最后更新**: 2026-02-13  
**版本**: v1.0  
**维护**: AI开发助手
