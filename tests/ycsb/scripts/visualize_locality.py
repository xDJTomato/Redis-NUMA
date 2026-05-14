#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NUMA Locality Visualizer
Generates two reports:
  1. locality_report.png     — aggregate bar charts (summary)
  2. locality_timeseries.png — per-second time-series (like bw_benchmark)

Usage:
    python3 visualize_locality.py --input locality_summary.csv --output locality_report.png
    python3 visualize_locality.py --input locality_summary.csv --output locality_report.png --timeseries locality_timeseries.csv
"""

import argparse
import csv
import re
import sys
import os
from collections import defaultdict

def safe_float(s, default=0.0):
    try:
        return float(str(s).strip().replace(',', ''))
    except (ValueError, AttributeError):
        return default

def safe_int(s, default=0):
    try:
        return int(float(str(s).strip().replace(',', '')))
    except (ValueError, AttributeError):
        return default

LABEL_DISPLAY = {
    'numa_local': 'NUMA local_first',
    'numa_interleave': 'NUMA interleave',
    'vanilla': 'Vanilla (libc)',
}

LABEL_SHORT = {
    'numa_local': 'NUMA\nlocal_first',
    'numa_interleave': 'NUMA\ninterleave',
    'vanilla': 'Vanilla\n(libc)',
}

COLORS = {
    'numa_local': '#2196F3',
    'numa_interleave': '#FF9800',
    'vanilla': '#4CAF50',
}


def parse_summary_csv(filepath):
    rows = []
    with open(filepath, 'r', newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)
    csv_dir = os.path.dirname(filepath)
    for row in rows:
        tp = safe_float(row.get('throughput_ops', 0))
        if tp > 0:
            continue
        ycsb_file = os.path.join(csv_dir, f"ycsb_{row['label']}.txt")
        if not os.path.isfile(ycsb_file):
            continue
        last_ops = 0
        with open(ycsb_file, 'r', encoding='utf-8', errors='ignore') as yf:
            for line in yf:
                m = re.search(r'Throughput\(ops/sec\),\s*([0-9.]+)', line)
                if m:
                    last_ops = float(m.group(1))
                m2 = re.search(r'([0-9.]+)\s+current ops/sec', line)
                if m2:
                    last_ops = float(m2.group(1))
        if last_ops > 0:
            row['throughput_ops'] = str(last_ops)
    return rows


def parse_timeseries_csv(filepath):
    """Returns {label: {time_sec:[], node_loads:[], ...}}"""
    data = defaultdict(lambda: defaultdict(list))
    with open(filepath, 'r', newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            label = row['label']
            data[label]['time_sec'].append(safe_float(row.get('time_sec', 0)))
            data[label]['node_loads'].append(safe_float(row.get('node_loads', 0)))
            data[label]['node_load_misses'].append(safe_float(row.get('node_load_misses', 0)))
            data[label]['local_dram'].append(safe_float(row.get('local_dram', 0)))
            data[label]['remote_dram'].append(safe_float(row.get('remote_dram', 0)))
            data[label]['ops_sec'].append(safe_float(row.get('ops_sec', 0)))
            data[label]['used_mem_mb'].append(safe_float(row.get('used_mem_mb', 0)))
            data[label]['rss_mb'].append(safe_float(row.get('rss_mb', 0)))
            data[label]['frag_ratio'].append(safe_float(row.get('frag_ratio', 0)))
            data[label]['migrate_total'].append(safe_float(row.get('migrate_total', 0)))
            data[label]['migrate_sec'].append(safe_float(row.get('migrate_sec', 0)))
    return dict(data)


def parse_ycsb_throughput(csv_dir, labels):
    """Parse per-second throughput from YCSB status output files."""
    result = {}
    for label in labels:
        ycsb_file = os.path.join(csv_dir, f"ycsb_{label}.txt")
        if not os.path.isfile(ycsb_file):
            continue
        times = []
        ops = []
        with open(ycsb_file, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                m = re.search(r'(\d+) sec: (\d+) operations; ([0-9.]+) current ops/sec', line)
                if m:
                    times.append(int(m.group(1)))
                    ops.append(float(m.group(3)))
        if times:
            result[label] = {'time': times, 'ops': ops}
    return result


def draw_summary(rows, output, dpi):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    labels = [r['label'] for r in rows]
    display_labels = [LABEL_SHORT.get(l, l) for l in labels]
    colors = [COLORS.get(l, '#999') for l in labels]
    x = range(len(labels))

    load_hit_rates, dram_local_rates, throughputs, mem_n0, mem_n1 = [], [], [], [], []
    for r in rows:
        nl = safe_float(r.get('node_loads', 0))
        nlm = safe_float(r.get('node_load_misses', 0))
        ld = safe_float(r.get('local_dram', 0))
        rd = safe_float(r.get('remote_dram', 0))
        load_hit_rates.append((nl - nlm) / nl * 100 if nl > 0 else 0)
        dram_local_rates.append(ld / (ld + rd) * 100 if (ld + rd) > 0 else 0)
        throughputs.append(safe_float(r.get('throughput_ops', 0)))
        mem_n0.append(safe_float(r.get('mem_node0_mb', 0)))
        mem_n1.append(safe_float(r.get('mem_node1_mb', 0)))

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle('Redis-NUMA Locality Analysis', fontsize=16, fontweight='bold', y=0.98)

    def clean_ax(ax):
        ax.spines['top'].set_visible(False)
        ax.spines['right'].set_visible(False)

    def bar_labels(ax, bars, vals, fmt='{:.1f}%', offset=1):
        for bar, val in zip(bars, vals):
            ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + offset,
                    fmt.format(val), ha='center', va='bottom', fontsize=11, fontweight='bold')

    # P1: Node Load Hit Rate
    ax = axes[0][0]
    bars = ax.bar(x, load_hit_rates, color=colors, width=0.6, edgecolor='white', linewidth=1.5)
    ax.set_title('NUMA Node Load Hit Rate', fontsize=13, fontweight='bold')
    ax.set_ylabel('Hit Rate (%)'); ax.set_xticks(x); ax.set_xticklabels(display_labels, fontsize=10)
    ax.set_ylim(0, 105); ax.axhline(y=95, color='#ccc', ls='--', lw=0.8)
    bar_labels(ax, bars, load_hit_rates); clean_ax(ax)

    # P2: L3→Local DRAM
    ax = axes[0][1]
    bars = ax.bar(x, dram_local_rates, color=colors, width=0.6, edgecolor='white', linewidth=1.5)
    ax.set_title('L3 Miss → Local DRAM Rate', fontsize=13, fontweight='bold')
    ax.set_ylabel('Local DRAM (%)'); ax.set_xticks(x); ax.set_xticklabels(display_labels, fontsize=10)
    ax.set_ylim(0, 105); ax.axhline(y=90, color='#ccc', ls='--', lw=0.8)
    bar_labels(ax, bars, dram_local_rates); clean_ax(ax)

    # P3: Memory Distribution
    ax = axes[1][0]
    total = [n0 + n1 for n0, n1 in zip(mem_n0, mem_n1)]
    p0 = [n0/t*100 if t > 0 else 0 for n0, t in zip(mem_n0, total)]
    p1 = [n1/t*100 if t > 0 else 0 for n1, t in zip(mem_n1, total)]
    ax.bar(x, p0, width=0.6, color='#42A5F5', label='Node 0 (local)', edgecolor='white', linewidth=1.5)
    ax.bar(x, p1, width=0.6, bottom=p0, color='#EF5350', label='Node 1 (remote)', edgecolor='white', linewidth=1.5)
    ax.set_title('Memory Distribution by NUMA Node', fontsize=13, fontweight='bold')
    ax.set_ylabel('Proportion (%)'); ax.set_xticks(x); ax.set_xticklabels(display_labels, fontsize=10)
    ax.set_ylim(0, 115); ax.legend(loc='upper right', fontsize=9)
    for i, (v0, v1) in enumerate(zip(p0, p1)):
        if v0 > 5: ax.text(i, v0/2, f'{v0:.0f}%', ha='center', va='center', fontsize=10, color='white', fontweight='bold')
        if v1 > 5: ax.text(i, v0+v1/2, f'{v1:.0f}%', ha='center', va='center', fontsize=10, color='white', fontweight='bold')
    clean_ax(ax)

    # P4: Throughput
    ax = axes[1][1]
    tk = [t/1000 for t in throughputs]
    bars = ax.bar(x, tk, color=colors, width=0.6, edgecolor='white', linewidth=1.5)
    ax.set_title('Throughput (YCSB Hotspot 64T)', fontsize=13, fontweight='bold')
    ax.set_ylabel('K ops/s'); ax.set_xticks(x); ax.set_xticklabels(display_labels, fontsize=10)
    if max(tk) > 0: ax.set_ylim(0, max(tk) * 1.2)
    bar_labels(ax, bars, tk, fmt='{:.1f}K', offset=max(tk)*0.02 if max(tk) > 0 else 1)
    clean_ax(ax)

    # Raw counter table
    cell_text = [[f"{safe_int(r.get(k,0)):,}" for k in ('node_loads','node_load_misses','local_dram','remote_dram')] for r in rows]
    row_labels = [LABEL_DISPLAY.get(r['label'], r['label']) for r in rows]
    col_labels = ['node-loads', 'node-load-misses', 'local_dram', 'remote_dram']
    plt.tight_layout(rect=[0, 0.12, 1, 0.95])
    tbl_ax = fig.add_axes([0.08, 0.0, 0.84, 0.10]); tbl_ax.axis('off')
    tbl = tbl_ax.table(cellText=cell_text, rowLabels=row_labels, colLabels=col_labels, loc='center', cellLoc='right')
    tbl.auto_set_font_size(False); tbl.set_fontsize(9); tbl.scale(1, 1.3)
    for key, cell in tbl.get_celld().items():
        cell.set_edgecolor('#ddd')
        if key[0] == 0: cell.set_facecolor('#f0f0f0'); cell.set_text_props(fontweight='bold')

    plt.savefig(output, dpi=dpi, bbox_inches='tight', facecolor='white')
    print(f"Summary report saved: {output}")
    plt.close(fig)


def draw_timeseries(ts_data, ycsb_data, output, dpi):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    ordered = ['numa_local', 'numa_interleave', 'vanilla']
    present = [l for l in ordered if l in ts_data]
    if not present:
        print("No time-series data to plot", file=sys.stderr)
        return

    fig, axes = plt.subplots(3, 2, figsize=(16, 14))
    fig.suptitle('Redis-NUMA Locality — Time Series (per second)', fontsize=16, fontweight='bold', y=0.98)

    def style_ax(ax):
        ax.grid(True, alpha=0.3)
        ax.spines['top'].set_visible(False)
        ax.spines['right'].set_visible(False)

    # P1: Throughput over time
    ax = axes[0][0]
    for label in present:
        if label in ycsb_data:
            t = ycsb_data[label]['time']
            o = [v/1000 for v in ycsb_data[label]['ops']]
            ax.plot(t, o, color=COLORS.get(label), linewidth=2, label=LABEL_DISPLAY.get(label, label), marker='o', markersize=4)
    ax.set_title('Throughput', fontsize=13, fontweight='bold')
    ax.set_ylabel('K ops/s')
    ax.legend(fontsize=9, loc='lower right')
    style_ax(ax)

    # P2: Node Load Hit Rate over time
    ax = axes[0][1]
    for label in present:
        d = ts_data[label]
        t = d['time_sec']
        hit_rate = [(l - m) / l * 100 if l > 0 else 0 for l, m in zip(d['node_loads'], d['node_load_misses'])]
        ax.plot(t, hit_rate, color=COLORS.get(label), linewidth=2, label=LABEL_DISPLAY.get(label, label))
    ax.set_title('Node Load Hit Rate', fontsize=13, fontweight='bold')
    ax.set_ylabel('Hit Rate (%)')
    ax.set_ylim(0, 105)
    ax.axhline(y=95, color='#ccc', ls='--', lw=0.8)
    ax.legend(fontsize=9, loc='lower right')
    style_ax(ax)

    # P3: L3 Miss → Local DRAM Rate over time
    ax = axes[1][0]
    for label in present:
        d = ts_data[label]
        t = d['time_sec']
        local_rate = [l / (l + r) * 100 if (l + r) > 0 else 0 for l, r in zip(d['local_dram'], d['remote_dram'])]
        ax.plot(t, local_rate, color=COLORS.get(label), linewidth=2, label=LABEL_DISPLAY.get(label, label))
    ax.set_title('L3 Miss → Local DRAM Rate', fontsize=13, fontweight='bold')
    ax.set_ylabel('Local DRAM (%)')
    ax.set_ylim(0, 105)
    ax.axhline(y=90, color='#ccc', ls='--', lw=0.8)
    ax.legend(fontsize=9, loc='lower right')
    style_ax(ax)

    # P4: Memory Usage over time
    ax = axes[1][1]
    for label in present:
        d = ts_data[label]
        t = d['time_sec']
        um = d['used_mem_mb']
        rm = d['rss_mb']
        if any(v > 0 for v in um):
            ax.plot(t, um, color=COLORS.get(label), linewidth=2, label=f'{LABEL_DISPLAY.get(label, label)} used')
            ax.plot(t, rm, color=COLORS.get(label), linewidth=1, linestyle='--', alpha=0.6, label=f'{LABEL_DISPLAY.get(label, label)} RSS')
    ax.set_title('Memory Usage', fontsize=13, fontweight='bold')
    ax.set_ylabel('MB')
    ax.legend(fontsize=8, loc='lower right', ncol=2)
    style_ax(ax)

    # P5: Memory Fragmentation Ratio over time
    ax = axes[2][0]
    for label in present:
        d = ts_data[label]
        t = d['time_sec']
        fr = d['frag_ratio']
        if any(v > 0 for v in fr):
            ax.plot(t, fr, color=COLORS.get(label), linewidth=2, label=LABEL_DISPLAY.get(label, label))
    ax.axhline(y=1.0, color='#ccc', ls='--', lw=0.8, label='Ideal (1.0)')
    ax.set_title('Memory Fragmentation Ratio', fontsize=13, fontweight='bold')
    ax.set_ylabel('ratio')
    ax.set_xlabel('Time (seconds)')
    ax.legend(fontsize=9, loc='upper right')
    style_ax(ax)

    # P6: Key Migration over time
    ax = axes[2][1]
    has_migration = False
    for label in present:
        d = ts_data[label]
        t = d['time_sec']
        ms = d['migrate_sec']
        if any(v > 0 for v in ms):
            has_migration = True
            ax.plot(t, ms, color=COLORS.get(label), linewidth=2, label=LABEL_DISPLAY.get(label, label))
        mt = d['migrate_total']
        if any(v > 0 for v in mt) and not any(v > 0 for v in ms):
            has_migration = True
            ax.plot(t, mt, color=COLORS.get(label), linewidth=2, label=f'{LABEL_DISPLAY.get(label, label)} (cumul.)')
    if not has_migration:
        ax.text(0.5, 0.5, 'No migrations\n(local_first / vanilla)', transform=ax.transAxes,
                ha='center', va='center', fontsize=12, color='#999')
    ax.set_title('Key Migration', fontsize=13, fontweight='bold')
    ax.set_ylabel('migrations / sec')
    ax.set_xlabel('Time (seconds)')
    if has_migration:
        ax.legend(fontsize=9, loc='upper right')
    style_ax(ax)

    plt.tight_layout(rect=[0, 0, 1, 0.95])
    plt.savefig(output, dpi=dpi, bbox_inches='tight', facecolor='white')
    print(f"Time-series report saved: {output}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description='NUMA Locality Visualizer')
    parser.add_argument('--input', '-i', required=True, help='locality_summary.csv')
    parser.add_argument('--output', '-o', default='locality_report.png', help='Summary output image')
    parser.add_argument('--timeseries', '-t', default=None, help='locality_timeseries.csv (auto-detected if omitted)')
    parser.add_argument('--dpi', type=int, default=150)
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"ERROR: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    try:
        import matplotlib
        matplotlib.use('Agg')
    except ImportError:
        print("ERROR: matplotlib not installed", file=sys.stderr)
        sys.exit(1)

    # 1) Summary bar charts
    rows = parse_summary_csv(args.input)
    if rows:
        draw_summary(rows, args.output, args.dpi)

    # 2) Time-series charts (auto-detect if not specified)
    ts_file = args.timeseries
    csv_dir = os.path.dirname(args.input)
    if ts_file is None:
        candidate = os.path.join(csv_dir, 'locality_timeseries.csv')
        if os.path.isfile(candidate):
            ts_file = candidate

    if ts_file and os.path.isfile(ts_file):
        ts_data = parse_timeseries_csv(ts_file)
        labels = list(ts_data.keys()) if ts_data else [r['label'] for r in rows]
        ycsb_data = parse_ycsb_throughput(csv_dir, labels)

        ts_output = args.output.replace('.png', '_timeseries.png')
        if ts_output == args.output:
            ts_output = args.output.rsplit('.', 1)[0] + '_timeseries.png'
        draw_timeseries(ts_data, ycsb_data, ts_output, args.dpi)
    else:
        print("No time-series CSV found, skipping time-series chart")


if __name__ == '__main__':
    main()
