#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Visualize YCSB value-size sweep benchmark."""

import argparse
import csv
import os
import sys
from datetime import datetime


def safe_float(value, default=0.0):
    try:
        return float(str(value).strip())
    except (TypeError, ValueError):
        return default


def parse_csv(path, label):
    rows = []
    with open(path, "r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            size = int(safe_float(row.get("value_size_bytes"), 0))
            rows.append({
                "label": label,
                "value_size_bytes": size,
                "value_size_label": row.get("value_size_label") or str(size),
                "operation_count": int(safe_float(row.get("operation_count"), 0)),
                "read_ok": int(safe_float(row.get("read_ok"), 0)),
                "read_error": int(safe_float(row.get("read_error"), 0)),
                "read_error_pct": safe_float(row.get("read_error_pct")),
                "throughput_ops_sec": safe_float(row.get("throughput_ops_sec")),
                "bandwidth_mib_sec": safe_float(row.get("bandwidth_mib_sec")),
                "read_avg_us": safe_float(row.get("read_avg_us")),
                "read_p95_us": safe_float(row.get("read_p95_us")),
                "read_p99_us": safe_float(row.get("read_p99_us")),
                "used_mem_mb": safe_float(row.get("used_mem_mb")),
                "rss_mb": safe_float(row.get("rss_mb")),
                "frag_ratio": safe_float(row.get("frag_ratio")),
                "numa_local_live_mb": safe_float(row.get("numa_local_live_mb")),
                "numa_remote_live_mb": safe_float(row.get("numa_remote_live_mb")),
                "numa_node0_live_mb": safe_float(row.get("numa_node0_live_mb")),
                "numa_node2_live_mb": safe_float(row.get("numa_node2_live_mb")),
                "remote_pct": safe_float(row.get("remote_pct")),
            })
    rows.sort(key=lambda r: r["value_size_bytes"])
    return rows


def plot(datasets, output_path, title=None, dpi=150):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    datasets = [(label, rows) for label, rows in datasets if rows]
    if not datasets:
        print("ERROR: No rows to plot")
        return False

    fig, axes = plt.subplots(2, 2, figsize=(15, 8.5))
    if title is None:
        title = "YCSB Hotspot Read Value Size Sweep"
    fig.suptitle(f"{title}  ({datetime.now().strftime('%Y-%m-%d %H:%M:%S')})",
                 fontsize=13, fontweight="bold")

    palette = {
        "Redis-NUMA": "#1565C0",
        "Vanilla Redis": "#D32F2F",
        "NUMA": "#1565C0",
        "Vanilla": "#D32F2F",
    }
    fallback = ["#1565C0", "#D32F2F", "#388E3C", "#F57C00", "#7B1FA2"]

    def color_for(label, index):
        return palette.get(label, fallback[index % len(fallback)])

    all_sizes = sorted({r["value_size_bytes"] for _, rows in datasets for r in rows})
    labels = []
    for size in all_sizes:
        if size >= 1048576:
            labels.append("1MiB")
        elif size >= 1024:
            labels.append(f"{size // 1024}KiB" if size % 1024 == 0 else f"{size/1024:.1f}KiB")
        else:
            labels.append(f"{size}B")

    def setup_xaxis(ax):
        ax.set_xscale("log", base=2)
        ax.set_xticks(all_sizes)
        ax.set_xticklabels(labels, rotation=45, ha="right")
        ax.grid(True, alpha=0.3, linestyle="--")

    def draw_metric(ax, key, ylabel, panel_title):
        for index, (label, rows) in enumerate(datasets):
            sizes = [r["value_size_bytes"] for r in rows]
            values = [r[key] for r in rows]
            color = color_for(label, index)
            ax.plot(sizes, values, marker="o", linewidth=1.6, markersize=4,
                    color=color, label=label)
        ax.set_title(panel_title, fontsize=10)
        ax.set_xlabel("Value size", fontsize=8)
        ax.set_ylabel(ylabel, fontsize=8)
        setup_xaxis(ax)
        ax.legend(fontsize=7, loc="best")

    draw_metric(axes[0, 0], "throughput_ops_sec", "ops/sec", "Request Processing Speed")
    draw_metric(axes[0, 1], "bandwidth_mib_sec", "MiB/sec", "Application-level Read Bandwidth")

    ax = axes[1, 0]
    styles = [
        ("read_avg_us", "avg", "-", "o"),
        ("read_p95_us", "p95", "--", "s"),
        ("read_p99_us", "p99", ":", "^"),
    ]
    for index, (label, rows) in enumerate(datasets):
        sizes = [r["value_size_bytes"] for r in rows]
        color = color_for(label, index)
        for key, suffix, linestyle, marker in styles:
            ax.plot(sizes, [r[key] for r in rows], marker=marker, linewidth=1.1,
                    markersize=3.5, linestyle=linestyle, color=color,
                    label=f"{label} {suffix}")
    ax.set_title("Read Latency", fontsize=10)
    ax.set_xlabel("Value size", fontsize=8)
    ax.set_ylabel("microseconds", fontsize=8)
    setup_xaxis(ax)
    ax.legend(fontsize=6.5, loc="best", ncol=2)

    ax = axes[1, 1]
    for index, (label, rows) in enumerate(datasets):
        sizes = [r["value_size_bytes"] for r in rows]
        color = color_for(label, index)
        local_pct = []
        remote_pct = []
        for r in rows:
            total = r["numa_local_live_mb"] + r["numa_remote_live_mb"]
            if total > 0:
                local_pct.append(r["numa_local_live_mb"] * 100.0 / total)
                remote_pct.append(r["numa_remote_live_mb"] * 100.0 / total)
            else:
                local_pct.append(0.0)
                remote_pct.append(0.0)
        ax.plot(sizes, local_pct, marker="o", linewidth=1.2,
                markersize=3.5, color=color, label=f"{label} local")
        ax.plot(sizes, remote_pct, marker="s", linewidth=1.2,
                markersize=3.5, linestyle="--", color=color, label=f"{label} remote")
    ax.set_title("Local / Remote Memory Share", fontsize=10)
    ax.set_xlabel("Value size", fontsize=8)
    ax.set_ylabel("percent of NUMA live memory", fontsize=8)
    ax.set_ylim(-2, 102)
    setup_xaxis(ax)
    ax.legend(fontsize=6.5, loc="best", ncol=2)

    for ax in axes.flat:
        ax.tick_params(labelsize=7)

    fig.subplots_adjust(left=0.07, right=0.93, top=0.91, bottom=0.13, hspace=0.36, wspace=0.28)

    try:
        plt.savefig(output_path, dpi=dpi, facecolor="white", edgecolor="none")
        w, h = fig.get_size_inches()
        print(f"Report saved: {output_path} ({int(w*dpi)}x{int(h*dpi)}px @ {dpi} DPI)")
        return True
    except Exception as exc:
        print(f"ERROR: Failed to save report: {exc}")
        return False
    finally:
        plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description="Visualize YCSB value-size sweep benchmark")
    parser.add_argument("--input", "-i", required=True, help="size_sweep_summary.csv")
    parser.add_argument("--output", "-o", required=True, help="output PNG")
    parser.add_argument("--label", default="Redis-NUMA", help="label for primary input")
    parser.add_argument("--compare-input", help="second size_sweep_summary.csv")
    parser.add_argument("--compare-label", default="Vanilla Redis", help="label for compare input")
    parser.add_argument("--title", default=None, help="figure title")
    parser.add_argument("--dpi", type=int, default=150, help="DPI (default: 150)")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"ERROR: File not found: {args.input}")
        sys.exit(1)

    datasets = [(args.label, parse_csv(args.input, args.label))]
    print(f"Parsing: {args.input}")
    print(f"  {len(datasets[0][1])} size points: {[r['value_size_bytes'] for r in datasets[0][1]]}")

    if args.compare_input:
        if not os.path.exists(args.compare_input):
            print(f"ERROR: File not found: {args.compare_input}")
            sys.exit(1)
        compare_rows = parse_csv(args.compare_input, args.compare_label)
        datasets.append((args.compare_label, compare_rows))
        print(f"Parsing: {args.compare_input}")
        print(f"  {len(compare_rows)} size points: {[r['value_size_bytes'] for r in compare_rows]}")

    ok = plot(datasets, args.output, args.title, args.dpi)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
