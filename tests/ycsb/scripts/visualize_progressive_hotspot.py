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


def plot(datasets, output_path, title=None, dpi=150):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import MaxNLocator

    datasets = [(label, rows) for label, rows in datasets if rows]
    if not datasets:
        print("ERROR: No rows to plot")
        return False

    palette = {
        "Redis-NUMA": "#1565C0",
        "Vanilla Redis": "#D32F2F",
        "Vanilla Redis (local memory)": "#D32F2F",
        "NUMA": "#1565C0",
        "Vanilla": "#D32F2F",
    }
    fallback = ["#1565C0", "#D32F2F", "#388E3C", "#F57C00", "#7B1FA2"]

    def color_for(label, index):
        return palette.get(label, fallback[index % len(fallback)])

    def output_variant(suffix):
        root, ext = os.path.splitext(output_path)
        return f"{root}_{suffix}{ext or '.png'}"

    all_threads = sorted({r["threads"] for _, rows in datasets for r in rows})
    max_threads = max(all_threads) if all_threads else 0
    major_ticks = [0]
    major_ticks.extend(t for t in all_threads if t % 32 == 0)
    if max_threads and max_threads not in major_ticks:
        major_ticks.append(max_threads)
    major_ticks = sorted(set(major_ticks))

    def style_axis(ax):
        ax.set_xlim(0, max_threads)
        ax.set_xticks(major_ticks)
        ax.xaxis.set_major_locator(MaxNLocator(nbins=6, integer=True))
        ax.grid(True, alpha=0.28, linestyle="--", linewidth=0.7)
        ax.tick_params(labelsize=9)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)

    throughput_path = output_variant("throughput")
    latency_path = output_variant("latency")

    fig, ax = plt.subplots(1, 1, figsize=(7.2, 4.2))
    ax.set_title(title or "Throughput under Progressive Hotspot Concurrency", fontsize=12, fontweight="bold")
    for index, (label, rows) in enumerate(datasets):
        threads = [r["threads"] for r in rows]
        values = [r["throughput_ops_sec"] for r in rows]
        color = color_for(label, index)
        ax.plot(threads, values, marker="o", linewidth=1.8, markersize=4.0,
                color=color, label=label)
    ax.set_xlabel("YCSB client threads", fontsize=10)
    ax.set_ylabel("Throughput (ops/s)", fontsize=10)
    ax.set_ylim(bottom=0)
    style_axis(ax)
    ax.legend(fontsize=9, loc="best", frameon=True)
    fig.subplots_adjust(left=0.12, right=0.90, top=0.86, bottom=0.16)

    try:
        plt.savefig(throughput_path, dpi=dpi, facecolor="white", edgecolor="none", bbox_inches="tight", pad_inches=0.08)
        plt.savefig(output_path, dpi=dpi, facecolor="white", edgecolor="none", bbox_inches="tight", pad_inches=0.08)
        w, h = fig.get_size_inches()
        print(f"Throughput figure saved: {throughput_path} ({int(w*dpi)}x{int(h*dpi)}px @ {dpi} DPI)")
        print(f"Compatibility copy saved: {output_path}")
    except Exception as exc:
        print(f"ERROR: Failed to save throughput figure: {exc}")
        plt.close(fig)
        return False
    finally:
        plt.close(fig)

    fig, ax = plt.subplots(1, 1, figsize=(7.2, 4.2))
    ax.set_title("Read Latency under Progressive Hotspot Concurrency", fontsize=12, fontweight="bold")
    for index, (label, rows) in enumerate(datasets):
        threads = [r["threads"] for r in rows]
        avg = [r["read_avg_us"] for r in rows]
        p99 = [r["read_p99_us"] for r in rows]
        color = color_for(label, index)
        ax.plot(threads, avg, marker="o", linewidth=1.8, markersize=4.0,
                color=color, label=f"{label} average")
        ax.plot(threads, p99, marker="s", linewidth=1.4, markersize=3.5,
                linestyle="--", color=color, alpha=0.78, label=f"{label} p99")
    ax.set_xlabel("YCSB client threads", fontsize=10)
    ax.set_ylabel("Read latency (µs)", fontsize=10)
    ax.set_ylim(bottom=0)
    style_axis(ax)
    ax.legend(fontsize=8, loc="best", frameon=True, ncol=1)
    fig.subplots_adjust(left=0.12, right=0.90, top=0.86, bottom=0.16)

    try:
        plt.savefig(latency_path, dpi=dpi, facecolor="white", edgecolor="none", bbox_inches="tight", pad_inches=0.08)
        w, h = fig.get_size_inches()
        print(f"Latency figure saved: {latency_path} ({int(w*dpi)}x{int(h*dpi)}px @ {dpi} DPI)")
    except Exception as exc:
        print(f"ERROR: Failed to save latency figure: {exc}")
        return False
    finally:
        plt.close(fig)

    return True


def main():
    parser = argparse.ArgumentParser(description="Visualize progressive YCSB hotspot benchmark")
    parser.add_argument("--input", "-i", required=True, help="progressive_summary.csv")
    parser.add_argument("--output", "-o", required=True, help="output PNG")
    parser.add_argument("--label", default="Redis-NUMA", help="label for primary input")
    parser.add_argument("--compare-input", help="second progressive_summary.csv")
    parser.add_argument("--compare-label", default="Vanilla Redis", help="label for compare input")
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

    ok = plot(datasets, args.output, args.title, args.dpi)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
