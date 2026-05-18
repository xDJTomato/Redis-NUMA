#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NUMA Bandwidth Benchmark Visualizer
Reads metrics.csv and generates an overlaid NUMA/vanilla benchmark report.
"""

import argparse
import csv
import os
import re
import sys
from datetime import datetime


def safe_float(s, default=0.0):
    try:
        return float(str(s).strip())
    except (TypeError, ValueError, AttributeError):
        return default


def parse_csv(filepath, label):
    data = {
        'label': label,
        'time': [],
        'phase': [],
        'ops_sec': [],
        'used_mem_mb': [],
        'rss_mb': [],
        'frag_ratio': [],
        'migrate_sec': [],
        'accesses_local': [],
        'accesses_remote': [],
        'local_access_pct': [],
    }
    phase_markers = []
    first_ts = None

    try:
        with open(filepath, 'r', newline='', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            for row in reader:
                if not row:
                    continue
                if row.get('timestamp') == 'PHASE_MARKER':
                    values = list(row.values())
                    try:
                        phase_markers.append((int(values[3]), values[1], values[2]))
                    except (ValueError, IndexError, TypeError):
                        pass
                    continue

                ts = int(safe_float(row.get('timestamp'), -1))
                if ts < 0:
                    continue
                if first_ts is None:
                    first_ts = ts

                local = safe_float(row.get('accesses_local'), 0)
                remote = safe_float(row.get('accesses_remote'), 0)
                total = local + remote

                data['time'].append(ts - first_ts)
                data['phase'].append(row.get('phase') or '')
                data['ops_sec'].append(safe_float(row.get('ops_sec'), 0))
                data['used_mem_mb'].append(safe_float(row.get('used_mem_mb'), 0))
                data['rss_mb'].append(safe_float(row.get('rss_mb'), 0))
                data['frag_ratio'].append(safe_float(row.get('frag_ratio'), 0))
                data['migrate_sec'].append(safe_float(row.get('migrate_sec'), 0))
                data['accesses_local'].append(local)
                data['accesses_remote'].append(remote)
                data['local_access_pct'].append(local * 100.0 / total if total > 0 else 0.0)
    except (FileNotFoundError, PermissionError, OSError) as exc:
        print(f"ERROR: {exc}")
        return data, [], None

    markers = []
    for marker_ts, phase_num, phase_name in phase_markers:
        markers.append((marker_ts - first_ts if first_ts else 0, phase_num, phase_name))

    return data, markers, first_ts


def parse_phase_latency(result_dir):
    if not result_dir:
        return []
    phase_files = [
        ('P1 Fill', 'phase1_load.txt'),
        ('P2 Hotspot', 'phase2_hotspot.txt'),
        ('P3 Sustain', 'phase3_sustain.txt'),
    ]
    latencies = []
    pattern = re.compile(r'^\[(READ|UPDATE|INSERT)\],\s*AverageLatency\(us\),\s*([0-9.]+)')
    for phase_label, filename in phase_files:
        path = os.path.join(result_dir, filename)
        values = []
        try:
            with open(path, 'r', encoding='utf-8', errors='replace') as f:
                for line in f:
                    match = pattern.match(line.strip())
                    if match:
                        values.append(safe_float(match.group(2)))
        except OSError:
            pass
        if values:
            latencies.append((phase_label, sum(values) / len(values)))
    return latencies


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


PALETTE = ['#1565C0', '#D32F2F', '#388E3C', '#F57C00']


def get_phase_ranges(data):
    ranges = {}
    cur = None
    start = None
    for t, p in zip(data['time'], data['phase']):
        if p != cur:
            if cur is not None and start is not None:
                ranges[cur] = (start, t)
            cur = p
            start = t
    if cur is not None and start is not None and data['time']:
        ranges[cur] = (start, data['time'][-1])
    return ranges


def add_phase_markers(ax, datasets):
    ymax = ax.get_ylim()[1]
    if ymax <= 0:
        ymax = 1
    for i, data in enumerate(datasets):
        ranges = get_phase_ranges(data)
        color = PALETTE[i % len(PALETTE)]
        label = data['label']
        y = ymax * (0.92 - i * 0.08)
        for phase_name in ('1_fill', '2_hotspot', '3_sustain'):
            if phase_name not in ranges:
                continue
            start, end = ranges[phase_name]
            ax.hlines(y, start, end, color=color, linewidth=2.0, alpha=0.55)
            ax.vlines([start, end], y - ymax * 0.015, y + ymax * 0.015,
                      color=color, linewidth=0.8, alpha=0.45)
            ax.text((start + end) / 2, y + ymax * 0.018,
                    f"{label} {PHASE_LABELS.get(phase_name, phase_name)}",
                    ha='center', va='bottom', fontsize=6.5, color=color, alpha=0.75)


def common_time_axis(ax, max_t, phase_ranges):
    ax.set_xlim(0, max_t)
    ax.grid(True, alpha=0.3, linestyle='--')


PHASE_ORDER = ['1_fill', '2_hotspot', '3_sustain']


def normalize_time_by_phase(data):
    """Normalize elapsed time so that each phase maps to a common grid.

    Returns (norm_time, phase_boundaries) where phase_boundaries maps
    phase_name -> (norm_start, norm_end) on the unified axis.
    The unified axis uses the maximum observed phase duration across all
    callers, but this function first computes per-dataset durations.
    """
    ranges = get_phase_ranges(data)
    durations = {}
    for p in PHASE_ORDER:
        if p in ranges:
            s, e = ranges[p]
            durations[p] = e - s
        else:
            durations[p] = 0
    return ranges, durations


def build_unified_grid(all_durations):
    """Build a common time grid from the max duration of each phase."""
    grid = {}
    offset = 0
    for p in PHASE_ORDER:
        dur = max(d.get(p, 0) for d in all_durations)
        grid[p] = (offset, offset + dur)
        offset += dur
    return grid, offset


def remap_time(data, own_ranges, unified_grid):
    """Remap a dataset's raw elapsed time to the unified phase-aligned axis."""
    norm = []
    for t, phase in zip(data['time'], data['phase']):
        if phase in own_ranges and phase in unified_grid:
            raw_start, raw_end = own_ranges[phase]
            uni_start, uni_end = unified_grid[phase]
            raw_dur = raw_end - raw_start
            uni_dur = uni_end - uni_start
            if raw_dur > 0:
                frac = (t - raw_start) / raw_dur
                norm.append(uni_start + frac * uni_dur)
            else:
                norm.append(uni_start)
        else:
            norm.append(t)
    return norm


def plot_report(datasets, latency_sets, output_path, title=None, dpi=150):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    datasets = [d for d in datasets if d['time']]
    if not datasets:
        print("ERROR: No data points to plot")
        return False

    if title is None:
        title = 'Three-Phase YCSB Full-Test Throughput'

    all_ranges = []
    all_durations = []
    for data in datasets:
        ranges, durations = normalize_time_by_phase(data)
        all_ranges.append(ranges)
        all_durations.append(durations)

    unified_grid, total_t = build_unified_grid(all_durations)

    fig, ax = plt.subplots(1, 1, figsize=(8.5, 4.8))
    fig.suptitle(title, fontsize=13, fontweight='bold')

    for i, data in enumerate(datasets):
        nt = remap_time(data, all_ranges[i], unified_grid)
        ax.plot(nt, data['ops_sec'], color=PALETTE[i % len(PALETTE)], linewidth=0.9,
                label=f"{data['label']} ops/sec")

    for phase in PHASE_ORDER:
        if phase in unified_grid:
            s, e = unified_grid[phase]
            bg = PHASE_COLORS.get(phase, '#F5F5F5')
            ax.axvspan(s, e, color=bg, alpha=0.35, zorder=0)
            ax.axvline(s, color='#9E9E9E', linewidth=0.6, linestyle=':', alpha=0.6)
            lbl = PHASE_LABELS.get(phase, phase)
            ax.text((s + e) / 2, 0.97, lbl, ha='center', va='top',
                    fontsize=8.5, fontweight='bold', color='#616161', alpha=0.8,
                    transform=ax.get_xaxis_transform())
    if PHASE_ORDER[-1] in unified_grid:
        ax.axvline(unified_grid[PHASE_ORDER[-1]][1], color='#9E9E9E',
                   linewidth=0.6, linestyle=':', alpha=0.6)

    ax.set_title('Request Throughput', fontsize=11)
    ax.set_xlabel('Elapsed Time (s, phase-aligned)', fontsize=10)
    ax.set_ylabel('Throughput (ops/s)', fontsize=10)
    ax.set_ylim(bottom=0)
    ax.set_xlim(0, total_t)
    ax.grid(True, alpha=0.3, linestyle='--')

    ax2 = ax.twinx()
    ratio_plotted = False
    for i, data in enumerate(datasets):
        if any(v > 0 for v in data['local_access_pct']):
            nt = remap_time(data, all_ranges[i], unified_grid)
            ax2.plot(nt, data['local_access_pct'], color='#2E7D32', linewidth=1.0,
                     linestyle=':', label=f"{data['label']} local access %")
            ratio_plotted = True
            break
    ax2.set_ylabel('Local NUMA Access Ratio (%)', fontsize=10, color='#2E7D32')
    ax2.tick_params(axis='y', labelcolor='#2E7D32', labelsize=7)
    ax2.set_ylim(0, 100)
    lines1, labels1 = ax.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax.legend(lines1 + lines2, labels1 + labels2, fontsize=8, loc='lower right', frameon=True)
    if not ratio_plotted:
        ax2.set_yticks([])

    ax.tick_params(labelsize=8)
    fig.subplots_adjust(left=0.10, right=0.88, top=0.86, bottom=0.14)

    try:
        plt.savefig(output_path, dpi=dpi, facecolor='white', edgecolor='none',
                    bbox_inches='tight', pad_inches=0.08)
        w, h = fig.get_size_inches()
        print(f"Report saved: {output_path} ({int(w*dpi)}x{int(h*dpi)}px @ {dpi} DPI)")
    except Exception as exc:
        print(f"ERROR: Failed to save: {exc}")
        return False
    finally:
        plt.close(fig)

    return True


def main():
    parser = argparse.ArgumentParser(description='NUMA Bandwidth Benchmark Visualizer')
    parser.add_argument('--input', '-i', required=True, help='Path to primary metrics.csv')
    parser.add_argument('--output', '-o', required=True, help='Output PNG path')
    parser.add_argument('--label', default='Redis-NUMA', help='Label for primary input')
    parser.add_argument('--phase-dir', help='Directory containing primary phase*.txt files')
    parser.add_argument('--compare-input', help='Path to comparison metrics.csv')
    parser.add_argument('--compare-label', default='Vanilla Redis', help='Label for comparison input')
    parser.add_argument('--compare-phase-dir', help='Directory containing comparison phase*.txt files')
    parser.add_argument('--title', default=None, help='Figure title')
    parser.add_argument('--dpi', type=int, default=150, help='DPI (default: 150)')
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"ERROR: File not found: {args.input}")
        sys.exit(1)

    print(f"Parsing: {args.input}")
    primary, _, first_ts = parse_csv(args.input, args.label)
    primary['first_ts'] = first_ts
    datasets = [primary]
    latency_sets = [{
        'label': args.label,
        'latencies': parse_phase_latency(args.phase_dir or os.path.dirname(args.input)),
    }]
    print(f"  {len(primary['time'])} points")

    if args.compare_input:
        if not os.path.exists(args.compare_input):
            print(f"ERROR: File not found: {args.compare_input}")
            sys.exit(1)
        print(f"Parsing: {args.compare_input}")
        compare, _, compare_first_ts = parse_csv(args.compare_input, args.compare_label)
        compare['first_ts'] = compare_first_ts
        datasets.append(compare)
        latency_sets.append({
            'label': args.compare_label,
            'latencies': parse_phase_latency(args.compare_phase_dir or os.path.dirname(args.compare_input)),
        })
        print(f"  {len(compare['time'])} points")

    ok = plot_report(datasets, latency_sets, args.output, args.title, args.dpi)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
