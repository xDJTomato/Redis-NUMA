#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Visualize YCSB value-size sweep benchmark — academic single-figure outputs."""

import argparse
import csv
import os
import sys


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
    rows = [r for r in rows if r["value_size_bytes"] != 1310720]
    rows.sort(key=lambda r: r["value_size_bytes"])
    return rows


PALETTE = {
    "Redis-NUMA (Composite LRU)": "#1565C0",
    "Redis-NUMA interleaved": "#1565C0",
    "Redis-NUMA (TinyLFU)": "#D32F2F",
    "Redis-NUMA": "#1565C0",
    "Vanilla Redis": "#2E7D32",
    "Vanilla Redis (local)": "#2E7D32",
    "Vanilla Redis (remote)": "#666666",
    "Vanilla Redis (interleaved)": "#7B1FA2",
    "NUMA": "#1565C0",
    "Vanilla": "#2E7D32",
}
FALLBACK = ["#1565C0", "#D32F2F", "#2E7D32", "#7B1FA2", "#F57C00", "#666666"]


def color_for(label, index):
    return PALETTE.get(label, FALLBACK[index % len(FALLBACK)])


def size_labels(all_sizes):
    labels = []
    for size in all_sizes:
        if size >= 1048576:
            mib = size / 1048576
            labels.append(f"{mib:.2f}M" if mib != int(mib) else f"{int(mib)}M")
        elif size >= 1024:
            labels.append(f"{size // 1024}K" if size % 1024 == 0
                          else f"{size/1024:.1f}K")
        else:
            labels.append(f"{size}B")
    return labels


def setup_xaxis(ax, all_sizes, labels):
    ax.set_xscale("log", base=2)
    ax.set_xticks(all_sizes)
    ax.set_xticklabels(labels, rotation=0, ha="center", fontsize=7)
    ax.set_xlabel("Value Size", fontsize=9)
    ax.grid(True, alpha=0.3, linestyle="--")


def save_fig(fig, path, dpi):
    try:
        fig.savefig(path, dpi=dpi, facecolor="white", edgecolor="none",
                    bbox_inches="tight", pad_inches=0.08)
        w, h = fig.get_size_inches()
        print(f"  Saved: {path} ({int(w*dpi)}x{int(h*dpi)}px)")
        return True
    except Exception as exc:
        print(f"  ERROR saving {path}: {exc}")
        return False


def plot_throughput(datasets, all_sizes, labels, output_path, dpi):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(7, 3.8))
    for idx, (label, rows) in enumerate(datasets):
        sizes = [r["value_size_bytes"] for r in rows]
        values = [r["throughput_ops_sec"] for r in rows]
        ax.plot(sizes, values, marker="o", linewidth=1.6, markersize=4,
                color=color_for(label, idx), label=label)
    ax.set_ylabel("Throughput (ops/s)", fontsize=9)
    ax.set_title("(a) Request Throughput vs. Value Size", fontsize=10)
    setup_xaxis(ax, all_sizes, labels)
    ax.legend(fontsize=8, loc="best", frameon=True)
    ax.tick_params(labelsize=7)
    fig.tight_layout()
    ok = save_fig(fig, output_path, dpi)
    plt.close(fig)
    return ok


def plot_bandwidth(datasets, all_sizes, labels, output_path, dpi):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(7, 3.8))
    for idx, (label, rows) in enumerate(datasets):
        sizes = [r["value_size_bytes"] for r in rows]
        values = [r["bandwidth_mib_sec"] for r in rows]
        ax.plot(sizes, values, marker="o", linewidth=1.6, markersize=4,
                color=color_for(label, idx), label=label)
    ax.set_ylabel("Bandwidth (MiB/s)", fontsize=9)
    ax.set_title("(b) Application-Level Read Bandwidth vs. Value Size", fontsize=10)
    setup_xaxis(ax, all_sizes, labels)
    ax.legend(fontsize=8, loc="best", frameon=True)
    ax.tick_params(labelsize=7)
    fig.tight_layout()
    ok = save_fig(fig, output_path, dpi)
    plt.close(fig)
    return ok


def plot_latency(datasets, all_sizes, labels, output_path, dpi):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    styles = [
        ("read_avg_us", "Avg", "-", "o"),
        ("read_p99_us", "P99", ":", "^"),
    ]

    fig, ax = plt.subplots(figsize=(7, 3.8))
    for idx, (label, rows) in enumerate(datasets):
        sizes = [r["value_size_bytes"] for r in rows]
        color = color_for(label, idx)
        for key, suffix, ls, marker in styles:
            ax.plot(sizes, [r[key] for r in rows], marker=marker, linewidth=1.1,
                    markersize=3.5, linestyle=ls, color=color,
                    label=f"{label} {suffix}")
    ax.set_ylabel("Latency (µs)", fontsize=9)
    ax.set_title("(c) Read Latency vs. Value Size", fontsize=10)
    setup_xaxis(ax, all_sizes, labels)
    ax.legend(fontsize=7, loc="best", ncol=2, frameon=True)
    ax.tick_params(labelsize=7)
    fig.tight_layout()
    ok = save_fig(fig, output_path, dpi)
    plt.close(fig)
    return ok


def main():
    parser = argparse.ArgumentParser(description="Visualize YCSB value-size sweep benchmark")
    parser.add_argument("--input", "-i", required=True, help="size_sweep_summary.csv")
    parser.add_argument("--output", "-o", required=True, help="output PNG base path (suffix auto-added)")
    parser.add_argument("--label", default="Redis-NUMA (Composite LRU)", help="label for primary input")
    parser.add_argument("--compare-input", help="second size_sweep_summary.csv")
    parser.add_argument("--compare-label", default="Vanilla Redis", help="label for compare input")
    parser.add_argument("--compare-input2", help="third size_sweep_summary.csv")
    parser.add_argument("--compare-label2", default="Vanilla Redis (remote)", help="label for third input")
    parser.add_argument("--compare-input3", help="fourth size_sweep_summary.csv")
    parser.add_argument("--compare-label3", default="Vanilla Redis (interleaved)", help="label for fourth input")
    parser.add_argument("--title", default=None, help="(unused, kept for CLI compat)")
    parser.add_argument("--dpi", type=int, default=150, help="DPI (default: 150)")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"ERROR: File not found: {args.input}")
        sys.exit(1)

    datasets = [(args.label, parse_csv(args.input, args.label))]
    print(f"Parsing: {args.input}")
    print(f"  {len(datasets[0][1])} size points")

    if args.compare_input:
        if not os.path.exists(args.compare_input):
            print(f"ERROR: File not found: {args.compare_input}")
            sys.exit(1)
        compare_rows = parse_csv(args.compare_input, args.compare_label)
        datasets.append((args.compare_label, compare_rows))
        print(f"Parsing: {args.compare_input}")
        print(f"  {len(compare_rows)} size points")

    if args.compare_input2:
        if not os.path.exists(args.compare_input2):
            print(f"ERROR: File not found: {args.compare_input2}")
            sys.exit(1)
        compare_rows2 = parse_csv(args.compare_input2, args.compare_label2)
        datasets.append((args.compare_label2, compare_rows2))
        print(f"Parsing: {args.compare_input2}")
        print(f"  {len(compare_rows2)} size points")

    if args.compare_input3:
        if not os.path.exists(args.compare_input3):
            print(f"ERROR: File not found: {args.compare_input3}")
            sys.exit(1)
        compare_rows3 = parse_csv(args.compare_input3, args.compare_label3)
        datasets.append((args.compare_label3, compare_rows3))
        print(f"Parsing: {args.compare_input3}")
        print(f"  {len(compare_rows3)} size points")

    all_sizes = sorted({r["value_size_bytes"] for _, rows in datasets for r in rows})
    labels = size_labels(all_sizes)

    base, ext = os.path.splitext(args.output)
    if not ext:
        ext = ".png"

    ok = True
    ok &= plot_throughput(datasets, all_sizes, labels, f"{base}_throughput{ext}", args.dpi)
    ok &= plot_bandwidth(datasets, all_sizes, labels, f"{base}_bandwidth{ext}", args.dpi)
    ok &= plot_latency(datasets, all_sizes, labels, f"{base}_latency{ext}", args.dpi)
    print(f"Done. Output base: {base}_*{ext}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
