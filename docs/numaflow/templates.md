# NUMAflow 模板库（新手友好）

模板是比「原子操作」和「13 个策略」更高层的开箱即用单元：每个模板是一个可直接运行的
DAG，带**用途说明 + 适用场景 + 默认参数**，新手只需按负载特征选一个，加载后微调阈值即可。

## 查看与使用

```bash
# CLI：列出 23 个模板（按 5 类分组）
./build/numaflow templates
# 导出某个模板的 DAG JSON
./build/numaflow template tier_caat tier_caat.json
# GUI：顶部下拉框选模板 -> Load Template，DAG 自动铺到画布
python gui/server.py    # http://127.0.0.1:8090
```

## 模板分类

### 1. tiering（分层迁移）
| 模板 | 说明 | 适用场景 |
| --- | --- | --- |
| tier_caat | 晋升热 + 降级冷（默认） | 通用分层 |
| tier_promote_hot | 只晋升热 key | DRAM 有余量 |
| tier_promote_freq | 只晋升高频 key | 频率倾斜负载 |
| tier_demote_cold | 只降级冷 key | 内存压力下回收 DRAM |
| tier_rebalance | 晋升+降级+再平衡 | 节点压力不均 |
| tier_cost_benefit | 只迁净收益为正的项 | 迁移带宽昂贵、保守 |

### 2. allocation（分配）
9 个 alloc_* 模板，对应 9 种分配策略。选型：单 socket 选 local_first；均匀访问选 interleave/round_robin；
混合对象大小选 cxl_optimized；延迟敏感选 latency_aware。

### 3. cost（成本目标）
| 模板 | 目标 | 场景 |
| --- | --- | --- |
| cost_min_latency | 最小化访问延迟 | 延迟敏感 |
| cost_min_migration | 最小化迁移次数 | 迁移带宽稀缺 |
| cost_balanced | 延迟 vs 迁移成本平衡 | 默认权衡 |

### 4. adaptive（自适应）
| 模板 | 档位 | 场景 |
| --- | --- | --- |
| adapt_conservative | 保守 | 易抖动负载 |
| adapt_balanced | 均衡 | 未知/变化负载 |
| adapt_aggressive | 激进 | DRAM 未充分利用 |

### 5. special（专项）
| 模板 | 说明 | 场景 |
| --- | --- | --- |
| cache_warming | 冷启动激进预热 | 重启/故障切换后 |
| hot_key_pinning | 把热 key 钉在 DRAM | 稳定小热集 |

## 新手建议路径
1. 不确定就用默认 tier_caat；
2. 频率倾斜 → tier_promote_freq 或 cost_min_latency；
3. 迁移带宽吃紧 → cost_min_migration；
4. 刚重启要快速预热 → cache_warming；
5. 想自动调优 → adapt_balanced。

模板由 numaflow/src/nf_template.c 实现，tests/test_template.c 保证 23 个模板都能构建出无环、引用合法原子操作的有效 DAG。
