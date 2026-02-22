# 测试文件归档目录

本目录集中存放所有历史测试文件和结果，用于追溯项目演进历史。

## 目录结构

```
tests/legacy/
├── README.md           # 本文档
├── scripts/            # 归档的脚本和配置
├── logs/               # 历史日志文件
└── results/            # 测试结果和数据文件
```

## 归档文件清单

### 根目录脚本

**performance_eval.sh**
- 描述：性能评估脚本
- 时间：2026-02-14
- 状态：已被专用测试脚本替代
- 位置：`tests/legacy/scripts/`

**benchmark_results.txt**
- 描述：基准测试结果
- 内容：早期性能基准数据
- 位置：`tests/legacy/scripts/`

**start_cxl_vm.sh**
- 描述：CXL 虚拟机启动脚本（空文件）
- 状态：未使用
- 位置：根目录保留

### 日志文件

**redis.log**
- 描述：Redis 运行日志
- 大小：4.2KB
- 位置：`tests/legacy/scripts/`

**redis_p2_stress.log**
- 描述：P2 阶段压力测试日志
- 大小：30.7KB
- 内容：早期压力测试记录
- 位置：`tests/legacy/scripts/`

### 数据文件

**dump.rdb**
- 描述：Redis 持久化快照
- 大小：210 MB
- 状态：测试遗留数据
- 位置：`tests/legacy/results/`
- 备注：可定期清理以节省空间

## 当前活跃测试目录

项目测试文件已按功能模块化组织：

### tests/numa/ - NUMA 功能测试
```
tests/numa/
├── README.md                    # NUMA 测试文档
├── run_all_tests.sh             # 批量运行所有测试
├── test_8g_aggressive.sh        # 8GB 激进分配测试
├── test_8g_fragmentation.sh     # 8GB 碎片率测试
├── test_composite_lru.sh        # Composite LRU 策略测试
├── test_composite_lru_cxl.sh    # CXL 环境 LRU 测试
├── test_cxl_balance.sh          # CXL 负载均衡测试
├── test_lru_migration.sh        # LRU 迁移测试
├── test_numa.sh                 # NUMA 基础功能测试
├── test_numa_config.sh          # NUMA 配置测试
├── test_integration.c           # 集成测试（C）
├── test_migrate.c               # 迁移功能测试（C）
├── test_prefix_heat.c           # PREFIX 热度测试（C）
└── test_prefix_heat_direct.c    # PREFIX 直接热度测试（C）
```

**推荐用途**：
- 日常开发：`./run_all_tests.sh`
- 单项测试：`./test_numa.sh`
- 性能测试：`./test_8g_aggressive.sh`

### tests/ycsb/ - YCSB 压力测试
```
tests/ycsb/
├── README.md                          # YCSB 测试文档
├── run_migration_high_pressure_v2.sh  # 高强度测试（推荐）⭐
├── run_migration_test.sh              # 基础迁移测试
├── workloads/
│   └── workload_numa_migration_full   # 迁移策略全覆盖
└── legacy/                            # YCSB 历史文件
    ├── README.md                      # YCSB 归档说明
    ├── scripts/                       # 旧版脚本
    └── results/                       # 历史结果
```

**推荐用途**：
- 日常验证：`./run_migration_test.sh`（2分钟）
- 压力测试：`./run_migration_high_pressure_v2.sh`（10GB）

## 清理建议

### 定期清理项（每月）
- `dump.rdb` - 删除测试遗留的大文件
- `*.log` 超过 50MB 的日志文件

### 永久保留项
- 所有 `.sh` 脚本（追溯演进历史）
- 代表性测试结果（如 benchmark_results.txt）
- README 和文档文件

### 清理命令
```bash
# 清理大型数据文件
cd tests/legacy/results
rm -f dump.rdb

# 清理大型日志（保留最近的）
find tests/legacy/logs -name "*.log" -size +50M -mtime +30 -delete
```

## 文件迁移历史

### 2026-02-22 第一次整理
- ✅ 创建 `tests/ycsb/legacy/` 归档 YCSB 历史文件
- ✅ 创建 `tests/legacy/` 归档根目录测试文件
- ✅ 保留 `tests/numa/` 作为活跃测试目录

### 迁移原则
1. **活跃文件**：保留在功能目录（tests/numa, tests/ycsb）
2. **历史文件**：迁移到对应的 legacy 目录
3. **根目录**：只保留核心文档和配置，测试文件迁移

## 相关文档

- [NUMA 测试文档](../numa/README.md)
- [YCSB 测试文档](../ycsb/README.md)
- [YCSB Legacy 归档](../ycsb/legacy/README.md)
- [NUMA 开发日志](../../NUMA_DEVELOPMENT_LOG.md)

---

**维护者**：NUMA 开发团队  
**最后更新**：2026-02-22
