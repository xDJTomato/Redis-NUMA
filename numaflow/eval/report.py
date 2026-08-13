#!/usr/bin/env python3
"""report.py - generate a self-contained HTML report with SVG charts from
NUMAflow benchmark JSON results.  Pure standard library (no matplotlib), so it
runs anywhere Python 3 is available."""
import json, glob, os, sys

STRATEGIES = ["noop", "composite_lru", "tinylfu", "caat"]
COLORS = {"noop": "#9e9e9e", "composite_lru": "#f4a261", "tinylfu": "#457b9d", "caat": "#2a9d8f"}
LABELS = {"noop": "Baseline (noop)", "composite_lru": "Composite LRU", "tinylfu": "TinyLFU", "caat": "CAAT (new)"}

def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)

def bar_chart(title, ylabel, data, fmt, higher_better):
    """data: dict workload -> list of values aligned with STRATEGIES."""
    w = 760; h = 360; ml = 70; mr = 20; mt = 48; mb = 70
    workloads = list(data.keys())
    n_w = len(workloads); n_s = len(STRATEGIES)
    plot_w = w - ml - mr; plot_h = h - mt - mb
    group_w = plot_w / n_w; bar_w = group_w * 0.8 / n_s
    allvals = [v for vals in data.values() for v in vals]
    vmax = max(allvals) if allvals else 1; vmin = 0
    if vmax == 0: vmax = 1
    def y(v): return mt + plot_h - (v - vmin) / (vmax - vmin) * plot_h
    s = []
    s.append(f'<svg width="{w}" height="{h}" xmlns="http://www.w3.org/2000/svg">')
    s.append(f'<rect width="{w}" height="{h}" fill="#fdfdfd"/>')
    s.append(f'<text x="{w/2}" y="26" text-anchor="middle" font-family="Segoe UI,Arial" font-size="17" font-weight="600" fill="#222">{title}</text>')
    # y grid + axis labels
    for i in range(5):
        v = vmin + (vmax - vmin) * i / 4
        yy = y(v)
        s.append(f'<line x1="{ml}" y1="{yy:.1f}" x2="{w-mr}" y2="{yy:.1f}" stroke="#e0e0e0"/>')
        s.append(f'<text x="{ml-8}" y="{yy+4:.1f}" text-anchor="end" font-family="Segoe UI,Arial" font-size="11" fill="#666">{v:,.0f}</text>')
    # bars
    for wi, wl in enumerate(workloads):
        gx = ml + wi * group_w
        for si in range(n_s):
            v = data[wl][si]
            x = gx + si * bar_w + group_w * 0.1
            yy = y(v); bh = mt + plot_h - yy
            c = COLORS[STRATEGIES[si]]
            s.append(f'<rect x="{x:.1f}" y="{yy:.1f}" width="{bar_w:.1f}" height="{bh:.1f}" fill="{c}" rx="2"/>')
            label = f'{v:,.0f}' if fmt == 'int' else f'{v:.2f}'
            s.append(f'<text x="{x+bar_w/2:.1f}" y="{yy-4:.1f}" text-anchor="middle" font-family="Segoe UI,Arial" font-size="8.5" fill="#333">{label}</text>')
        s.append(f'<text x="{gx+group_w/2:.1f}" y="{h-mb+20:.1f}" text-anchor="middle" font-family="Segoe UI,Arial" font-size="12" fill="#333">{wl}</text>')
    s.append(f'<text x="{ml/2}" y="{h/2}" text-anchor="middle" font-family="Segoe UI,Arial" font-size="12" fill="#333" transform="rotate(-90 {ml/2} {h/2})">{ylabel}</text>')
    # legend
    lx = ml
    for si in range(n_s):
        s.append(f'<rect x="{lx}" y="{h-22}" width="12" height="12" fill="{COLORS[STRATEGIES[si]]}" rx="2"/>')
        s.append(f'<text x="{lx+16}" y="{h-11}" font-family="Segoe UI,Arial" font-size="11" fill="#333">{LABELS[STRATEGIES[si]]}</text>')
        lx += 16 + len(LABELS[STRATEGIES[si]]) * 6.6 + 22
    s.append('</svg>')
    return "".join(s)

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    resdir = os.environ.get("NF_RESULTS", os.path.join(here, "..", "..", "results"))
    files = sorted(glob.glob(os.path.join(resdir, "bench_*.json")))
    if not files:
        print("no benchmark results found in", resdir); sys.exit(1)
    benches = [(os.path.basename(f)[6:-5], load(f)) for f in files]  # strip bench_ and .json
    workloads = [w for w, _ in benches]

    hit_data = {}; net_data = {}; mig_data = {}
    for wl, b in benches:
        mig = {m["strategy"]: m for m in b["migration"]}
        hit_data[wl] = [mig[s]["local_hit_ratio"] * 100 for s in STRATEGIES]
        net_data[wl] = [mig[s]["net_cost"] / 1e6 for s in STRATEGIES]
        mig_data[wl] = [mig[s]["migrations"] for s in STRATEGIES]

    charts = (
        bar_chart("Local Hit Ratio (%)", "hit ratio %", hit_data, "int", True) +
        bar_chart("Net Cost (millions of ns, lower is better)", "net cost", net_data, "int", False) +
        bar_chart("Migrations emitted", "migrations", mig_data, "int", False)
    )

    # summary: CAAT vs best baseline per workload
    rows = []
    for wl in workloads:
        caat = net_data[wl][3]
        best_base = min(net_data[wl][0], net_data[wl][1], net_data[wl][2])
        best_base_name = LABELS[STRATEGIES[[0,1,2][net_data[wl][:3].index(best_base)]]]
        imp = (best_base - caat) / best_base * 100
        rows.append(f'<tr><td>{wl}</td><td>{caat:,.1f}</td><td>{best_base:,.1f}</td><td>{best_base_name}</td><td style="color:#2a9d8f;font-weight:600">-{imp:.1f}%</td></tr>')

    html = f'''<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NUMAflow - Memory Scheduling Strategy Benchmark</title>
<style>
body{{font-family:'Segoe UI',Arial,sans-serif;margin:0;background:#f4f5f7;color:#222}}
.wrap{{max-width:860px;margin:24px auto;padding:0 16px}}
h1{{font-size:24px;margin:8px 0 4px}}h2{{font-size:18px;margin:28px 0 8px;color:#333}}
.card{{background:#fff;border:1px solid #e5e7eb;border-radius:10px;padding:16px;margin:12px 0;box-shadow:0 1px 2px rgba(0,0,0,.04)}}
table{{border-collapse:collapse;width:100%;font-size:13px}}
th,td{{border:1px solid #e5e7eb;padding:7px 10px;text-align:right}}
th:first-child,td:first-child{{text-align:left}}th{{background:#f9fafb}}
.muted{{color:#6b7280;font-size:13px}}
</style></head><body><div class="wrap">
<h1>NUMAflow &mdash; Memory Scheduling Strategy Benchmark</h1>
<p class="muted">Fair, QEMU-free evaluation over an emulated NUMA topology (DRAM + CXL). 
Every strategy replays the identical access trace with the same seed, budget and capacity.</p>
<div class="card">{charts}</div>
<h2>CAAT net-cost improvement vs. best baseline</h2>
<div class="card"><table><thead><tr><th>Workload</th><th>CAAT net cost</th><th>Best baseline</th><th>Baseline</th><th>Improvement</th></tr></thead><tbody>
{"".join(rows)}</tbody></table></div>
<h2>Methodology</h2>
<div class="card"><p class="muted">
The benchmark emulates a two-tier NUMA system: a small fast DRAM tier (~50% of the working set) and a
large slow CXL tier. All keys cold-start on CXL. Migration strategies promote hot keys into DRAM and
(CAAT only) demote cold keys back to CXL to keep DRAM optimally utilized. Net cost = modeled access
latency/bandwidth cost + migration cost. Lower net cost is better.</p></div>
</div></body></html>'''

    out = os.path.join(resdir, "report.html")
    with open(out, "w", encoding="utf-8") as f:
        f.write(html)
    print("wrote", out)

if __name__ == "__main__":
    main()
