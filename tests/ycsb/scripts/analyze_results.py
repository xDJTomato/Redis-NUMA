#!/usr/bin/env python3
"""
YCSB 测试结果分析工具
生成图表和详细报告
"""

import os
import sys
import re
import json
import glob
from datetime import datetime
from collections import defaultdict

def parse_ycsb_log(log_file):
    """解析 YCSB 日志文件"""
    results = {
        'throughput': 0,
        'operations': {},
        'overall': {}
    }
    
    with open(log_file, 'r') as f:
        content = f.read()
    
    # 解析吞吐量
    throughput_match = re.search(r'Throughput\(ops/sec\), ([\d.]+)', content)
    if throughput_match:
        results['throughput'] = float(throughput_match.group(1))
    
    # 解析各操作类型的统计
    op_pattern = r'\[(\w+)\], (\w+), ([\d.]+)'
    for match in re.finditer(op_pattern, content):
        op_type, metric, value = match.groups()
        if op_type not in results['operations']:
            results['operations'][op_type] = {}
        results['operations'][op_type][metric] = float(value)
    
    return results

def parse_redis_stats(stats_file):
    """解析 Redis 统计信息"""
    stats = {}
    
    with open(stats_file, 'r') as f:
        content = f.read()
    
    # 解析关键指标
    patterns = {
        'used_memory': r'used_memory:(\d+)',
        'used_memory_human': r'used_memory_human:([\d.]+\w+)',
        'total_commands_processed': r'total_commands_processed:(\d+)',
        'instantaneous_ops_per_sec': r'instantaneous_ops_per_sec:(\d+)',
        'keyspace_hits': r'keyspace_hits:(\d+)',
        'keyspace_misses': r'keyspace_misses:(\d+)',
    }
    
    for key, pattern in patterns.items():
        match = re.search(pattern, content)
        if match:
            stats[key] = match.group(1)
    
    return stats

def generate_report(results_dir):
    """生成测试报告"""
    print("=" * 60)
    print("YCSB NUMA 测试分析报告")
    print("=" * 60)
    print(f"分析时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"结果目录: {results_dir}")
    print()
    
    # 收集所有测试结果
    all_results = []
    
    for run_log in glob.glob(os.path.join(results_dir, '*_run.log')):
        basename = os.path.basename(run_log).replace('_run.log', '')
        
        # 解析 YCSB 结果
        ycsb_results = parse_ycsb_log(run_log)
        
        # 尝试解析对应的 Redis 统计
        stats_file = run_log.replace('_run.log', '_redis_stats.txt')
        redis_stats = {}
        if os.path.exists(stats_file):
            redis_stats = parse_redis_stats(stats_file)
        
        all_results.append({
            'name': basename,
            'ycsb': ycsb_results,
            'redis': redis_stats
        })
    
    if not all_results:
        print("未找到测试结果文件")
        return
    
    # 按吞吐量排序
    all_results.sort(key=lambda x: x['ycsb']['throughput'], reverse=True)
    
    # 打印摘要
    print("性能排名:")
    print("-" * 60)
    print(f"{'排名':<6}{'测试名称':<40}{'吞吐量(ops/sec)':<20}")
    print("-" * 60)
    
    for i, result in enumerate(all_results, 1):
        name = result['name'][:38]
        throughput = result['ycsb']['throughput']
        print(f"{i:<6}{name:<40}{throughput:<20.2f}")
    
    print()
    print("-" * 60)
    
    # 详细分析
    print("\n详细分析:")
    print("=" * 60)
    
    for result in all_results:
        print(f"\n测试: {result['name']}")
        print("-" * 60)
        
        ycsb = result['ycsb']
        print(f"吞吐量: {ycsb['throughput']:.2f} ops/sec")
        
        # 各操作类型延迟
        for op_type, metrics in ycsb['operations'].items():
            if 'AverageLatency' in metrics:
                avg_lat = metrics['AverageLatency']
                p95_lat = metrics.get('95thPercentileLatency', 'N/A')
                p99_lat = metrics.get('99thPercentileLatency', 'N/A')
                print(f"  [{op_type}] Avg: {avg_lat:.2f}us, P95: {p95_lat}us, P99: {p99_lat}us")
        
        # Redis 统计
        if result['redis']:
            redis = result['redis']
            print(f"\n  Redis 统计:")
            if 'used_memory_human' in redis:
                print(f"    内存使用: {redis['used_memory_human']}")
            if 'instantaneous_ops_per_sec' in redis:
                print(f"    实时 OPS: {redis['instantaneous_ops_per_sec']}")
            if 'keyspace_hits' in redis and 'keyspace_misses' in redis:
                hits = int(redis['keyspace_hits'])
                misses = int(redis['keyspace_misses'])
                total = hits + misses
                if total > 0:
                    hit_rate = (hits / total) * 100
                    print(f"    缓存命中率: {hit_rate:.2f}%")
    
    # 生成 JSON 报告
    json_report = os.path.join(results_dir, f'analysis_{datetime.now().strftime("%Y%m%d_%H%M%S")}.json')
    with open(json_report, 'w') as f:
        json.dump(all_results, f, indent=2)
    
    print(f"\n\nJSON 报告已保存: {json_report}")

def main():
    if len(sys.argv) > 1:
        results_dir = sys.argv[1]
    else:
        results_dir = os.path.join(os.path.dirname(__file__), '..', 'results')
    
    if not os.path.exists(results_dir):
        print(f"错误: 结果目录不存在: {results_dir}")
        sys.exit(1)
    
    generate_report(results_dir)

if __name__ == '__main__':
    main()
