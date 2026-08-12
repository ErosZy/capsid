#!/usr/bin/env python3
"""Dump per-round measured samples from a results dir, one line per sample.
Usage: python3 dump-rounds.py <results-dir>"""
import json
import sys
import os
import glob

out = sys.argv[1]
print(f"{'side':4s} {'workload':8s} {'conn':>4s} {'round':>5s} {'QPS':>9s} "
      f"{'p50_ms':>8s} {'p95_ms':>8s} {'p99_ms':>8s}")
for path in sorted(glob.glob(os.path.join(out, "samples.*.jsonl"))):
    base = os.path.basename(path).replace("samples.", "").replace(".jsonl", "")
    parts = base.split(".")
    side, workload = parts[0], parts[1]
    conn = int(parts[2].lstrip("c"))
    round_ = int(parts[3].lstrip("r"))
    for line in open(path):
        s = json.loads(line)
        if s.get("phase") != "measured":
            continue
        print(f"{side:4s} {workload:8s} {conn:4d} {round_:5d} {s['qps']:9.1f} "
              f"{s['p50_ms']:8.2f} {s['p95_ms']:8.2f} {s['p99_ms']:8.2f}")
