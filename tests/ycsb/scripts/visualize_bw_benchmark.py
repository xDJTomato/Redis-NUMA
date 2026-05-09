#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NUMA Bandwidth Benchmark Visualizer
Reads metrics.csv and generates multi-panel benchmark report.

Usage:
    python3 visualize_bw_benchmark.py --input metrics.csv --output report.png [--dpi 150]
"""

import argparse
import csv
import sys
import os
from datetime import datetime


def safe_float(s, default=0.0):
    try:
        return float(str(s).strip())
    except (ValueError, AttributeError):
        return default


def parse_csv(filepath):
    """Parse metrics.csv, skip PHASE_MARKER rows, return data dict.

    Supports both old format (12 columns) and new format (14 columns with
    accesses_local/accesses_remote).
    """
    data = {
        'time': [],
        'phase': [],
        'ops_sec': [],
        'used_mem_mb': [],
        'rss_mb': [],
        'frag_ratio': [],
        'migrate_total': [],
        'migrate_sec': [],
        'evicted_keys': [],
        'accesses_local': [],
        'accesses_remote': [],
    }
    phase_markers = []
    first_ts = None

    try:
        with open(filepath, 'r', newline='', encoding='utf-8') as f:
            reader = csv.reader(f)
            try:
                next(reader)
            except StopIteration:
                return data, phase_markers, None

            for row in reader:
                if not row:
                    continue
                if row[0] == 'PHASE_MARKER':
                    try:
                        if len(row) >= 4:
                            phase_markers.append((int(row[3]), row[1], row[2]))
                    except (ValueError, IndexError):
                        pass
                    continue

                try:
                    ts = int(row[0])
                    if first_ts is None:
                        first_ts = ts

                    data['time'].append(ts - first_ts)
                    data['phase'].append(row[1] if len(row) > 1 else '')
                    data['ops_sec'].append(safe_float(row[3] if len(row) > 3 else 0))
                    data['used_mem_mb'].append(safe_float(row[4] if len(row) > 4 else 0))
                    data['rss_mb'].append(safe_float(row[5] if len(row) > 5 else 0))
                    data['frag_ratio'].append(safe_float(row[6] if len(row) > 6 else 0))
                    data['migrate_total'].append(safe_float(row[7] if len(row) > 7 else 0))
                    data['migrate_sec'].append(safe_float(row[8] if len(row) > 8 else 0))
                    data['evicted_keys'].append(safe_float(row[11] if len(row) > 11 else 0))
                    data['accesses_local'].append(safe_float(row[12] if len(row) > 12 else 0))
                    data['accesses_remote'].append(safe_float(row[13] if len(row) > 13 else 0))
                except (ValueError, IndexError):
                    continue
    except (FileNotFoundError, PermissionError, Exception) as e:
        print(f"ERROR: {e}")
        return data, phase_markers, None

    markers = []
    for marker_ts, phase_num, phase_name in phase_markers:
        markers.append((marker_ts - first_ts if first_ts else 0, phase_num, phase_name))

    return data, markers, first_ts


def get_phase_ranges(data):
    ranges = {}
    cur = None
    start = None
    for i, (t, p) in enumerate(zip(data['time'], data['phase'])):
        if p != cur:
            if cur is not None and start is not None:
                ranges[cur] = (start, t)
            cur = p
            start = t
    if cur is not None and start is not None:
        ranges[cur] = (start, data['time'][-1])
    return ranges


PHASE_COLORS = {
    '1_fill': '#E3F2FD',
    '2_hotspot': '#FFF9C4',
    '3_sustain': '#FFEBEE',
}
PHASE_LABELS = {
    '1_fill': 'P1: Fill',
    '2_hotspot': 'P2: Hotspot',
    '3_sustain': 'P3: Sustain',
}


def shade_phases(ax, phase_ranges):
    for name, (s, e) in phase_ranges.items():
        ax.axvspan(s, e, alpha=0.3, color=PHASE_COLORS.get(name, '#F5F5F5'), zorder=0)
        if s > 0:
            ax.axvline(x=s, color='gray', linestyle='--', linewidth=0.8, alpha=0.6, zorder=1)
        mid = (s + e) / 2
        ylim = ax.get_ylim()
        ax.text(mid, ylim[1] - (ylim[1] - ylim[0]) * 0.06, PHASE_LABELS.get(name, name),
                ha='center', va='top', fontsize=7, alpha=0.6, zorder=5)


def plot_report(data, markers, first_ts, output_path, dpi=150):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    if not data['time']:
        print("ERROR: No data points to plot")
        return False

    has_access_data = any(v > 0 for v in data['accesses_local']) or \
                      any(v > 0 for v in data['accesses_remote'])
    has_migration = any(v > 0 for v in data['migrate_sec'])

    n_rows = 3
    n_cols = 2
    fig, axes = plt.subplots(n_rows, n_cols, figsize=(15, 9))

    if first_ts:
        ts_str = datetime.fromtimestamp(first_ts).strftime("%Y-%m-%d %H:%M:%S")
    else:
        ts_str = "Unknown"
    fig.suptitle(f'NUMA Bandwidth Benchmark Report  ({ts_str})', fontsize=13, fontweight='bold')

    pr = get_phase_ranges(data)
    t = data['time']
    max_t = max(t) if t else 0

    # [0,0] Throughput
    ax = axes[0, 0]
    ax.plot(t, data['ops_sec'], color='#1565C0', linewidth=0.8)
    ax.set_title('Throughput', fontsize=10)
    ax.set_ylabel('ops/sec')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_t)
    shade_phases(ax, pr)

    # [0,1] Memory Usage
    ax = axes[0, 1]
    ax.plot(t, data['used_mem_mb'], color='#1976D2', linewidth=0.8, label='used_memory')
    ax.plot(t, data['rss_mb'], color='#D32F2F', linewidth=0.8, label='RSS')
    ax.set_title('Memory Usage', fontsize=10)
    ax.set_ylabel('MB')
    ax.legend(fontsize=7, loc='best')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_t)
    shade_phases(ax, pr)

    # [1,0] Migration Rate
    ax = axes[1, 0]
    if has_migration:
        ax.plot(t, data['migrate_sec'], color='#7B1FA2', linewidth=0.8)
        ax.fill_between(t, data['migrate_sec'], alpha=0.15, color='#7B1FA2')
    ax.set_title('Migration Rate', fontsize=10)
    ax.set_ylabel('migrations/sec')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_t)
    shade_phases(ax, pr)

    # [1,1] Data Access Distribution (local vs remote, per sec)
    ax = axes[1, 1]
    if has_access_data:
        ax.plot(t, data['accesses_local'], color='#388E3C', linewidth=0.8, label='Local (DRAM)')
        ax.plot(t, data['accesses_remote'], color='#F57C00', linewidth=0.8, label='Remote (CXL)')

        ax2 = ax.twinx()
        remote_pct = []
        for lo, re in zip(data['accesses_local'], data['accesses_remote']):
            total = lo + re
            remote_pct.append(re / total * 100 if total > 0 else 0)
        ax2.plot(t, remote_pct, color='#C62828', linewidth=0.8, linestyle=':', alpha=0.7, label='Remote %')
        ax2.set_ylabel('Remote %', fontsize=8, color='#C62828')
        ax2.tick_params(axis='y', labelcolor='#C62828', labelsize=7)
        ax2.set_ylim(0, max(max(remote_pct) * 1.3, 10) if remote_pct else 100)

        lines1, labels1 = ax.get_legend_handles_labels()
        lines2, labels2 = ax2.get_legend_handles_labels()
        ax.legend(lines1 + lines2, labels1 + labels2, fontsize=7, loc='best')
    else:
        ax.text(0.5, 0.5, 'No access data\n(single-node or old CSV)',
                transform=ax.transAxes, ha='center', va='center', fontsize=10, alpha=0.5)
    ax.set_title('Data Access Distribution', fontsize=10)
    ax.set_ylabel('accesses/sec')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_t)
    shade_phases(ax, pr)

    # [2,0] Fragmentation
    ax = axes[2, 0]
    ax.plot(t, data['frag_ratio'], color='#795548', linewidth=0.8)
    ax.axhline(y=1.0, color='green', linestyle=':', alpha=0.6, linewidth=0.8, label='Ideal (1.0)')
    ax.set_title('Memory Fragmentation', fontsize=10)
    ax.set_ylabel('ratio')
    ax.legend(fontsize=7, loc='best')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_t)
    shade_phases(ax, pr)

    # [2,1] Evicted Keys
    ax = axes[2, 1]
    ax.plot(t, data['evicted_keys'], color='#C62828', linewidth=0.8)
    ax.set_title('Evicted Keys (Cumulative)', fontsize=10)
    ax.set_ylabel('count')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_t)
    shade_phases(ax, pr)

    for ax in axes[-1]:
        ax.set_xlabel('Time (seconds)', fontsize=8)

    fig.subplots_adjust(left=0.07, right=0.93, top=0.93, bottom=0.06, hspace=0.38, wspace=0.30)

    try:
        plt.savefig(output_path, dpi=dpi, facecolor='white', edgecolor='none')
        w, h = fig.get_size_inches()
        print(f"Report saved: {output_path} ({int(w*dpi)}x{int(h*dpi)}px @ {dpi} DPI)")
    except Exception as e:
        print(f"ERROR: Failed to save: {e}")
        return False
    finally:
        plt.close()

    return True


def main():
    parser = argparse.ArgumentParser(description='NUMA Bandwidth Benchmark Visualizer')
    parser.add_argument('--input', '-i', required=True, help='Path to metrics.csv')
    parser.add_argument('--output', '-o', required=True, help='Output PNG path')
    parser.add_argument('--dpi', type=int, default=150, help='DPI (default: 150)')
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"ERROR: File not found: {args.input}")
        sys.exit(1)

    print(f"Parsing: {args.input}")
    data, markers, first_ts = parse_csv(args.input)

    if not data['time']:
        print("ERROR: No data in CSV")
        sys.exit(1)

    pr = get_phase_ranges(data)
    print(f"  {len(data['time'])} points, {max(data['time'])}s, phases: {list(pr.keys())}")

    ok = plot_report(data, markers, first_ts, args.output, args.dpi)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
