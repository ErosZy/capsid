#!/usr/bin/env python3
"""Paired A/B runner for serialized classic-script benchmark corpora.

Each optimizer/runtime candidate is represented by a classic-bytecode binary
and pass mask.  Programs are compiled once per arm, smoke-validated, then run
in balanced ABBA/BAAB order with one fresh QuickJS runtime per sample.  The
script records raw samples and a paired-log-ratio summary; it never decides
whether an optimization should ship.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], timeout: int, stdout: Path | None = None,
        stderr: Path | None = None) -> subprocess.CompletedProcess[str]:
    out_handle = stdout.open("w", encoding="utf-8") if stdout else subprocess.PIPE
    err_handle = stderr.open("w", encoding="utf-8") if stderr else subprocess.PIPE
    try:
        return subprocess.run(command, check=False, text=True,
                              stdout=out_handle, stderr=err_handle,
                              timeout=timeout)
    finally:
        if stdout:
            out_handle.close()  # type: ignore[union-attr]
        if stderr:
            err_handle.close()  # type: ignore[union-attr]


def parse_single_sample(text: str) -> float:
    rows = [json.loads(line) for line in text.splitlines() if line.strip()]
    if len(rows) != 1 or rows[0].get("ok") is not True:
        raise ValueError(f"expected one successful JSON sample, got {rows!r}")
    value = float(rows[0]["ms"])
    if not math.isfinite(value) or value <= 0:
        raise ValueError(f"invalid elapsed time {value}")
    return value


def log_mean_ci95(values: list[float]) -> tuple[float, float] | None:
    """Two-sided Student-t interval for a mean in log-ratio space."""
    if len(values) < 2:
        return None
    t95 = [
        0.0, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365,
        2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131,
        2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069,
        2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042,
    ]
    degrees = len(values) - 1
    critical = t95[degrees] if degrees < len(t95) else 1.96
    center = statistics.mean(values)
    half_width = critical * statistics.stdev(values) / math.sqrt(len(values))
    return ((math.exp(center - half_width) - 1.0) * 100.0,
            (math.exp(center + half_width) - 1.0) * 100.0)


def paired_summary(samples: list[dict[str, Any]]) -> dict[str, Any]:
    grouped: dict[tuple[str, int], dict[str, list[float]]] = {}
    for row in samples:
        key = (row["program"], row["pair"])
        grouped.setdefault(key, {"control": [], "candidate": []})[
            row["arm"]
        ].append(float(row["ms"]))

    per_program: dict[str, list[float]] = {}
    for (program, _pair), arms in grouped.items():
        if len(arms["control"]) != 2 or len(arms["candidate"]) != 2:
            raise ValueError(f"incomplete pair for {program}")
        control = statistics.geometric_mean(arms["control"])
        candidate = statistics.geometric_mean(arms["candidate"])
        per_program.setdefault(program, []).append(math.log(control / candidate))

    result: dict[str, Any] = {"programs": {}, "suites": {}}
    program_centers: list[float] = []
    suite_centers: dict[str, list[float]] = {}
    program_suites = {str(row["program"]): str(row["suite"])
                      for row in samples}
    for program, log_ratios in sorted(per_program.items()):
        center = statistics.mean(log_ratios)
        gains = [(math.exp(value) - 1.0) * 100.0 for value in log_ratios]
        result["programs"][program] = {
            "pairs": len(log_ratios),
            "gain_pct": (math.exp(center) - 1.0) * 100.0,
            "pair_gains_pct": gains,
            "positive_pairs": sum(value > 0 for value in log_ratios),
            "gain_ci95_pct": log_mean_ci95(log_ratios),
        }
        program_centers.append(center)
        suite_centers.setdefault(program_suites[program], []).append(center)
    for suite, centers in sorted(suite_centers.items()):
        result["suites"][suite] = {
            "programs": len(centers),
            "equal_weight_geomean_gain_pct":
                (math.exp(statistics.mean(centers)) - 1.0) * 100.0,
            "program_dispersion_ci95_pct": log_mean_ci95(centers),
        }
    result["equal_weight_geomean_gain_pct"] = (
        (math.exp(statistics.mean(program_centers)) - 1.0) * 100.0
        if program_centers else None
    )
    result["program_dispersion_ci95_pct"] = log_mean_ci95(program_centers)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--control-tool", type=Path, required=True)
    parser.add_argument("--candidate-tool", type=Path)
    parser.add_argument("--control-passes", default="0")
    parser.add_argument("--candidate-passes", default="0xffffffff")
    parser.add_argument("--pairs", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--cpuset", default="2")
    parser.add_argument("--program", action="append", default=[],
                        help="program name or filename; repeat to select")
    args = parser.parse_args()
    if args.pairs <= 0 or args.timeout <= 0:
        parser.error("--pairs and --timeout must be positive")
    candidate_tool = args.candidate_tool or args.control_tool
    manifest_path = args.corpus / "manifest.json"
    corpus_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    programs = list(corpus_manifest["programs"])
    selected = set(args.program)
    if selected:
        programs = [row for row in programs
                    if row["name"] in selected or row["file"] in selected or
                    f'{row["suite"]}/{row["name"]}' in selected]
    if not programs:
        parser.error("no programs selected")
    args.out.mkdir(parents=True, exist_ok=True)
    bytecode_dir = args.out / "bytecode"
    bytecode_dir.mkdir(exist_ok=True)

    manifest = {
        "schema": "capsid-classic-suite-ab-v1",
        "command": sys.argv,
        "git_head": subprocess.run(
            ["git", "rev-parse", "HEAD"], check=True, text=True,
            stdout=subprocess.PIPE).stdout.strip(),
        "git_diff_sha256": hashlib.sha256(subprocess.run(
            ["git", "diff", "--no-ext-diff", "--binary"], check=True,
            stdout=subprocess.PIPE).stdout).hexdigest(),
        "platform": platform.platform(),
        "python": sys.version,
        "cpuset": args.cpuset,
        "pairs": args.pairs,
        "timeout_seconds": args.timeout,
        "corpus_manifest_sha256": sha256(manifest_path),
        "control": {"tool": str(args.control_tool.resolve()),
                    "sha256": sha256(args.control_tool),
                    "passes": args.control_passes},
        "candidate": {"tool": str(candidate_tool.resolve()),
                      "sha256": sha256(candidate_tool),
                      "passes": args.candidate_passes},
        "programs": programs,
    }
    (args.out / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    compiled: dict[tuple[str, str], Path] = {}
    failures: list[dict[str, str]] = []
    for program in programs:
        source = args.corpus / program["file"]
        stem = Path(program["file"]).stem
        for arm, tool, passes in (
            ("control", args.control_tool, args.control_passes),
            ("candidate", candidate_tool, args.candidate_passes),
        ):
            output = bytecode_dir / f"{stem}.{arm}.qjsb"
            command = [str(tool), "compile", "--input", str(source),
                       "--source-name", f"file:///{program['file']}",
                       "--output", str(output), "--optimize", "--passes",
                       passes, "--report"]
            result = run(command, args.timeout,
                         stderr=args.out / f"{stem}.{arm}.compile.stderr")
            if result.returncode != 0:
                failures.append({"program": stem, "stage": f"compile-{arm}"})
                break
            smoke = run(
                ["taskset", "-c", args.cpuset, str(tool), "run", "--input",
                 str(output), "--warmup", "0", "--rounds", "1",
                 "--expect-global-true", "__capsidSuiteOk"],
                args.timeout,
                stdout=args.out / f"{stem}.{arm}.smoke.jsonl",
                stderr=args.out / f"{stem}.{arm}.smoke.stderr")
            if smoke.returncode != 0:
                failures.append({"program": stem, "stage": f"smoke-{arm}"})
                break
            compiled[(stem, arm)] = output

    runnable = [program for program in programs
                if (Path(program["file"]).stem, "control") in compiled and
                (Path(program["file"]).stem, "candidate") in compiled]
    samples: list[dict[str, Any]] = []
    raw_path = args.out / "samples.jsonl"
    with raw_path.open("w", encoding="utf-8") as raw:
        for program in runnable:
            stem = Path(program["file"]).stem
            for pair in range(1, args.pairs + 1):
                order = ("control", "candidate", "candidate", "control") \
                    if pair % 2 else \
                    ("candidate", "control", "control", "candidate")
                for sequence, arm in enumerate(order, 1):
                    tool = args.control_tool if arm == "control" else candidate_tool
                    command = ["taskset", "-c", args.cpuset, str(tool), "run",
                               "--input", str(compiled[(stem, arm)]),
                               "--warmup", "0", "--rounds", "1",
                               "--expect-global-true", "__capsidSuiteOk"]
                    result = run(command, args.timeout)
                    if result.returncode != 0:
                        failures.append({"program": stem,
                                         "stage": f"pair-{pair}-{sequence}-{arm}",
                                         "stderr": str(result.stderr)})
                        break
                    try:
                        elapsed = parse_single_sample(str(result.stdout))
                    except (ValueError, json.JSONDecodeError) as exc:
                        failures.append({"program": stem,
                                         "stage": f"parse-{pair}-{sequence}-{arm}",
                                         "error": str(exc)})
                        break
                    row = {"suite": program["suite"], "program": stem,
                           "pair": pair, "sequence": sequence,
                           "arm": arm, "ms": elapsed}
                    samples.append(row)
                    raw.write(json.dumps(row, sort_keys=True) + "\n")
                    raw.flush()
                else:
                    continue
                break

    complete_programs = []
    for program in runnable:
        stem = Path(program["file"]).stem
        if sum(row["program"] == stem for row in samples) == args.pairs * 4:
            complete_programs.append(stem)
    complete_samples = [row for row in samples
                        if row["program"] in complete_programs]
    summary = paired_summary(complete_samples)
    summary.update({"requested_programs": len(programs),
                    "runnable_programs": len(runnable),
                    "complete_programs": len(complete_programs),
                    "failures": failures})
    (args.out / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
