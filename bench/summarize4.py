#!/usr/bin/env python3
"""Summarize a four-stack comparison results dir: per-stack mean QPS/latency
per workload+conn, plus a QPS ranking table. Usage:
python3 summarize4.py <results-dir>"""
import json
import sys
import os
import glob

out = sys.argv[1]
rows = {}
for path in sorted(glob.glob(os.path.join(out, "samples.*.jsonl"))):
    base = os.path.basename(path).replace("samples.", "").replace(".jsonl", "")
    parts = base.split(".")
    side, workload = parts[0], parts[1]
    conn = int(parts[2].lstrip("c"))
    for line in open(path):
        s = json.loads(line)
        if s.get("phase") != "measured":
            continue
        rows.setdefault((side, workload, conn), []).append(s)

print(f"{'stack':8s} {'workload':8s} {'conn':>4s} {'QPS':>9s} {'p50_ms':>8s} "
      f"{'p95_ms':>8s} {'p99_ms':>8s} err")
summary = {}
for key, samples in sorted(rows.items()):
    side, workload, conn = key
    n = len(samples)
    qps = sum(s["qps"] for s in samples) / n
    p50 = sum(s["p50_ms"] for s in samples) / n
    p95 = sum(s["p95_ms"] for s in samples) / n
    p99 = sum(s["p99_ms"] for s in samples) / n
    err = sum(s["errors"] for s in samples)
    summary[key] = (qps, p50, p95, p99, err)
    print(f"{side:8s} {workload:8s} {conn:4d} {qps:9.1f} {p50:8.2f} {p95:8.2f} "
          f"{p99:8.2f} {err:3d}")

print()
print("QPS ranking (per workload × conn):")
workloads = sorted({w for (_, w, _) in summary})
conns = sorted({c for (_, _, c) in summary})
stacks = sorted({s for (s, _, _) in summary})
for workload in workloads:
    for conn in conns:
        ranked = sorted(((summary[(s, workload, conn)][0], s) for s in stacks
                         if (s, workload, conn) in summary),
                        reverse=True)
        desc = " | ".join(f"{s}: {qps:.0f}" for qps, s in ranked)
        print(f"  {workload:8s} c{conn:<3d} {desc}")
