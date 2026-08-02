#!/usr/bin/env python3
"""Generates manifest.json and report.md for a benchmark run directory.

The runner collects raw data under $OUT/.tmp/ (meta.env, samples lines,
correctness lines, ready lines, resource snapshots, per-round logs and
perf-stat files) and this script derives the fixed output layout:
manifest.json, report.md. report.md is a derived view only — it is rebuilt
from the raw files, never hand-edited.
"""

import hashlib
import json
import os
import re
import subprocess
import sys

META_KEYS = [
    "RUN_ID", "GENERATED_AT", "COMMIT", "BUILD_ARGS", "COMMAND_ARGS",
    "BASELINE_CMD", "CANDIDATE_CMD", "WORKER_CMD", "WORKER_SHA",
    "BUNDLE_CMD", "BUNDLE_SHA", "LOADGEN_CMD", "LOADGEN_SHA",
    "HOST_BIN_CMD", "HOST_BIN_SHA", "WORKLOAD", "ROUNDS", "WARMUP",
    "DURATION", "CONNECTIONS", "INFLIGHT", "CPUSET", "APP", "AUTHORITY",
    "TIMEOUT_MS", "WINDOW", "TCP_NODELAY", "TEST_MODE", "UNAME", "NPROC",
    "CGROUP_CPU_MAX", "PERF_PARANOID", "BASELINE_ENV", "CANDIDATE_ENV",
    "STATISTIC", "REQUIRE_IPC_COUNTERS", "BASELINE_HOST_PROFILE",
    "IPC_MECHANISM_BASELINE", "IPC_MECHANISM_CANDIDATE",
]

REQUIRED_FILES = [
    "samples.jsonl", "correctness.json", "baseline-gateway.pprof",
    "baseline-worker.perf.data", "candidate-host.perf.data",
    "candidate-worker.perf.data", "report.md",
]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_meta(path):
    meta = {}
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line:
                continue
            key, _, value = line.partition("=")
            meta[key] = value
    return meta


def read_jsonl(path):
    entries = []
    if not os.path.exists(path):
        return entries
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                entries.append(json.loads(line))
    return entries


def read_json(path, default):
    if not os.path.exists(path):
        return default
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def parse_perf_stat(path):
    """Extracts task-clock / context-switches / cpu-migrations /
    page-faults (normalizing msec vs µs) and flags unsupported counters."""
    result = {"cpu_ms": 0.0, "context_switches": 0, "cpu_migrations": 0,
              "page_faults": 0, "unsupported": []}
    if not os.path.exists(path):
        return result
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if "<not supported>" in line:
                result["unsupported"].append(line.strip())
            match = re.search(r"([\d,.]+)\s+(msec|µs)\s+task-clock", line)
            if match:
                value = float(match.group(1).replace(",", ""))
                result["cpu_ms"] = value / 1000.0 if match.group(2) == "µs" else value
            for key, pattern in [
                ("context_switches", r"([\d,]+)\s+context-switches"),
                ("cpu_migrations", r"([\d,]+)\s+cpu-migrations"),
                ("page_faults", r"([\d,]+)\s+page-faults"),
            ]:
                match = re.search(pattern, line)
                if match:
                    result[key] = int(match.group(1).replace(",", ""))
    return result


def profile_summary(out, label, path):
    """Derives the dominant-stack text for one profile file."""
    lines = [f"### {label}"]
    if path.endswith(".pprof"):
        if not os.path.exists(path) or os.path.getsize(path) == 0:
            lines.append("(empty)")
            return lines
        run = subprocess.run(
            ["go", "tool", "pprof", "-top", "-nodecount=8", path],
            capture_output=True, text=True, timeout=120)
        top = run.stdout.splitlines()
        # Skip the pprof header block, keep the table.
        started = False
        for line in top:
            if re.match(r"^\s+flat\s+flat%", line):
                started = True
            if started:
                lines.append(line[:140])
            if len([l for l in lines if l.startswith("     ")]) >= 10:
                break
    else:
        if not os.path.exists(path) or os.path.getsize(path) == 0:
            lines.append("(empty)")
            return lines
        # --no-call-graph: the flat symbol table does not need the dwarf
        # call chains, and resolving them spawns addr2line children that can
        # linger after the run and steal CPU from the next one.
        run = subprocess.run(
            ["perf", "report", "--stdio", "--no-children", "--no-call-graph",
             "-i", path, "--sort", "overhead,symbol"],
            capture_output=True, text=True, timeout=120)
        count = 0
        # Flat overhead lines render as "  <percent>%  <command> ..." — the
        # percent follows the first number directly (the old pattern
        # required a second number before the %, so it never matched and
        # every profile was reported as "no symbols resolved").
        for line in run.stdout.splitlines():
            if re.match(r"^\s+[\d.]+%", line):
                lines.append(line.strip()[:140])
                count += 1
                if count >= 8:
                    break
        if count == 0:
            lines.append("(no symbols resolved)")
    return lines


def build_manifest(out, meta, evidence_status, incomplete_reasons):
    components = {
        "baseline": {"cmd": meta.get("BASELINE_CMD", ""),
                     "sha256": sha256(meta.get("BASELINE_CMD", "")) if os.path.exists(meta.get("BASELINE_CMD", "")) else ""},
        "candidate": {"cmd": meta.get("CANDIDATE_CMD", ""),
                      "sha256": sha256(meta.get("CANDIDATE_CMD", "")) if os.path.exists(meta.get("CANDIDATE_CMD", "")) else ""},
        "worker": {"cmd": meta.get("WORKER_CMD", ""), "sha256": meta.get("WORKER_SHA", "")},
        "bundle": {"cmd": meta.get("BUNDLE_CMD", ""), "sha256": meta.get("BUNDLE_SHA", "")},
        "loadgen": {"cmd": meta.get("LOADGEN_CMD", ""), "sha256": meta.get("LOADGEN_SHA", "")},
    }
    if meta.get("HOST_BIN_CMD"):
        components["host_bin"] = {
            "cmd": meta.get("HOST_BIN_CMD", ""),
            "sha256": meta.get("HOST_BIN_SHA", ""),
        }
    try:
        command_args = json.loads(meta.get("COMMAND_ARGS", "[]"))
    except json.JSONDecodeError:
        command_args = []
    # Measured-window IPC mechanism counters, per side (from the profile-run
    # loadgen window; see run-ab.sh). Absent sides record {}.
    ipc_mechanism = {}
    for side in ("baseline", "candidate"):
        try:
            ipc_mechanism[side] = json.loads(
                meta.get("IPC_MECHANISM_" + side.upper(), "{}"))
        except json.JSONDecodeError:
            ipc_mechanism[side] = {}
    mechanism_keys = set()
    for side in ipc_mechanism.values():
        mechanism_keys.update(side.get("counters", {}).keys())
    for side in ipc_mechanism.values():
        for key in mechanism_keys:
            side.setdefault("counters", {}).setdefault(key, 0)
    test_mode = meta.get("TEST_MODE", "0") == "1"
    if test_mode and evidence_status == "complete":
        evidence_status = "diagnostic"
    manifest = {
        "schema": "capsid-bench-manifest-v1",
        "run_id": meta.get("RUN_ID", ""),
        "generated_at": meta.get("GENERATED_AT", ""),
        "commit": meta.get("COMMIT", ""),
        "build_args": meta.get("BUILD_ARGS", ""),
        "components": components,
        "params": {
            "workload": meta.get("WORKLOAD", ""),
            "rounds": int(meta.get("ROUNDS", 0) or 0),
            "warmup_s": int(meta.get("WARMUP", 0) or 0),
            "duration_s": int(meta.get("DURATION", 0) or 0),
            "connections": int(meta.get("CONNECTIONS", 0) or 0),
            "inflight": int(meta.get("INFLIGHT", 0) or 0),
            "cpuset": meta.get("CPUSET", "none") or "none",
            "application": meta.get("APP", ""),
            "public_authority": meta.get("AUTHORITY", ""),
            "request_timeout_ms": int(meta.get("TIMEOUT_MS", 0) or 0),
            "initial_stream_window": int(meta.get("WINDOW", 0) or 0),
            "tcp_nodelay": meta.get("TCP_NODELAY", "off"),
            "statistic": meta.get("STATISTIC", "mean"),
            "baseline_env": meta.get("BASELINE_ENV", ""),
            "candidate_env": meta.get("CANDIDATE_ENV", ""),
            "require_ipc_counters": meta.get("REQUIRE_IPC_COUNTERS", "0") == "1",
            "baseline_host_profile": meta.get("BASELINE_HOST_PROFILE", "0") == "1",
            "profile_runs": True,
        },
        "environment": {
            "uname": meta.get("UNAME", ""),
            "nproc": int(meta.get("NPROC", 0) or 0),
            "cgroup_cpu_max": meta.get("CGROUP_CPU_MAX", "n/a"),
            "perf_event_paranoid": meta.get("PERF_PARANOID", "n/a"),
        },
        "resource": read_json(os.path.join(out, ".tmp", "resource.json"), {}),
        "ipc_mechanism": ipc_mechanism,
        "unsupported_counters": meta.get("UNSUPPORTED_COUNTERS", ""),
        "test_mode": test_mode,
        "command": command_args,
        "evidence_status": evidence_status,
        "incomplete_reasons": incomplete_reasons,
        "files": {},
    }
    for name in REQUIRED_FILES:
        path = os.path.join(out, name)
        if os.path.exists(path):
            manifest["files"][name] = sha256(path)
    # Perf-recorded baseline gateway (--baseline-host-profile) is not a
    # REQUIRED_FILE (the Go baseline writes a pprof instead); hash it when
    # the run produced it.
    baseline_gateway_perf = os.path.join(out, "baseline-gateway.perf.data")
    if os.path.exists(baseline_gateway_perf):
        manifest["files"]["baseline-gateway.perf.data"] = sha256(
            baseline_gateway_perf)
    # Perf-stat and log directory digests.
    perf_stat_dir = os.path.join(out, "perf-stat")
    if os.path.isdir(perf_stat_dir):
        manifest["files"]["perf-stat"] = {}
        for entry in sorted(os.listdir(perf_stat_dir)):
            epath = os.path.join(perf_stat_dir, entry)
            if os.path.isfile(epath):
                manifest["files"]["perf-stat"][entry] = sha256(epath)
    # Ready records and correctness.
    for fname in ("ready-records.json", "correctness.json"):
        path = os.path.join(out, fname)
        if os.path.exists(path):
            if "ready-records.json" not in manifest["files"]:
                manifest["files"][fname] = sha256(path)
    # Component log SHAs.
    tmp_dir = os.path.join(out, ".tmp")
    if os.path.isdir(tmp_dir):
        log_shas = {}
        for entry in sorted(os.listdir(tmp_dir)):
            if entry.endswith(".log"):
                log_shas[entry] = sha256(os.path.join(tmp_dir, entry))
        if log_shas:
            manifest["files"]["component_logs"] = log_shas
    return manifest


def build_report(out, meta, manifest):
    lines = []
    lines.append(f"# A/B benchmark {meta.get('RUN_ID', '')}")
    lines.append("")
    lines.append(f"- commit: {meta.get('COMMIT', '')}")
    lines.append(f"- workload: {meta.get('WORKLOAD', '')}, rounds: "
                 f"{meta.get('ROUNDS', '')}, warmup: {meta.get('WARMUP', '')}s, "
                 f"measured: {meta.get('DURATION', '')}s")
    lines.append(f"- connections: {meta.get('CONNECTIONS', '')}, inflight: "
                 f"{meta.get('INFLIGHT', '')}, cpuset: {meta.get('CPUSET', 'none') or 'none'}, "
                 f"tcp_nodelay: {meta.get('TCP_NODELAY', 'off')}")
    lines.append(f"- baseline env: {meta.get('BASELINE_ENV', '') or '(none)'}")
    lines.append(f"- candidate env: {meta.get('CANDIDATE_ENV', '') or '(none)'}")
    lines.append(f"- evidence: {manifest['evidence_status']}")
    if manifest["incomplete_reasons"]:
        lines.append(f"- incomplete reasons: {manifest['incomplete_reasons']}")
    lines.append("")
    lines.append("## Headline QPS (measured rounds)")
    lines.append("")
    lines.append("| side | round | qps | p50_ms | p95_ms | p99_ms | "
                 "dispatch_wait_ms | completed | errors | timeouts |")
    lines.append("|------|-------|-----|--------|--------|--------|"
                 "-----------------|-----------|--------|----------|")
    samples = read_jsonl(os.path.join(out, "samples.jsonl"))
    for sample in samples:
        if sample.get("phase") != "measured" or sample.get("round", 0) == 0:
            continue
        lines.append("| {} | {} | {:.2f} | {:.3f} | {:.3f} | {:.3f} | {:.3f} | "
                     "{} | {} | {} |".format(
                         sample.get("side", ""), sample.get("round", ""),
                         sample.get("qps", 0), sample.get("p50_ms", 0),
                         sample.get("p95_ms", 0), sample.get("p99_ms", 0),
                         sample.get("dispatch_wait_ms", 0),
                         sample.get("completed", 0), sample.get("errors", 0),
                         sample.get("timeouts", 0)))
    lines.append("")
    lines.append("## Verdict")
    lines.append("")
    statistic = meta.get("STATISTIC", "mean")
    rounds = int(meta.get("ROUNDS", 0) or 0)
    qps_by_side = {"baseline": [], "candidate": []}
    p50_by_side = {"baseline": [], "candidate": []}
    for sample in samples:
        if sample.get("phase") != "measured":
            continue
        side = sample.get("side", "")
        rnd = sample.get("round", 0)
        if side in qps_by_side and 1 <= rnd <= rounds:
            qps_by_side[side].append(float(sample.get("qps", 0)))
            p50_by_side[side].append(float(sample.get("p50_ms", 0)))
    import statistics
    def aggregate(values):
        values = sorted(values)
        if not values:
            return None
        if statistic == "median":
            return statistics.median(values)
        return statistics.mean(values)
    base_qps = aggregate(qps_by_side["baseline"])
    cand_qps = aggregate(qps_by_side["candidate"])
    base_p50 = aggregate(p50_by_side["baseline"])
    cand_p50 = aggregate(p50_by_side["candidate"])
    lines.append(f"- frozen statistic: {statistic} of the "
                 f"{rounds} measured rounds per side (frozen in the manifest "
                 f"before the run; the other statistic is reported only for "
                 f"context and never drives acceptance)")
    # Incomplete-sample scenarios (runner RED tests) must still produce a
    # report; n/a values only appear when the evidence gate is failing anyway.
    if base_qps is None:
        lines.append("- baseline QPS: n/a")
    else:
        lines.append(f"- baseline QPS: {base_qps:.2f}")
    if cand_qps is None:
        lines.append("- candidate QPS: n/a")
    else:
        lines.append(f"- candidate QPS: {cand_qps:.2f}")
    delta_qps = (cand_qps - base_qps) / base_qps * 100 \
        if base_qps and cand_qps is not None else 0.0
    delta_p50 = (cand_p50 - base_p50) / base_p50 * 100 \
        if base_p50 and cand_p50 is not None else 0.0
    lines.append(f"- delta QPS: {delta_qps:+.2f}%; delta p50: {delta_p50:+.2f}%")
    # Bodyless off/on A/B: both sides are the same capsid-host binary,
    # differing only in CAPSID_BODYLESS. The gate is frozen on three
    # per-request mechanism metrics (each counter divided by the completed
    # requests of its own diagnostic window): every one must drop >= 20%
    # off→on, with no QPS regression. Per-request ratios make the
    # sequential per-side diagnostic windows volume-comparable, and the
    # metrics never mix counters of different units. Otherwise the M1B gate
    # (QPS >= +5% or p50 <= -10%) applies.
    bodyless_ab = "CAPSID_BODYLESS" in meta.get("BASELINE_ENV", "") or \
        "CAPSID_BODYLESS" in meta.get("CANDIDATE_ENV", "")
    mech = manifest["ipc_mechanism"]
    mech_keys = sorted(set(mech.get("baseline", {}).get("counters", {})) |
                       set(mech.get("candidate", {}).get("counters", {})))
    # Frozen per-request mechanism metrics (frozen in evidence.py before
    # any run; the bodyless A/B verdict uses exactly these three).
    mech_gate_metrics = [
        "host.commands_submitted",   # commands the host submitted to the worker
        "client.queued_frames",      # request-direction wire frames
        "client.parsed_frames",      # worker events parsed back
    ]
    completed_by_side = {}
    for sample in samples:
        if (sample.get("phase") == "measured" and
                sample.get("round") == 0 and sample.get("side")):
            completed_by_side[sample.get("side")] = float(
                sample.get("completed") or 0)
    if bodyless_ab and all(k in mech_keys for k in mech_gate_metrics):
        gate_ok = True
        for key in mech_gate_metrics:
            base_total = mech.get("baseline", {}).get("counters", {}).get(key, 0)
            cand_total = mech.get("candidate", {}).get("counters", {}).get(key, 0)
            base_req = completed_by_side.get("baseline", 0)
            cand_req = completed_by_side.get("candidate", 0)
            if not base_total or not cand_total or not base_req or not cand_req:
                lines.append(f"- {key}/req: n/a (window counters or completed "
                             f"requests missing)")
                gate_ok = False
                continue
            base_per_req = base_total / base_req
            cand_per_req = cand_total / cand_req
            drop = (base_per_req - cand_per_req) / base_per_req * 100
            lines.append(f"- {key}/req: {base_per_req:.4f} → {cand_per_req:.4f} "
                         f"(drop {drop:.2f}%)")
            if drop < 20.0:
                gate_ok = False
        lines.append(f"- bodyless A/B (same binary, CAPSID_BODYLESS off→on): "
                     f"frozen per-request mechanism gate — all three metrics "
                     f"above must drop ≥20%, with no QPS regression")
        lines.append(f"- verdict: " + (
            "PASS" if gate_ok and delta_qps >= 0.0 else "FAIL"))
    else:
        lines.append(f"- M1B acceptance gate: QPS ≥ +5% or p50 ≤ -10%; "
                     f"verdict: " + (
            "PASS" if delta_qps >= 5.0 or delta_p50 <= -10.0 else "FAIL"))
    if mech_keys:
        lines.append("")
        lines.append("### IPC mechanism counters (measured-rounds window)")
        lines.append("")
        lines.append("| counter | baseline | candidate | drop % (positive = fewer) |")
        lines.append("|---------|----------|-----------|----------------------------|")
        for key in mech_keys:
            base = mech.get("baseline", {}).get("counters", {}).get(key, 0)
            cand = mech.get("candidate", {}).get("counters", {}).get(key, 0)
            drop = (base - cand) / base * 100 if base else 0.0
            lines.append(f"| {key} | {base} | {cand} | {drop:+.2f}% |")
        lines.append("")
    lines.append("")
    lines.append("## CPU/response and resources (profile runs)")
    lines.append("")
    lines.append("| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | "
                 "read_syscalls | write_syscalls | read_bytes | write_bytes |")
    lines.append("|------|---------|-----------------|--------------|--------------|"
                 "---------------|----------------|------------|-------------|")
    resource = manifest["resource"]
    samples_by_side_round0 = {
        (s.get("side"), s.get("round")): s
        for s in samples if s.get("phase") == "measured"
    }
    for side in ("baseline", "candidate"):
        for label in ("gateway", "worker"):
            key = f"{side}_{label}"
            entry = resource.get(key)
            stat = parse_perf_stat(os.path.join(out, "perf-stat", f"{side}-{label}.stat"))
            sample = samples_by_side_round0.get((side, 0))
            cpu_per_response = "n/a"
            if sample and sample.get("completed") and stat["cpu_ms"]:
                cpu_per_response = f"{stat['cpu_ms'] / sample['completed']:.4f}"
            if entry is None:
                continue
            lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
                side, label, cpu_per_response,
                entry.get("rss_delta_kb", 0), entry.get("pss_delta_kb", 0),
                entry.get("read_syscalls", 0), entry.get("write_syscalls", 0),
                entry.get("read_bytes", 0), entry.get("write_bytes", 0)))
    lines.append("")
    lines.append("## perf-stat summary")
    lines.append("")
    lines.append("| side | process | cpu_ms | context_switches | cpu_migrations | "
                 "page_faults | unsupported |")
    lines.append("|------|---------|--------|-----------------|----------------|"
                 "------------|-------------|")
    for side in ("baseline", "candidate"):
        for label in ("gateway", "worker"):
            stat = parse_perf_stat(os.path.join(out, "perf-stat", f"{side}-{label}.stat"))
            lines.append("| {} | {} | {:.1f} | {} | {} | {} | {} |".format(
                side, label, stat["cpu_ms"], stat["context_switches"],
                stat["cpu_migrations"], stat["page_faults"],
                "; ".join(stat["unsupported"]) or "-"))
    lines.append("")
    lines.append("## Dominant stacks (profile runs)")
    lines.append("")
    baseline_gateway_profile = os.path.join(out, "baseline-gateway.pprof")
    if not os.path.exists(baseline_gateway_profile):
        baseline_gateway_profile = os.path.join(
            out, "baseline-gateway.perf.data")
    for label, path in [
        ("baseline-gateway (Go pprof top)" if
            baseline_gateway_profile.endswith(".pprof")
            else "baseline-gateway (perf top, self)",
            baseline_gateway_profile),
        ("baseline-worker (perf top, self)", os.path.join(out, "baseline-worker.perf.data")),
        ("candidate-host (perf top, self)", os.path.join(out, "candidate-host.perf.data")),
        ("candidate-worker (perf top, self)", os.path.join(out, "candidate-worker.perf.data")),
    ]:
        lines.append("")
        lines.extend(profile_summary(out, label, path))
    lines.append("")
    lines.append("Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, "
                 "candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.")
    return "\n".join(lines) + "\n"


def main():
    out = sys.argv[1]
    meta = read_meta(os.path.join(out, ".tmp", "meta.env"))
    evidence_status = meta.get("EVIDENCE_STATUS", "incomplete")
    incomplete_reasons = meta.get("INCOMPLETE_REASONS", "")
    manifest = build_manifest(out, meta, evidence_status, incomplete_reasons)
    with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")
    report = build_report(out, meta, manifest)
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as handle:
        handle.write(report)
    # Validate the outputs we just wrote.
    with open(os.path.join(out, "manifest.json"), "r", encoding="utf-8") as handle:
        json.load(handle)
    print("evidence: manifest.json and report.md written")


if __name__ == "__main__":
    main()
