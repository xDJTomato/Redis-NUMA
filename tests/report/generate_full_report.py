#!/usr/bin/env python3
"""generate_full_report.py - aggregate run_full_validation.sh's step
results (summary.json) plus NUMAflow bench_*.json into one self-contained
HTML report with inline SVG charts. Pure standard library, same approach
as numaflow/eval/report.py.

Usage: generate_full_report.py <report_dir> <summary_json> <results_dir>
"""
import json, glob, os, sys, html

STRATEGIES = ["noop", "composite_lru", "tinylfu", "caat"]
COLORS = {"noop": "#9e9e9e", "composite_lru": "#f4a261", "tinylfu": "#457b9d", "caat": "#2a9d8f"}
LABELS = {"noop": "Baseline (noop)", "composite_lru": "Composite LRU", "tinylfu": "TinyLFU", "caat": "CAAT (new)"}
STATUS_COLOR = {"passed": "#2a9d8f", "partial": "#f4a261", "timeout": "#f4a261",
                "failed": "#e63946", "skipped": "#9e9e9e", "unknown": "#9e9e9e"}


def bar_chart(title, ylabel, data, higher_better):
    w = 760; h = 340; ml = 70; mr = 20; mt = 44; mb = 66
    workloads = list(data.keys())
    if not workloads:
        return ""
    n_w = len(workloads); n_s = len(STRATEGIES)
    plot_w = w - ml - mr; plot_h = h - mt - mb
    group_w = plot_w / n_w; bar_w = group_w * 0.8 / n_s
    allvals = [v for vals in data.values() for v in vals]
    vmax = max(allvals) if allvals else 1
    vmax = vmax or 1
    def y(v): return mt + plot_h - v / vmax * plot_h
    s = [f'<svg width="{w}" height="{h}" xmlns="http://www.w3.org/2000/svg">',
         f'<rect width="{w}" height="{h}" fill="var(--card-bg)"/>',
         f'<text x="{w/2}" y="24" text-anchor="middle" font-family="Inter,Segoe UI,Arial" '
         f'font-size="15" font-weight="600" fill="var(--fg)">{html.escape(title)}</text>']
    for i in range(5):
        v = vmax * i / 4
        yy = y(v)
        s.append(f'<line x1="{ml}" y1="{yy:.1f}" x2="{w-mr}" y2="{yy:.1f}" stroke="var(--grid)"/>')
        s.append(f'<text x="{ml-8}" y="{yy+4:.1f}" text-anchor="end" font-family="Inter,Segoe UI,Arial" font-size="10" fill="var(--muted)">{v:,.0f}</text>')
    for wi, wl in enumerate(workloads):
        gx = ml + wi * group_w
        for si in range(n_s):
            v = data[wl][si]
            x = gx + si * bar_w + group_w * 0.1
            yy = y(v); bh = mt + plot_h - yy
            s.append(f'<rect x="{x:.1f}" y="{yy:.1f}" width="{bar_w:.1f}" height="{bh:.1f}" fill="{COLORS[STRATEGIES[si]]}" rx="2"/>')
        s.append(f'<text x="{gx+group_w/2:.1f}" y="{h-mb+20:.1f}" text-anchor="middle" font-family="Inter,Segoe UI,Arial" font-size="11" fill="var(--fg)">{html.escape(wl)}</text>')
    lx = ml
    for si in range(n_s):
        s.append(f'<rect x="{lx}" y="{h-20}" width="11" height="11" fill="{COLORS[STRATEGIES[si]]}" rx="2"/>')
        s.append(f'<text x="{lx+15}" y="{h-10}" font-family="Inter,Segoe UI,Arial" font-size="10.5" fill="var(--fg)">{LABELS[STRATEGIES[si]]}</text>')
        lx += 15 + len(LABELS[STRATEGIES[si]]) * 6.4 + 20
    s.append('</svg>')
    return "".join(s)


def native_bench_chart(native_bench):
    """native_bench: list of {"name","avg_latency_ns","bandwidth_penalty_ns","dram_baseline_ns"}."""
    if not native_bench:
        return ""
    w = 760; h = 340; ml = 70; mr = 20; mt = 44; mb = 66
    names = [r["name"] for r in native_bench]
    lat = [r["avg_latency_ns"] for r in native_bench]
    dram = native_bench[0].get("dram_baseline_ns", 0)
    n_w = len(names)
    plot_w = w - ml - mr; plot_h = h - mt - mb
    group_w = plot_w / n_w; bar_w = group_w * 0.5
    vmax = max(lat + [dram]) or 1
    def y(v): return mt + plot_h - v / vmax * plot_h
    s = [f'<svg width="{w}" height="{h}" xmlns="http://www.w3.org/2000/svg">',
         f'<rect width="{w}" height="{h}" fill="var(--card-bg)"/>',
         f'<text x="{w/2}" y="24" text-anchor="middle" font-family="Inter,Segoe UI,Arial" '
         f'font-size="15" font-weight="600" fill="var(--fg)">CXLMemSim native model: avg latency per access (ns)</text>']
    for i in range(5):
        v = vmax * i / 4
        yy = y(v)
        s.append(f'<line x1="{ml}" y1="{yy:.1f}" x2="{w-mr}" y2="{yy:.1f}" stroke="var(--grid)"/>')
        s.append(f'<text x="{ml-8}" y="{yy+4:.1f}" text-anchor="end" font-family="Inter,Segoe UI,Arial" font-size="10" fill="var(--muted)">{v:,.0f}</text>')
    dy = y(dram)
    s.append(f'<line x1="{ml}" y1="{dy:.1f}" x2="{w-mr}" y2="{dy:.1f}" stroke="#e63946" stroke-dasharray="4,3"/>')
    s.append(f'<text x="{w-mr}" y="{dy-4:.1f}" text-anchor="end" font-family="Inter,Segoe UI,Arial" font-size="10" fill="#e63946">DRAM baseline ({dram:.0f}ns)</text>')
    for wi, name in enumerate(names):
        gx = ml + wi * group_w
        v = lat[wi]
        x = gx + group_w * 0.25
        yy = y(v); bh = mt + plot_h - yy
        s.append(f'<rect x="{x:.1f}" y="{yy:.1f}" width="{bar_w:.1f}" height="{bh:.1f}" fill="#2a9d8f" rx="2"/>')
        s.append(f'<text x="{x+bar_w/2:.1f}" y="{yy-6:.1f}" text-anchor="middle" font-family="Inter,Segoe UI,Arial" font-size="10.5" fill="var(--fg)">{v:.1f}</text>')
        s.append(f'<text x="{gx+group_w/2:.1f}" y="{h-mb+20:.1f}" text-anchor="middle" font-family="Inter,Segoe UI,Arial" font-size="11" fill="var(--fg)">{html.escape(name)}</text>')
    s.append('</svg>')
    return "".join(s)


def load_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def bench_dict(results_dir, suffix=""):
    """Load results/bench_<workload><suffix>.json into {workload: [net_cost_by_strategy]} + hit ratios."""
    pattern = os.path.join(results_dir, f"bench_*{suffix}.json") if suffix else os.path.join(results_dir, "bench_*.json")
    hit_data, net_data = {}, {}
    for f in sorted(glob.glob(pattern)):
        base = os.path.basename(f)[6:-5]  # strip "bench_" and ".json"
        if suffix:
            if not base.endswith(suffix):
                continue
            wl = base[: -len(suffix)]
        else:
            if base.endswith("_cxlcal"):
                continue
            wl = base
        b = load_json(f)
        if not b or "migration" not in b:
            continue
        mig = {m["strategy"]: m for m in b["migration"]}
        if not all(s in mig for s in STRATEGIES):
            continue
        hit_data[wl] = [mig[s]["local_hit_ratio"] * 100 for s in STRATEGIES]
        net_data[wl] = [mig[s]["net_cost"] / 1e6 for s in STRATEGIES]
    return hit_data, net_data


def main():
    if len(sys.argv) != 4:
        print("usage: generate_full_report.py <report_dir> <summary_json> <results_dir>", file=sys.stderr)
        sys.exit(1)
    report_dir, summary_path, results_dir = sys.argv[1:4]

    summary = load_json(summary_path) or {}

    hit_data, net_data = bench_dict(results_dir)
    hit_cal, net_cal = bench_dict(results_dir, suffix="_cxlcal")

    charts = bar_chart("Local Hit Ratio (%)", "hit ratio %", hit_data, True) + \
             bar_chart("Net Cost (millions of ns, lower is better)", "net cost", net_data, False)
    cal_charts = bar_chart("Net Cost, CXLMemSim-calibrated tier (125ns / 25000 MB/s)", "net cost", net_cal, False)

    cxl_root = os.path.join(results_dir, "..", "tests", "cxl", "results")
    native_files = sorted(glob.glob(os.path.join(cxl_root, "cxlmemsim_native_bench_*.json")))
    native_bench = None
    if native_files:
        d = load_json(native_files[-1])
        if d and "workloads" in d:
            native_bench = d["workloads"]
    native_chart = native_bench_chart(native_bench) if native_bench else ""

    step_order = ["build", "unit_tests", "numaflow_bench", "ycsb", "qemu_vm", "cxlmemsim"]
    step_labels = {"build": "Build (make)", "unit_tests": "Unit tests (make test)",
                   "numaflow_bench": "NUMAflow benchmark", "ycsb": "YCSB bandwidth benchmark",
                   "qemu_vm": "QEMU multi-NUMA-node smoke test", "cxlmemsim": "CXLMemSim device link"}
    rows = []
    for k in step_order:
        if k not in summary:
            continue
        st = summary[k].get("status", "unknown")
        detail = summary[k].get("detail", "")
        color = STATUS_COLOR.get(st, "#9e9e9e")
        rows.append(
            f'<tr><td>{html.escape(step_labels.get(k, k))}</td>'
            f'<td><span class="pill" style="background:{color}22;color:{color};border:1px solid {color}55">{html.escape(st)}</span></td>'
            f'<td class="detail">{html.escape(str(detail))}</td></tr>'
        )

    css = """
:root{
  --bg:#f5f6f8; --card-bg:#ffffff; --fg:#1c2128; --muted:#6b7280; --grid:#e5e7eb;
  --accent:#2a9d8f; --border:#e5e7eb;
}
@media (prefers-color-scheme: dark){
  :root:not([data-theme="light"]){
    --bg:#0f1115; --card-bg:#181b21; --fg:#e6e8eb; --muted:#9aa1ac; --grid:#2a2e37;
    --accent:#4fd1c5; --border:#2a2e37;
  }
}
:root[data-theme="dark"]{
  --bg:#0f1115; --card-bg:#181b21; --fg:#e6e8eb; --muted:#9aa1ac; --grid:#2a2e37;
  --accent:#4fd1c5; --border:#2a2e37;
}
*{box-sizing:border-box}
body{font-family:'Inter','Segoe UI',Arial,sans-serif;margin:0;background:var(--bg);color:var(--fg)}
.wrap{max-width:900px;margin:0 auto;padding:32px 20px 60px}
h1{font-size:26px;margin:0 0 6px;letter-spacing:-0.01em}
h2{font-size:17px;margin:32px 0 10px;color:var(--fg)}
p.muted{color:var(--muted);font-size:13.5px;margin:4px 0 0}
.card{background:var(--card-bg);border:1px solid var(--border);border-radius:12px;padding:18px;margin:12px 0;
      box-shadow:0 1px 3px rgba(0,0,0,.06);overflow-x:auto}
table{border-collapse:collapse;width:100%;font-size:13.5px}
th,td{border-bottom:1px solid var(--border);padding:9px 10px;text-align:left;vertical-align:top}
th{color:var(--muted);font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:.03em}
td.detail{color:var(--muted);font-family:ui-monospace,Consolas,monospace;font-size:12px}
.pill{display:inline-block;padding:2px 10px;border-radius:999px;font-size:12px;font-weight:600}
"""

    out_html = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Redis-NUMA Validation Report</title>
<style>{css}</style></head>
<body><div class="wrap">
<h1>Redis-NUMA &mdash; Full Validation Report</h1>
<p class="muted">Generated by run_full_validation.sh. Steps that could not run in this environment
are marked <b>skipped</b> with the reason, never fabricated.</p>
<h2>Pipeline steps</h2>
<div class="card"><table><thead><tr><th>Step</th><th>Status</th><th>Detail</th></tr></thead>
<tbody>{"".join(rows)}</tbody></table></div>
<h2>NUMAflow scheduling-strategy benchmark</h2>
<div class="card">{charts if charts else '<p class="muted">no bench_*.json found under results/</p>'}</div>
<h2>NUMAflow model calibrated against a real CXLMemSim device-link run</h2>
<p class="muted">Same 4 workloads x 4 strategies, but the non-DRAM tier's latency/bandwidth are overridden
with values captured from an actual CXLMemSim device-link check (~125ns blended latency, 25000 MB/s)
instead of numa_shim.c's synthetic tier-1 defaults (300ns / 8000 MB/s).</p>
<div class="card">{cal_charts if cal_charts else '<p class="muted">no bench_*_cxlcal.json found -- run run_full_validation.sh (not --quick) to generate them</p>'}</div>
<h2>CXLMemSim's own device-timing model on the same workloads</h2>
<p class="muted">Instead of NUMAflow's simplified flat latency/bandwidth cost model, this drives
the same four workload traces directly through CXLMemSim's C++ CXLMemExpander
(calculate_latency/calculate_bandwidth), so it reflects CXLMemSim's own congestion-aware
bandwidth model and load/store-ratio-driven latency, not a hand-copied constant.
See tests/cxl/cxlmemsim_workload_bench.cpp.</p>
<div class="card">{native_chart if native_chart else '<p class="muted">no cxlmemsim_native_bench_*.json found -- run tests/cxl/run_cxlmemsim.sh</p>'}</div>
</div></body></html>"""

    out_path = os.path.join(report_dir, "index.html")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(out_html)
    print("wrote", out_path)


if __name__ == "__main__":
    main()
