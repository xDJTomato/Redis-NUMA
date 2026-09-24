#!/usr/bin/env python3
"""Verify all published NUMAflow results have complete, consistent measurements."""
import json
import math
from pathlib import Path

DIRECTORY = Path(__file__).resolve().parent
WORKLOADS = ("zipf", "uniform", "hotspot", "temporal")
STRATEGIES = {"noop", "composite_lru", "tinylfu", "caat"}
for model in ("default", "cxlcal"):
    for workload in WORKLOADS:
        path = DIRECTORY / f"bench_{workload}_{model}.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        cfg = data["config"]
        assert cfg == dict(workload=workload, keys=20000, accesses=200000,
                           epoch=5000, budget=64, nodes=2, seed=20240517), path
        topo = data["topology"]
        assert len(topo) == 2 and topo[0]["latency_ns"] == 60
        assert topo[1]["latency_ns"] == (300 if model == "default" else 125)
        assert topo[1]["bandwidth_mbps"] == (8000 if model == "default" else 25000)
        assert {m["strategy"] for m in data["migration"]} == STRATEGIES
        assert len(data["allocation"]) == 9, path
        assert len({a["strategy"] for a in data["allocation"]}) == 9, path
        for item in data["migration"] + data["allocation"]:
            for key in ("access_cost", "migration_cost", "net_cost", "local_hit_ratio"):
                assert math.isfinite(item[key]) and item[key] >= 0, (path, key)
            assert abs(item["net_cost"] - item["access_cost"] - item["migration_cost"]) < 0.01
            assert 0 <= item["local_hit_ratio"] <= 1 and item["migrations"] >= 0
        print(f"verified {path.name}: 4 migration + 9 allocation policies")
report = (DIRECTORY / "report.html").read_text(encoding="utf-8")
assert report.count("<svg") == 3 and all(w in report for w in WORKLOADS), "incomplete full HTML charts"
for model in ("default", "cxlcal"):
    for workload in WORKLOADS:
        data = json.loads((DIRECTORY / f"bench_{workload}_{model}.json").read_text())
        migration = {m["strategy"]: m["net_cost"] for m in data["migration"]}
        baseline = min(migration[name] for name in STRATEGIES if name != "caat")
        delta = (migration["caat"] - baseline) / baseline * 100
        assert f">{delta:+.1f}%</td>" in report, (workload, model, "report sign")
for workload in WORKLOADS:
    path = DIRECTORY / "reference" / f"bench_{workload}.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    cfg = data["config"]
    assert cfg == dict(workload=workload, keys=3000, accesses=120000,
                       epoch=3000, budget=256, nodes=2, seed=20240517), path
    assert {m["strategy"] for m in data["migration"]} == STRATEGIES
    assert len(data["allocation"]) == 9, path
    print(f"verified reference {path.name}: 4 migration + 9 allocation policies")
ref_report = (DIRECTORY / "reference" / "report.html").read_text(encoding="utf-8")
assert ref_report.count("<svg") == 3 and all(w in ref_report for w in WORKLOADS), "incomplete reference charts"
for workload in WORKLOADS:
    data = json.loads((DIRECTORY / "reference" / f"bench_{workload}.json").read_text())
    migration = {m["strategy"]: m["net_cost"] for m in data["migration"]}
    baseline = min(migration[name] for name in STRATEGIES if name != "caat")
    delta = (migration["caat"] - baseline) / baseline * 100
    assert f">{delta:+.1f}%</td>" in ref_report, (workload, "reference sign")
print("All 12 benchmark files and both reports validated")
