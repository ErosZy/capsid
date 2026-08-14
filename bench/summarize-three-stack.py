#!/usr/bin/env python3
"""Summarize a three-stack comparison results dir with the doc's conclusion
gate: per (stack, workload) cell, mean QPS/latency over rounds plus CV; a
cell is a conclusion (not just an observed sample) only when CV <= 7% and
total errors + timeouts == 0.

Usage: python3 bench/summarize-three-stack.py <results-dir>
"""
import glob
import json
import os
import sys

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

if not rows:
    print("no measured samples found")
    sys.exit(1)

sizes = ("", "8k", "16k", "32k")
types = ("json", "bytes", "stream")

print("## per-cell table (mean over rounds)\n")
print("| stack | workload | QPS | p50 | p95 | p99 | CV% | err | verdict |")
print("|-------|----------|-----|-----|-----|-----|-----|-----|---------|")
summary = {}
for (side, workload, conn), samples in sorted(rows.items()):
    n = len(samples)
    qps = sum(s["qps"] for s in samples) / n
    p50 = sum(s["p50_ms"] for s in samples) / n
    p95 = sum(s["p95_ms"] for s in samples) / n
    p99 = sum(s["p99_ms"] for s in samples) / n
    err = sum(s["errors"] for s in samples) + sum(s["timeouts"] for s in samples)
    var = sum((s["qps"] - qps) ** 2 for s in samples) / n if n > 1 else 0.0
    cv = (var ** 0.5) / qps * 100 if qps else float("inf")
    verdict = "ok" if (cv <= 7.0 and err == 0) else "sample"
    summary[(side, workload, conn)] = (qps, p50, p95, p99, cv, err, verdict)
    print(f"| {side:8s} | {workload:8s} | {qps:7.0f} | {p50:5.1f} | "
          f"{p95:5.1f} | {p99:5.1f} | {cv:5.1f} | {err:3d} | {verdict} |")

print("\n## type x size matrix (QPS, capsid / php / python)\n")
matrix_conn = sorted({c for (_, _, c) in summary})[0]
print(f"(conns = {matrix_conn})\n")
for t in types:
    print(f"### {t}")
    print("| size | capsid | php | python |")
    print("|------|--------|-----|--------|")
    for size in sizes:
        workload = t + size
        cells = []
        for stack in ("capsid", "php", "python"):
            if (stack, workload, matrix_conn) in summary:
                qps, _, _, _, _, _, v = summary[(stack, workload, matrix_conn)]
                cells.append(f"{qps:.0f}" + ("" if v == "ok" else "*"))
            else:
                cells.append("-")
        print(f"| {size or '1k':>3s} | {cells[0]:>6s} | {cells[1]:>4s} | "
              f"{cells[2]:>6s} |")

print("\n* = observed sample only (CV > 7% or errors/timeouts > 0)\n")

ok_cells = [(s, w) for (s, w, c), v in summary.items() if v[6] == "ok"]
print(f"cells with conclusion-grade data: {len(ok_cells)}/"
      f"{len(summary)}")
