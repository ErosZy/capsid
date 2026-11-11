#!/usr/bin/env python3
"""Managed/strict density lane with explicit observation/formal artifact modes."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import signal
import sys

from density_control import DensitySearch
from density_probe import DensityProbe, write_json
from measurement_runtime import ProbeError
from perf_evidence import PROTOCOL, sha256, validate_lane


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for flag, env in (("out", "OUT"), ("host", "HOST_BIN"), ("worker", "WORKER"), ("workerd", "WORKERD_BIN")):
        parser.add_argument("--" + flag, type=Path, default=os.environ.get(env), required=not os.environ.get(env))
    parser.add_argument("--apps", type=Path, default=os.environ.get("APPS", "/tmp/cold-start-apps"))
    parser.add_argument("--cpuset", default=os.environ.get("SUT_CPUSET", "0-3"))
    parser.add_argument("--sizes", nargs="+", choices=("10k", "100k", "1m"), default=os.environ.get("SIZES", "10k 100k 1m").split())
    parser.add_argument("--stacks", nargs="+", choices=("capsid", "workerd"), default=os.environ.get("STACKS", "capsid workerd").split())
    parser.add_argument("--budget-mb", type=int, default=os.environ.get("BUDGET_MB", 2048))
    parser.add_argument("--max-workers", type=int, default=os.environ.get("MAX_WORKERS", 4096))
    parser.add_argument("--max-cells", type=int, default=os.environ.get("MAX_CELLS", 1024))
    parser.add_argument("--warm-requests", type=int, default=os.environ.get("WARM_REQS", 100))
    parser.add_argument("--parallel", type=int, default=os.environ.get("CURL_PAR", 64))
    parser.add_argument("--metadata", type=Path, default=os.environ.get("CAPSID_BENCH_METADATA"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository archive root, independent of the measured source checkout")
    parser.add_argument("--observation", action="store_true", help="run a restricted observation, never a publication envelope")
    parser.add_argument("--cell-deadline", type=int, default=3600)
    args = parser.parse_args(argv)
    if sys.platform != "linux":
        parser.error("density process-tree PSS measurement requires Linux")
    if len(set(args.sizes)) != len(args.sizes) or len(set(args.stacks)) != len(args.stacks):
        parser.error("duplicate density sizes or stacks")
    if not args.sizes or not set(args.sizes) <= {"10k", "100k", "1m"} or not args.stacks or not set(args.stacks) <= {"capsid", "workerd"}:
        parser.error("unknown density size or stack")
    if any(value <= 0 for value in (args.budget_mb, args.max_workers, args.max_cells, args.warm_requests, args.parallel, args.cell_deadline)):
        parser.error("density limits must be positive")
    if not args.observation and (args.metadata is None or set(args.sizes) != {"10k", "100k", "1m"} or
                                set(args.stacks) != {"capsid", "workerd"} or args.budget_mb != 2048 or
                                args.warm_requests != 100 or args.parallel != 64):
        parser.error("formal density requires run metadata and the complete frozen protocol; use --observation for a restricted smoke")
    if any(os.environ.get(key, "1") != "1" for key in ("CAPSID_MANAGED", "CAPSID_STRICT")):
        parser.error("the density lane always uses managed/strict posture")
    for key in ("host", "worker", "workerd", "apps", "out"):
        setattr(args, key, getattr(args, key).resolve())
    for binary in (args.host, args.worker, args.workerd):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"missing executable: {binary}")
    if args.out.exists():
        parser.error("OUT must be a new directory; existing evidence is never truncated")
    args.repo = args.repo.resolve()
    if not args.observation and not args.out.is_relative_to(args.repo / "bench/results"):
        parser.error("formal density archives must live under bench/results")
    return args


def verify_fixtures(args):
    path = Path(__file__).with_name("gen-cold-start-apps.py")
    spec = importlib.util.spec_from_file_location("density_fixture_generator", path)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    originals = {}
    for stack in args.stacks:
        for size in args.sizes:
            file = args.apps / f"app-{stack}-{size}.mjs"
            expected = generator.body_to(generator.SIZES[size])[0] + (
                generator.capsid_entry() if stack == "capsid" else generator.workerd_entry())
            data = file.read_bytes()
            if data != expected.encode("utf-8"):
                raise ProbeError("fixture-identity", "density fixture differs from the frozen generator", file=str(file))
            originals[f"app-{stack}-{size}"] = (file, data)
    return originals


def run_lane(args):
    from perf_run import (machine_identity, readable_manifest, recheck_runner, run_metadata,
                          seal_archive, session_lock, snapshot_runner, utc_now)
    args._owns_output = False
    binary_paths = {"capsid-host": args.host, "capsid-worker": args.worker, "workerd": args.workerd}
    binaries_before = {key: sha256(path) for key, path in binary_paths.items()}
    originals = verify_fixtures(args)
    metadata = None if args.observation else run_metadata(args.metadata, binary_paths)
    args.out.mkdir(parents=True)
    args._owns_output = True
    started = utc_now()
    runner, runner_inputs = snapshot_runner(args.out)
    environment = {"OUT": args.out, "HOST_BIN": args.host, "WORKER": args.worker, "WORKERD_BIN": args.workerd,
                   "APPS": args.apps, "SUT_CPUSET": args.cpuset, "SIZES": " ".join(args.sizes),
                   "STACKS": " ".join(args.stacks), "BUDGET_MB": args.budget_mb,
                   "MAX_WORKERS": args.max_workers, "MAX_CELLS": args.max_cells, "WARM_REQS": args.warm_requests,
                   "CURL_PAR": args.parallel, "CAPSID_MANAGED": 1, "CAPSID_STRICT": 1}
    command = shlex.join([f"{key}={value}" for key, value in environment.items()] +
                        [sys.executable, "-B", str(Path(__file__).resolve()), "--repo", str(args.repo),
                         "--cell-deadline", str(args.cell_deadline)] +
                        (["--observation"] if metadata is None else ["--metadata", str(args.metadata.resolve())]))
    write_json(args.out / "run.json", {"status": "running", "startedAt": started, "command": command,
               "profile": "observation" if metadata is None else "formal", "binarySHA256": binaries_before,
               "runner": runner})
    rows, records, fixtures = [], [], {}
    frozen_apps = args.out / "fixtures"
    frozen_apps.mkdir()
    for name, (source, data) in originals.items():
        target = frozen_apps / source.name
        target.write_bytes(data)
        value = sha256(target)
        fixtures[name] = {"file": "fixtures/" + source.name, "sha256": value}
        runner_inputs[str(target)] = value
    with session_lock(metadata is not None) as lock:
        adapter = DensityProbe(host=args.host, worker=args.worker, workerd=args.workerd, apps=frozen_apps,
                               out=args.out, cpuset=args.cpuset, warm_requests=args.warm_requests, parallel=args.parallel)
        with (args.out / "probes.jsonl").open("w", encoding="utf-8", newline="\n") as probes:
            def record_probe(record):
                probes.write(json.dumps(record, sort_keys=True, allow_nan=False) + "\n")
                probes.flush()
            search = DensitySearch(adapter, record_probe, budget_mb=args.budget_mb, warm_requests=args.warm_requests,
                                   deadline_seconds=args.cell_deadline)
            for stack in args.stacks:
                for size in args.sizes:
                    for phase in ("cold", "warm"):
                        # The search deadline is per cell, not consumed by
                        # preceding cells; allocator and probe IDs stay shared.
                        search.deadline = search.clock() + args.cell_deadline
                        cap = args.max_workers if stack == "capsid" else args.max_cells
                        result = search.fill(stack, size, phase, cap)
                        directory = result["probeDirectory"]
                        raw, correct = directory + "/final.jsonl", directory + "/correctness.json"
                        (args.out / raw).write_text(json.dumps(result, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
                        write_json(args.out / correct, result["correctness"])
                        wrapper = directory + ("/runtime/apps/orders/v1/bundle.mjs" if stack == "capsid" else "/runtime/bundle.mjs")
                        fixture_id = "density-" + directory
                        fixtures[fixture_id] = {"file": wrapper, "sha256": sha256(args.out / wrapper)}
                        records.append({"stack": stack, "size": size, "phase": phase, "samples": [raw],
                                        "correctness": [correct], "fixtures": [f"app-{stack}-{size}", fixture_id],
                                        "binaries": ["capsid-host", "capsid-worker"] if stack == "capsid" else ["workerd"]})
                        rows.append(f"{stack} {size} {result['unit']}={result['count']} pss_mb={result['pss_readings_kb'][0]/1024:.1f} phase={phase} note={result['limit']}")
        if any(sha256(path) != binaries_before[key] for key, path in binary_paths.items()):
            raise ProbeError("binary-changed", "measured executable bytes changed during the lane")
        recheck_runner(runner_inputs)
        (args.out / "cells.txt").write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
        common = {"startedAt": started, "completedAt": utc_now(), "machine": machine_identity(),
                  "profile": "observation" if metadata is None else "formal", "status": "complete",
                  "command": command,
                  "posture": {"platform": "linux", "mode": "managed", "sandbox": "strict"},
                  "observedBinarySHA256": binaries_before,
                  "runner": runner,
                  "protocol": dict(PROTOCOL["density"], sutCpuSet=args.cpuset,
                                   caps={"capsid": args.max_workers, "workerd": args.max_cells},
                                   budgetMb=args.budget_mb, warmRequests=args.warm_requests,
                                   parallelRequests=args.parallel, pssIntervalSeconds=1, pssAttempts=8)}
        write_json(args.out / "run.json", common)
        if metadata is None:
            write_json(args.out / "observation.json", common)
            seal_archive(args.out)
            return {"status": "complete", "profile": "observation", "cells": len(rows), "publicationEligible": False}
        manifest = dict(common, schema=1, lane="density", session=metadata["session"], origin=metadata["origin"],
                        command=command, summary="cells.txt", isolation="isolation.json", binaries=metadata["binaries"],
                        fixtures=fixtures, records=records)
        write_json(args.out / "run-metadata.json", metadata)
        write_json(args.out / "isolation.json", {"session": metadata["session"], "origin": metadata["origin"],
                   "machine": manifest["machine"], "posture": manifest["posture"], "quietSession": True,
                   "ownedPidsRemaining": 0, "coordinationLock": lock})
        (args.out / "manifest.txt").write_text(readable_manifest(manifest), encoding="utf-8", newline="\n")
        write_json(args.out / "lane.json", manifest)
        seal_archive(args.out)
        # Validate the exact archive we just produced before reporting success.
        spec = importlib.util.spec_from_file_location("density_collector", Path(__file__).with_name("collect-perf-point.py"))
        collector = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(collector)
        validate_lane(args.out, args.repo, "density", collector.parse_density(args.out),
                      metadata["origin"]["version"], metadata["origin"]["release"])
        return {"status": "complete", "profile": "formal", "cells": len(rows), "publicationEligible": True}


def main(argv=None):
    args = parse_args(argv)
    def stop(signum, _):
        raise ProbeError("interrupted", "density controller was interrupted", signal=signum)
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        outcome = run_lane(args)
    except BaseException as exc:
        if getattr(args, "_owns_output", False) and args.out.is_dir():
            result = exc.record() if isinstance(exc, ProbeError) else {
                "status": "error", "error": {"type": type(exc).__name__, "message": str(exc)}}
            write_json(args.out / "failed.json", result)
            if (args.out / "run.json").is_file():
                run = json.loads((args.out / "run.json").read_text(encoding="utf-8"))
                write_json(args.out / "run.json", dict(run, status="error", error=result["error"]))
        raise
    print(json.dumps(outcome, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (ProbeError, OSError, ValueError, KeyError, TypeError) as exc:
        print(f"density lane failed: {exc}", file=sys.stderr)
        sys.exit(1)
