#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Progressive YCSB hotspot benchmark visualizer."""

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
            rows.append({
                "label": label,
                "threads": int(safe_float(row.get("threads"), 0)),
                "throughput_ops_sec": safe_float(row.get("throughput_ops_sec")),
                "bandwidth_mib_sec": safe_float(row.get("bandwidth_mib_sec")),
                "read_avg_us": safe_float(row.get("read_avg_us")),
                "read_p95_us": safe_float(row.get("read_p95_us")),
                "read_p99_us": safe_float(row.get("read_p99_us")),
                "update_avg_us": safe_float(row.get("update_avg_us")),
                "used_mem_mb": safe_float(row.get("used_mem_mb")),
                "rss_mb": safe_float(row.get("rss_mb")),
                "frag_ratio": safe_float(row.get("frag_ratio")),
                "numa_local_live_mb": safe_float(row.get("numa_local_live_mb")),
                "numa_remote_live_mb": safe_float(row.get("numa_remote_live_mb")),
                "numa_node0_live_mb": safe_float(row.get("numa_node0_live_mb")),
                "numa_node2_live_mb": safe_float(row.get("numa_node2_live_mb")),
                "remote_pct": safe_float(row.get("remote_pct")),
            })
    rows.sort(key=lambda r: r["threads"])
    return rows


PALETTE = {
    "Redis-NUMA (Composite LRU)": "#1565C0",
    "Redis-NUMA (TinyLFU)": "#D32F2F",
    "Redis-NUMA": "#1565C0",
    "Vanilla Redis": "#2E7D32",
    "Vanilla Redis (local memory)": "#2E7D32",
    "Vanilla Redis (local)": "#2E7D32",
    "Vanilla Redis (remote)": "#666666",
    "Vanilla Redis (interleaved)": "#7B1FA2",
    "NUMA": "#1565C0",
    "Vanilla": "#2E7D32",
}
FALLBACK = ["#1565C0", "#D32F2F", "#2E7D32", "#7B1FA2", "#F57C00", "#666666"]


def color_for(label, index):
    return PALETTE.get(label, FALLBACK[index % len(FALLBACK)])


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


def setup_xaxis(ax, all_threads):
    max_threads = max(all_threads) if all_threads else 0
    ax.set_xlim(0, max_threads)
    ax.set_xlabel("YCSB Client Threads", fontsize=9)
    ax.grid(True, alpha=0.3, linestyle="--")


def plot_throughput(datasets, all_threads, output_path, title, dpi):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(7, 3.8))
    for idx, (label, rows) in enumerate(datasets):
        threads = [r["threads"] for r in rows]
        values = [r["throughput_ops_sec"] for r in rows]
        ax.plot(threads, values, marker="o", linewidth=1.6, markersize=4,
                color=color_for(label, idx), label=label)
    ax.set_ylabel("Throughput (ops/s)", fontsize=9)
    ax.set_title(title or "(a) Throughput Scalability vs. Thread Count", fontsize=10)
    ax.set_ylim(bottom=0)
    setup_xaxis(ax, all_threads)
    ax.legend(fontsize=8, loc="best", frameon=True)
    ax.tick_params(labelsize=7)
    fig.tight_layout()
    ok = save_fig(fig, output_path, dpi)
    plt.close(fig)
    return ok


def plot_latency(datasets, all_threads, output_path, dpi):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    styles = [
        ("read_avg_us", "Avg", "-", "o"),
        ("read_p99_us", "P99", ":", "^"),
    ]

    fig, ax = plt.subplots(figsize=(7, 3.8))
    for idx, (label, rows) in enumerate(datasets):
        threads = [r["threads"] for r in rows]
        color = color_for(label, idx)
        for key, suffix, ls, marker in styles:
            ax.plot(threads, [r[key] for r in rows], marker=marker, linewidth=1.1,
                    markersize=3.5, linestyle=ls, color=color,
                    label=f"{label} {suffix}")
    ax.set_ylabel("Latency (µs)", fontsize=9)
    ax.set_title("(b) Read Latency vs. Thread Count (Avg / P99)", fontsize=10)
    ax.set_ylim(bottom=0)
    setup_xaxis(ax, all_threads)
    ax.legend(fontsize=7, loc="best", ncol=2, frameon=True)
    ax.tick_params(labelsize=7)
    fig.tight_layout()
    ok = save_fig(fig, output_path, dpi)
    plt.close(fig)
    return ok


def plot(datasets, output_path, title=None, dpi=150):
    datasets = [(label, rows) for label, rows in datasets if rows]
    if not datasets:
        print("ERROR: No rows to plot")
        return False

    all_threads = sorted({r["threads"] for _, rows in datasets for r in rows})
    base, ext = os.path.splitext(output_path)
    if not ext:
        ext = ".png"

    ok = True
    ok &= plot_throughput(datasets, all_threads, f"{base}_throughput{ext}", title, dpi)
    ok &= plot_latency(datasets, all_threads, f"{base}_latency{ext}", dpi)

    # compatibility copy: main output = throughput
    try:
        import shutil
        shutil.copy2(f"{base}_throughput{ext}", output_path)
        print(f"  Compatibility copy: {output_path}")
    except Exception:
        pass

    print(f"Done. Output base: {base}_*{ext}")
    return ok


def main():
    parser = argparse.ArgumentParser(description="Visualize progressive YCSB hotspot benchmark")
    parser.add_argument("--input", "-i", required=True, help="progressive_summary.csv")
    parser.add_argument("--output", "-o", required=True, help="output PNG")
    parser.add_argument("--label", default="Redis-NUMA (Composite LRU)", help="label for primary input")
    parser.add_argument("--compare-input", help="second progressive_summary.csv")
    parser.add_argument("--compare-label", default="Vanilla Redis", help="label for compare input")
    parser.add_argument("--compare-input2", help="third progressive_summary.csv")
    parser.add_argument("--compare-label2", default="Vanilla Redis (interleaved)", help="label for third input")
    parser.add_argument("--compare-input3", help="fourth progressive_summary.csv")
    parser.add_argument("--compare-label3", default="Vanilla Redis (remote)", help="label for fourth input")
    parser.add_argument("--title", default=None, help="figure title")
    parser.add_argument("--dpi", type=int, default=150, help="DPI (default: 150)")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"ERROR: File not found: {args.input}")
        sys.exit(1)

    datasets = [(args.label, parse_csv(args.input, args.label))]
    print(f"Parsing: {args.input}")
    print(f"  {len(datasets[0][1])} thread points: {[r['threads'] for r in datasets[0][1]]}")

    if args.compare_input:
        if not os.path.exists(args.compare_input):
            print(f"ERROR: File not found: {args.compare_input}")
            sys.exit(1)
        compare_rows = parse_csv(args.compare_input, args.compare_label)
        datasets.append((args.compare_label, compare_rows))
        print(f"Parsing: {args.compare_input}")
        print(f"  {len(compare_rows)} thread points: {[r['threads'] for r in compare_rows]}")

    if args.compare_input2:
        if not os.path.exists(args.compare_input2):
            print(f"ERROR: File not found: {args.compare_input2}")
            sys.exit(1)
        compare_rows2 = parse_csv(args.compare_input2, args.compare_label2)
        datasets.append((args.compare_label2, compare_rows2))
        print(f"Parsing: {args.compare_input2}")
        print(f"  {len(compare_rows2)} thread points: {[r['threads'] for r in compare_rows2]}")

    if args.compare_input3:
        if not os.path.exists(args.compare_input3):
            print(f"ERROR: File not found: {args.compare_input3}")
            sys.exit(1)
        compare_rows3 = parse_csv(args.compare_input3, args.compare_label3)
        datasets.append((args.compare_label3, compare_rows3))
        print(f"Parsing: {args.compare_input3}")
        print(f"  {len(compare_rows3)} thread points: {[r['threads'] for r in compare_rows3]}")

    ok = plot(datasets, args.output, args.title, args.dpi)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
