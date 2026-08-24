#!/usr/bin/env python3
"""Aggregate quickjs-ng opcode-profile v3 dumps into candidate
ranking (tier-3 plan §3.4). Input: directory of *.profile.jsonl produced by
bench/profile-collect.sh. Output: per-mode tables to stdout — total exec by
opcode (cost-ranked), slow-path candidates with miss ratios, and the
arith/branch/call/prop class mix. V2/V3 also rank exact property sites for
monomorphic field-IC eligibility. V1 remains readable for archived evidence;
sequence selection uses the v3 source provenance in profile_sequences.py.

Production performance is never read from a profiling build; this only
ranks which opcode classes are worth specializing.
"""
import argparse
import collections
import glob
import json
import os

# Relative cost weights (rough dispatch + slow-path amplification) used
# only for ranking, not for any correctness claim. Fast-path entries cost
# ~1 dispatch unit; slow paths (js_*_slow, coercion, property lookup)
# cost 10..50x. Ranking is deliberately conservative: unknown costs are
# counted at their dispatch floor.
SLOW_COST = 20

def load_profiles(path):
    """Yield (mode, evidence_file, opcode_profile_dict) for every dump."""
    for f in sorted(glob.glob(os.path.join(path, "*.profile.jsonl"))):
        mode = "source" if ".source." in f else "opt"
        with open(f, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line.startswith("{"):
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if obj.get("schema") not in (
                        "quickjs-ng-opcode-profile-v1",
                        "quickjs-ng-opcode-profile-v2",
                        "quickjs-ng-opcode-profile-v3"):
                    continue
                yield mode, os.path.basename(f), obj

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", help="profile collection directory")
    ap.add_argument("--top", type=int, default=30,
                    help="rows per table (default 30)")
    args = ap.parse_args()

    # Aggregate per mode: exec counters, class mix, slow-path candidates.
    stats = {"source": collections.Counter(),
             "opt": collections.Counter()}
    classes = {"source": collections.Counter(),
               "opt": collections.Counter()}
    slow = {"source": collections.Counter(),
            "opt": collections.Counter()}
    seen_runtimes = {"source": 0, "opt": 0}
    prop_sites = {"source": [], "opt": []}
    site_overflow = {"source": 0, "opt": 0}

    # Opcodes whose handlers have both a fast and a generic slow path. For
    # these, only executions whose class bucket is a slow path enter the
    # slow-path ranking; fast-path executions count at dispatch floor.
    # Arith "other" (incl. mixed int/float after accumulator overflow) and
    # property slow buckets are the expensive class; int/float fast buckets
    # are not. Opcodes without class buckets (call, tail_call, ...) keep
    # their entire execution as candidate slow-path cost — the ranking is
    # deliberately conservative for them.
    CLASSED_SLOW = {"add", "sub", "mul", "div", "mod"}
    PROP_SLOW = {"get_field", "get_field2", "get_array_el",
                 "get_array_el2"}

    for mode, evidence_file, obj in load_profiles(args.dir):
        seen_runtimes[mode] += 1
        for op, n in obj.get("exec", {}).items():
            stats[mode][op] += n
        # Arith: only the "other" bucket is a genuine slow-path entry.
        for op, d in obj.get("arith", {}).items():
            if op in CLASSED_SLOW:
                other = d.get("other", 0)
                if other:
                    slow[mode][op] += other * SLOW_COST
        # Property v1: "slow" is the generic-path entry. V2 separates the
        # actual helper paths; array non-direct classes all enter generic
        # lookup, while field prototype-data stays on the inline lookup loop.
        for op, d in obj.get("prop", {}).items():
            if op in PROP_SLOW:
                sl = d.get("slow", 0)
                sl += d.get("accessor_or_generic", 0)
                sl += d.get("primitive_or_nullish", 0)
                if op in ("get_array_el", "get_array_el2"):
                    sl += d.get("prototype_or_int_fallback", 0)
                    sl += d.get("missing_or_key_fallback", 0)
                if sl:
                    slow[mode][op] += sl * SLOW_COST
        # Calls and unclassified opcodes: conservative dispatch-floor cost.
        for op, n in obj.get("exec", {}).items():
            if op in ("call", "call_method", "call_constructor",
                      "tail_call", "tail_call_method"):
                slow[mode][op] += n * SLOW_COST
        for op, d in obj.get("arith", {}).items():
            for cls, n in d.items():
                classes[mode][f"arith:{op}:{cls}"] += n
        for op, d in obj.get("prop", {}).items():
            for cls, n in d.items():
                classes[mode][f"prop:{op}:{cls}"] += n
        for op, d in obj.get("branch", {}).items():
            for cls, n in d.items():
                classes[mode][f"branch:{op}:{cls}"] += n
        for op, d in obj.get("call_argc", {}).items():
            for cls, n in d.items():
                classes[mode][f"call:{op}:{cls}"] += n
        # V2 exact sites cover every opcode; property sites add `prop`
        # classes. The fallback keys keep the first v2 draft readable.
        raw_sites = obj.get("sites", obj.get("prop_sites", []))
        for site in raw_sites:
            d = site.get("prop", site.get("classes", {}))
            if not d:
                continue
            direct = int(d.get("direct", 0))
            total_site = sum(int(n) for n in d.values())
            prop_sites[mode].append((
                direct, total_site, site.get("op", "?"), evidence_file,
                int(obj.get("runtime", 0)), int(site.get("function", 0)),
                int(site.get("pc", 0))))
        site_overflow[mode] += int(
            obj.get("site_overflow", obj.get("prop_site_overflow", 0)))

    for mode in ("source", "opt"):
        total = sum(stats[mode].values())
        print(f"== {mode}: {seen_runtimes[mode]} runtimes, "
              f"{total:,} dynamic opcode executions ==")
        if total == 0:
            continue
        print(f"\n-- top {args.top} by dynamic exec (dispatch cost) --")
        for op, n in stats[mode].most_common(args.top):
            print(f"  {n:>12,}  {100.0*n/total:6.2f}%  {op}")
        print(f"\n-- slow-path ranking (dispatch + {SLOW_COST}x slow) --")
        for op, n in slow[mode].most_common(15):
            print(f"  {n:>12,}  {op}")
        print("\n-- class mix --")
        for key, n in classes[mode].most_common(25):
            if n > 0:
                print(f"  {n:>12,}  {key}")
        if prop_sites[mode]:
            print("\n-- exact property sites (direct count / total / ratio) --")
            ranked = sorted(prop_sites[mode], reverse=True)
            for direct, site_total, op, evidence_file, runtime, function, pc in ranked[:args.top]:
                ratio = 100.0 * direct / site_total if site_total else 0.0
                eligible = " IC" if (op == "get_field" and direct >= 128 and
                                      ratio >= 95.0) else ""
                print(f"  {direct:>12,} / {site_total:>12,}  {ratio:6.2f}%  "
                      f"{op} {evidence_file}:r{runtime}:f{function}:pc{pc}{eligible}")
            print(f"  site_table_overflow={site_overflow[mode]}")

if __name__ == "__main__":
    main()
