# NUMA 测试脚本目录

本目录包含 Redis NUMA 扩展的所有测试脚本。

## 测试脚本分类

### 1. 功能测试脚本

| 脚本 | 功能 | 使用方法 |
|------|------|---------|
| `test_lru_migration.sh` | LRU 迁移策略手动触发测试 | `./test_lru_migration.sh` |
| `test_numa.sh` | NUMA 基础功能测试 | `./test_numa.sh` |
| `test_numa_config.sh` | NUMA 配置测试 | `./test_numa_config.sh` |
| `test_cxl_balance.sh` | CXL 负载均衡测试 | `./test_cxl_balance.sh` |

### 2. 性能与压力测试

| 脚本 | 功能 | 使用方法 |
|------|------|---------|
| `test_8g_aggressive.sh` | 8GB 内存激进压力测试 | `./test_8g_aggressive.sh` |
| `test_8g_fragmentation.sh` | 8GB 内存碎片测试 | `./test_8g_fragmentation.sh` |
| `test_composite_lru.sh` | Composite LRU 策略测试 | `./test_composite_lru.sh` |
| `test_composite_lru_cxl.sh` | Composite LRU CXL 测试 | `./test_composite_lru_cxl.sh` |

### 3. 单元测试源码

| 文件 | 功能 | 编译方法 |
|------|------|---------|
| `test_prefix_heat.c` | PREFIX 热度 API 测试 | `make test_prefix_heat` |
| `test_prefix_heat_direct.c` | PREFIX 热度直接测试 | `make test_prefix_heat_direct` |
| `test_integration.c` | 集成测试 | `make test_integration` |
| `test_migrate.c` | 迁移功能单元测试 | `make test_migrate` |

## 快速开始

### 运行所有基础测试

```bash
cd tests/numa
./run_all_tests.sh
```

### 运行单个测试

```bash
# LRU 迁移测试
./test_lru_migration.sh

# 8GB 压力测试
./test_8g_fragmentation.sh
```

### 编译并运行单元测试

```bash
# 编译 PREFIX 热度测试
gcc -DHAVE_NUMA -o test_prefix_heat test_prefix_heat.c \
    ../src/zmalloc.o ../src/numa_pool.o -I.. -lnuma -lpthread

# 运行测试
./test_prefix_heat
```

## 测试说明

### test_lru_migration.sh

功能：
- 启动 Redis 服务器
- 插入 100 个测试 key
- 模拟访问模式（热点/中频/低频）
- 手动触发 Key 迁移到节点 1
- 验证数据完整性
- 性能测试

预期输出：
- 所有 key 迁移成功
- 数据完整性验证通过
- 性能 > 150K req/s

### test_prefix_heat.c

功能：
- 测试 PREFIX 热度 API
- 验证 hotness/access_count/last_access 字段
- 测试边界条件

## 注意事项

1. 运行测试前确保 Redis 已编译：`make`
2. 部分测试需要 root 权限或 NUMA 支持
3. 压力测试会占用大量内存，确保系统有足够资源
4. 测试完成后会自动清理 Redis 进程

## 相关文档

- [NUMA_DEVELOPMENT_LOG.md](../../docs/NUMA_DEVELOPMENT_LOG.md)
- [docs/modules/07-numa-composite-lru.md](../../docs/modules/07-numa-composite-lru.md)
- [docs/modules/05-numa-key-migrate.md](../../docs/modules/05-numa-key-migrate.md)
