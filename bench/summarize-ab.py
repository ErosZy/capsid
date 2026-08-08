#!/usr/bin/env python3
"""Summarize an A/B benchmark results dir (samples.*.jsonl) into side-by-side
old/new means plus per-round detail. Usage: python3 summarize-ab.py <results-dir>"""
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

print(f"{'side':4s} {'workload':8s} {'conn':>4s} {'rounds':>6s} {'QPS':>9s} "
      f"{'p50_ms':>8s} {'p95_ms':>8s} {'p99_ms':>8s} err")
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
    print(f"{side:4s} {workload:8s} {conn:4d} {n:6d} {qps:9.1f} {p50:8.2f} "
          f"{p95:8.2f} {p99:8.2f} {err:3d}")

print()
print("delta new vs old (%):  QPS      p50     p95     p99")
for workload, conn in ([(w, c) for w in ("json", "json16k", "json64k")
                        for c in (64,)] + [("json64k", 1)]):
    o, n = summary[("old", workload, conn)], summary[("new", workload, conn)]
    dq = (n[0] - o[0]) / o[0] * 100
    dp = tuple((b - a) / a * 100 for a, b in zip(o[1:4], n[1:4]))
    print(f"{workload:8s} c{conn:<3d} {dq:+8.1f}% {dp[0]:+7.1f}% {dp[1]:+7.1f}% "
          f"{dp[2]:+7.1f}%")

print()
print("per-round QPS:")
for workload in ("json", "json16k", "json64k"):
    line = f"{workload:8s}"
    for side in ("old", "new"):
        qs = [round(s["qps"], 1) for (k, samples) in rows.items()
              if k[1] == workload and k[2] == 64 and k[0] == side
              for s in samples]
        line += f"  {side}={qs}"
    print(line)
