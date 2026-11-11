#!/usr/bin/env python3
"""Owned cold-start lane, with separate observation and formal archive modes."""
import argparse
from contextlib import contextmanager
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import sys

from cold_control import checked_round, cpu_selection, identity
from cold_probe import ColdProbe, compile_fixture, write_json
from measurement_runtime import ProbeError
from perf_evidence import PROTOCOL, STACKS, median, required_binaries, sha256, validate_lane
from perf_run import (machine_identity, readable_manifest, recheck_runner, run_metadata,
                      seal_archive, session_lock, snapshot_runner, utc_now)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    build = Path(os.environ.get("BUILD_DIR", "build-m1d"))
    parser.add_argument("--out", type=Path, default=os.environ.get("OUT"))
    for flag, env, default in (("worker", "WORKER", build / "capsid-worker"),
            ("cold", "COLD", build / "cold-start"), ("compiler", "COMPILE", build / "capsid-bytecode-compile"),
            ("node", "NODE_BIN", shutil.which("node")), ("deno", "DENO_BIN", shutil.which("deno")),
            ("workerd", "WORKERD_BIN", shutil.which("workerd"))):
        parser.add_argument("--" + flag, type=Path, default=os.environ.get(env, default))
    parser.add_argument("--apps", type=Path, default=os.environ.get("APPS", "/tmp/cold-start-apps"))
    parser.add_argument("--cpuset", default=os.environ.get("SUT_CPUSET", "0-3"))
    parser.add_argument("--sizes", nargs="+", default=os.environ.get("SIZES", "10k 100k 1m").split())
    parser.add_argument("--stacks", nargs="+", default=os.environ.get("STACKS", "capsid node deno workerd").split())
    parser.add_argument("--rounds", type=int, default=os.environ.get("ROUNDS", 5))
    parser.add_argument("--warm", type=int, default=os.environ.get("WARM", 50))
    parser.add_argument("--round-timeout", type=int, default=180)
    parser.add_argument("--metadata", type=Path, default=os.environ.get("CAPSID_BENCH_METADATA"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--observation", action="store_true")
    args = parser.parse_args(argv)
    if sys.platform != "linux": parser.error("cold process-tree PSS measurement requires Linux")
    args.stacks = [s for value in args.stacks for s in (("capsid-source", "capsid-bytecode") if value == "capsid" else (value,))]
    if (not args.sizes or not set(args.sizes) <= {"10k", "100k", "1m"} or len(args.sizes) != len(set(args.sizes)) or
            not args.stacks or not set(args.stacks) <= set(STACKS["cold-start"]) or len(args.stacks) != len(set(args.stacks))):
        parser.error("unknown or duplicate cold sizes/stacks")
    if any(value <= 0 for value in (args.rounds, args.warm, args.round_timeout)) or args.rounds > 1000 or args.warm > 100000 or args.round_timeout > 3600:
        parser.error("cold rounds, warm requests and deadlines must be positive and bounded")
    if not args.observation and (args.metadata is None or args.rounds != 5 or args.warm != 50 or
            set(args.sizes) != {"10k", "100k", "1m"} or set(args.stacks) != set(STACKS["cold-start"])):
        parser.error("formal cold requires metadata and the complete frozen protocol; use --observation for a restricted smoke")
    if os.environ.get("CAPSID_STRICT", "1") != "1": parser.error("cold C-ABI workers always use strict posture")
    try: selected = cpu_selection(args.cpuset, os.sched_getaffinity(0))
    except ProbeError as exc: parser.error(str(exc))
    if not args.observation and len(selected) != 4: parser.error("formal cold requires four available CPU indices")
    args.repo, args.apps = args.repo.resolve(), args.apps.resolve()
    args.out = (args.out or args.repo / "bench/results" / ("cold-start-" + utc_now().replace(":", ""))).resolve()
    if args.out.exists(): parser.error("OUT must be new; existing evidence is never truncated")
    if not args.observation and not args.out.is_relative_to(args.repo / "bench/results"):
        parser.error("formal cold archives must live under bench/results")
    paths = binary_paths(args)
    for name, path in paths.items():
        if path is None or not path.is_file() or not os.access(path, os.X_OK): parser.error(f"missing executable for {name}: {path}")
    for attr in ("worker", "cold", "compiler", "node", "deno", "workerd"):
        value = getattr(args, attr)
        if value is not None: setattr(args, attr, value.resolve())
    return args


def binary_paths(args):
    paths = {"capsid-worker": args.worker, "cold-start": args.cold, "capsid-bytecode-compile": args.compiler,
             "node": args.node, "deno": args.deno, "workerd": args.workerd}
    required = set().union(*(required_binaries("cold-start", stack) for stack in args.stacks))
    return {name: paths[name] for name in sorted(required)}


def verify_fixtures(args):
    spec = importlib.util.spec_from_file_location("cold_fixture_generator", Path(__file__).with_name("gen-cold-start-apps.py"))
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    originals = {}
    for stack in args.stacks:
        kind = "capsid" if stack.startswith("capsid-") else stack
        for size in args.sizes:
            name = f"app-{kind}-{size}"
            if name in originals: continue
            file = args.apps / (name + ".mjs")
            expected = generator.body_to(generator.SIZES[size])[0] + getattr(generator, kind + "_entry")()
            data = file.read_bytes()
            if data != expected.encode("utf-8"):
                raise ProbeError("fixture-identity", "cold fixture differs from frozen generator", file=str(file))
            originals[name] = (file, data)
    return originals


@contextmanager
def client_affinity(selected):
    previous = os.sched_getaffinity(0)
    os.sched_setaffinity(0, selected)
    try: yield sorted(os.sched_getaffinity(0))
    finally: os.sched_setaffinity(0, previous)


def run_lane(args):
    args._owns_output = False
    selected = cpu_selection(args.cpuset, os.sched_getaffinity(0))
    if not args.observation and len(selected) != 4:
        raise ProbeError("cold-affinity", "formal cold requires four CPU indices")
    paths = binary_paths(args)
    before = {name: sha256(path) for name, path in paths.items()}
    originals = verify_fixtures(args)
    metadata = None if args.observation else run_metadata(args.metadata, paths)
    args.out.mkdir(parents=True)
    args._owns_output = True
    started = utc_now()
    runner, inputs = snapshot_runner(args.out, files=("cold-start.sh", "cold-start.py", "cold_probe.py", "cold_control.py",
        "cold-start.cc", "cold_start_measurement.h", "measurement_runtime.py", "perf_run.py",
        "gen-cold-start-apps.py", "perf_evidence.py", "qps_control.py", "collect-perf-point.py"))
    if metadata is not None: inputs[str(args.metadata.resolve())] = sha256(args.metadata)
    command = shlex.join([sys.executable, "-B", str(Path(__file__).resolve()), "--out", str(args.out),
        "--repo", str(args.repo), "--apps", str(args.apps), "--cpuset", args.cpuset, "--rounds", str(args.rounds),
        "--warm", str(args.warm), "--round-timeout", str(args.round_timeout), "--sizes", *args.sizes, "--stacks", *args.stacks] +
        [value for name in paths for value in ("--" + {"capsid-worker": "worker", "cold-start": "cold", "capsid-bytecode-compile": "compiler"}.get(name, name), str(paths[name]))] +
        (["--observation"] if metadata is None else ["--metadata", str(args.metadata.resolve())]))
    write_json(args.out / "run.json", {"status": "running", "startedAt": started, "command": command,
               "observedBinarySHA256": before, "runner": runner})
    fixtures, records, rows = {}, [], []
    frozen = args.out / "fixtures"
    frozen.mkdir()
    for name, (source, data) in originals.items():
        target = frozen / source.name
        target.write_bytes(data)
        fixtures[name] = {"file": target.relative_to(args.out).as_posix(), "sha256": sha256(target)}
        inputs[str(target)] = sha256(target)
    with session_lock(metadata is not None) as lock, client_affinity(selected) as affinity:
        compiled = {}
        if "capsid-bytecode" in args.stacks:
            for size in args.sizes:
                compiled[size] = compile_fixture(paths["capsid-bytecode-compile"], frozen / f"app-capsid-{size}.mjs",
                                                  args.out / "compiled" / size, args.cpuset)
                for file in compiled[size].parent.iterdir():
                    if file.name in ("bundle.qjsb", "attestation.json", "message.bin"):
                        name = "cold-bytecode-" + size + "-" + file.name
                        fixtures[name] = {"file": file.relative_to(args.out).as_posix(), "sha256": sha256(file)}
                        inputs[str(file)] = sha256(file)
        adapter = ColdProbe(binaries=paths, apps=frozen, compiled=compiled, out=args.out, cpuset=args.cpuset,
                            warm_requests=args.warm, round_timeout=args.round_timeout)
        with (args.out / "probes.jsonl").open("x", encoding="utf-8") as log:
            for stack in args.stacks:
                for size in args.sizes:
                    cell = args.out / "cells" / stack / size
                    cell.mkdir(parents=True)
                    measured = []
                    roots = set()
                    fixture_ids = [f"app-{'capsid' if stack.startswith('capsid-') else stack}-{size}"]
                    if stack == "capsid-bytecode": fixture_ids += [n for n in fixtures if n.startswith("cold-bytecode-" + size + "-")]
                    for round_no in range(args.rounds + 1):
                        result = adapter(stack=stack, size=size, round_no=round_no)
                        checked_round(result, args.warm)
                        root = identity(result["measurementRoot"])
                        if root in roots: raise ProbeError("cold-process-reused", "cold probe reused a process across rounds")
                        roots.add(root)
                        if (type(result.get("round")) is not int or
                                (result.get("stack"), result.get("size"), result.get("round")) != (stack, size, round_no)):
                            raise ProbeError("cold-cell", "probe returned a different cell or round")
                        log.write(json.dumps(result, sort_keys=True, allow_nan=False) + "\n")
                        log.flush()
                        directory = args.out / result["probeDirectory"] / "runtime"
                        if directory.is_dir():
                            for file in sorted(directory.iterdir()):
                                if file.is_file():
                                    name = "cold-runtime-" + result["probeDirectory"] + "-" + file.name
                                    fixtures[name] = {"file": file.relative_to(args.out).as_posix(), "sha256": sha256(file)}
                                    fixture_ids.append(name)
                        if round_no == 0:
                            (cell / "discarded.jsonl").write_text(json.dumps(result, allow_nan=False) + "\n", encoding="utf-8")
                            write_json(cell / "discarded-correctness.json", result["correctness"])
                        else:
                            measured.append(result)
                    (cell / "measured.jsonl").write_text("".join(json.dumps(v, allow_nan=False) + "\n" for v in measured), encoding="utf-8")
                    write_json(cell / "correctness.json", {"ok": True, "errors_as_success": False, "errors": 0,
                        "mismatches": 0, "timeouts": 0, "responses_checked": sum(v["correctness"]["responses_checked"] for v in measured)})
                    relative = cell.relative_to(args.out).as_posix()
                    records.append({"stack": stack, "size": size, "samples": [relative + "/measured.jsonl"],
                        "correctness": [relative + "/correctness.json"], "discardedSamples": [relative + "/discarded.jsonl"],
                        "discardedCorrectness": [relative + "/discarded-correctness.json"], "fixtures": fixture_ids,
                        "binaries": sorted(required_binaries("cold-start", stack))})
                    prefix = stack.replace("capsid-", "capsid ")
                    rows.append(f"{prefix} {size} total_ms={median([v['total_ms'] for v in measured]):.2f} "
                        f"ready_ms={median([v['ready_ms'] for v in measured]):.2f} warm_ms={median([v['warm_ms'] for v in measured]):.3f} "
                        f"pss_mb={median([v['pss_kb'] for v in measured])/1024:.1f}")
        if any(sha256(path) != before[name] for name, path in paths.items()):
            raise ProbeError("binary-changed", "measured executables changed during cold lane")
        recheck_runner(inputs)
        if metadata is not None: run_metadata(args.metadata, paths)
        (args.out / "cells.txt").write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
        common = {"startedAt": started, "completedAt": utc_now(), "status": "complete", "command": command,
            "profile": "observation" if metadata is None else "formal", "machine": machine_identity(), "clientAffinity": affinity,
            "posture": {"platform": "linux", "mode": "c-abi", "sandbox": "strict"}, "runner": runner,
            "observedBinarySHA256": before, "protocol": dict(PROTOCOL["cold-start"], sutCpuSet=args.cpuset,
                rounds=args.rounds, warmRequests=args.warm, pssAttempts=8, pssIntervalSeconds=1, settleSeconds=1)}
        write_json(args.out / "run.json", common)
        if metadata is None:
            write_json(args.out / "observation.json", common)
            seal_archive(args.out)
            return {"status": "complete", "profile": "observation", "cells": len(rows), "publicationEligible": False}
        manifest = dict(common, schema=1, lane="cold-start", session=metadata["session"], origin=metadata["origin"],
            summary="cells.txt", isolation="isolation.json", binaries=metadata["binaries"], fixtures=fixtures, records=records)
        write_json(args.out / "run-metadata.json", metadata)
        write_json(args.out / "isolation.json", {"session": metadata["session"], "origin": metadata["origin"],
            "machine": common["machine"], "posture": common["posture"], "quietSession": True,
            "ownedPidsRemaining": 0, "coordinationLock": lock})
        (args.out / "manifest.txt").write_text(readable_manifest(manifest), encoding="utf-8", newline="\n")
        write_json(args.out / "lane.json", manifest)
        seal_archive(args.out)
        spec = importlib.util.spec_from_file_location("cold_collector", Path(__file__).with_name("collect-perf-point.py"))
        collector = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(collector)
        validate_lane(args.out, args.repo, "cold-start", collector.parse_cold_start(args.out),
                      metadata["origin"]["version"], metadata["origin"]["release"])
        return {"status": "complete", "profile": "formal", "cells": len(rows), "publicationEligible": True}


def main(argv=None):
    args = parse_args(argv)
    def stop(signum, _): raise ProbeError("interrupted", "cold controller was interrupted", signal=signum)
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        result = run_lane(args)
    except BaseException as exc:
        if getattr(args, "_owns_output", False) and args.out.is_dir():
            error = exc.record() if isinstance(exc, ProbeError) else {"status": "error", "error": {"type": type(exc).__name__, "message": str(exc)}}
            write_json(args.out / "failed.json", error)
            for name in ("run.json", "lane.json"):
                if (args.out / name).is_file():
                    previous = json.loads((args.out / name).read_text())
                    write_json(args.out / name, dict(previous, status="error", error=error["error"]))
        raise
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    try: main()
    except (ProbeError, OSError, ValueError, KeyError, TypeError) as exc:
        print(f"cold lane failed: {exc}", file=sys.stderr)
        sys.exit(1)
