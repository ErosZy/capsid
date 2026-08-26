#!/usr/bin/env python3
"""Collect source-attributed opcode profiles for a classic-suite corpus.

Compilation uses the production rewriter build. Execution uses a separate
CONFIG_OPCODE_PROFILE runner, so instrumented timing is never treated as a
performance result. Each program gets a fresh runtime and its own v4 JSONL
profile, preserving stable source/function/PC coordinates.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import subprocess
import sys
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], timeout: int, stdout: Path, stderr: Path) -> int:
    with stdout.open("w", encoding="utf-8") as out_handle, \
            stderr.open("w", encoding="utf-8") as err_handle:
        try:
            result = subprocess.run(command, check=False, text=True,
                                    stdout=out_handle, stderr=err_handle,
                                    timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124
    return result.returncode


def source_hash(name: str) -> str:
    value = 14695981039346656037
    for byte in name.encode("utf-8")[:255]:
        value ^= byte
        value = (value * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


def validate_profile(path: Path, source_name: str) -> dict[str, int]:
    """Reject partial, obsolete, or non-joinable instrumentation output."""
    runtimes = 0
    sites = 0
    source_sites = 0
    overflow = 0
    expected_source = source_hash(source_name)
    with path.open(encoding="utf-8", errors="strict") as stream:
        for line_number, raw in enumerate(stream, 1):
            line = raw.strip()
            if not line:
                continue
            try:
                profile = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(
                    f"invalid profile JSON on line {line_number}: {error}") from error
            if profile.get("schema") != "quickjs-ng-opcode-profile-v4":
                raise ValueError("classic profile requires opcode profile v4")
            runtimes += 1
            overflow += int(profile.get("site_overflow", 0))
            for site in profile.get("sites", []):
                sites += 1
                for field in ("code_hash", "code_len", "line", "column", "pc"):
                    if field not in site:
                        raise ValueError(f"site missing stable field {field}")
                if str(site.get("source_hash", "")).lower() == expected_source:
                    source_sites += 1
    if runtimes != 1:
        raise ValueError(f"expected one runtime dump, got {runtimes}")
    if overflow != 0:
        raise ValueError(f"incomplete exact-site table: overflow={overflow}")
    if source_sites == 0:
        raise ValueError("profile has no sites for compiled source")
    return {"profile_runtimes": runtimes, "profile_sites": sites,
            "profile_source_sites": source_sites, "site_overflow": overflow}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True,
                        help="normal classic-bytecode binary")
    parser.add_argument("--profile-tool", type=Path, required=True,
                        help="CONFIG_OPCODE_PROFILE classic-bytecode binary")
    parser.add_argument("--passes", default="0xffffffff")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--cpuset", default="2")
    parser.add_argument("--program", action="append", default=[],
                        help="program name or filename; repeat to select")
    parser.add_argument("--resume", action="store_true",
                        help="continue a matching interrupted collection")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    corpus_manifest_path = args.corpus / "manifest.json"
    corpus_manifest = json.loads(
        corpus_manifest_path.read_text(encoding="utf-8"))
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
    fresh_manifest: dict[str, object] = {
        "schema": "capsid-classic-suite-profile-v1",
        "command": sys.argv,
        "git_head": subprocess.run(
            ["git", "rev-parse", "HEAD"], check=True, text=True,
            stdout=subprocess.PIPE).stdout.strip(),
        "git_diff_sha256": hashlib.sha256(subprocess.run(
            ["git", "diff", "--no-ext-diff", "--binary"], check=True,
            stdout=subprocess.PIPE).stdout).hexdigest(),
        "platform": platform.platform(),
        "cpuset": args.cpuset,
        "timeout_seconds": args.timeout,
        "passes": args.passes,
        "corpus_manifest_sha256": sha256(corpus_manifest_path),
        "compiler": {"path": str(args.compiler.resolve()),
                     "sha256": sha256(args.compiler)},
        "profile_tool": {"path": str(args.profile_tool.resolve()),
                         "sha256": sha256(args.profile_tool)},
        "programs": programs,
        "results": [],
    }
    manifest_path = args.out / "manifest.json"
    if args.resume and manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for key in ("schema", "cpuset", "passes",
                    "corpus_manifest_sha256", "compiler", "profile_tool"):
            if manifest.get(key) != fresh_manifest.get(key):
                parser.error(f"cannot resume: manifest {key} differs")
        manifest.setdefault("resume_commands", []).append(sys.argv)
    else:
        manifest = fresh_manifest
    results: list[dict[str, object]] = manifest["results"]  # type: ignore[assignment]
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    # Resume only completed evidence. Failed/partial rows must be retried and
    # replaced, otherwise a timeout is permanently treated as a completed
    # program and the final failure ledger can never become green.
    successful_programs = {
        str(row["program"]) for row in results
        if row.get("compile_returncode") == 0 and
        row.get("execute_returncode") == 0 and
        "profile_sha256" in row
    }
    results[:] = [row for row in results
                  if str(row["program"]) in successful_programs]
    prior_programs = successful_programs

    for index, program in enumerate(programs, 1):
        source = args.corpus / program["file"]
        stem = Path(program["file"]).stem
        if stem in prior_programs:
            print(f"[{index}/{len(programs)}] resume-skip {stem}", flush=True)
            continue
        bytecode = bytecode_dir / f"{stem}.rewrite.qjsb"
        profile = args.out / f"{stem}.opt.profile.jsonl"
        profile_tmp = args.out / f"{stem}.opt.profile.tmp.jsonl"
        failed_profile = args.out / f"{stem}.opt.failed.jsonl"
        for stale in (profile, profile_tmp, failed_profile):
            stale.unlink(missing_ok=True)
        compile_stdout = args.out / f"{stem}.compile.stdout"
        compile_stderr = args.out / f"{stem}.compile.stderr"
        compile_command = [
            str(args.compiler), "compile", "--input", str(source),
            "--source-name", f"file:///{program['file']}",
            "--output", str(bytecode), "--rewrite", "--passes",
            args.passes, "--report",
        ]
        print(f"[{index}/{len(programs)}] compile {stem}", flush=True)
        compile_rc = run(compile_command, args.timeout,
                         compile_stdout, compile_stderr)
        source_name = f"file:///{program['file']}"
        row: dict[str, object] = {"program": stem,
                                  "source_name": source_name,
                                  "compile_returncode": compile_rc}
        if compile_rc == 0:
            execute_command = [
                "taskset", "-c", args.cpuset, str(args.profile_tool), "run",
                "--input", str(bytecode), "--warmup", "0", "--rounds", "1",
                "--expect-global-true", "__capsidSuiteOk",
                "--opcode-profile", str(profile_tmp),
            ]
            print(f"[{index}/{len(programs)}] profile {stem}", flush=True)
            execute_rc = run(execute_command, args.timeout,
                             args.out / f"{stem}.run.jsonl",
                             args.out / f"{stem}.run.stderr")
            row["execute_returncode"] = execute_rc
            if execute_rc == 0 and profile_tmp.exists():
                try:
                    row.update(validate_profile(profile_tmp, source_name))
                except (OSError, UnicodeError, ValueError) as error:
                    row["profile_validation_error"] = str(error)
                    row["execute_returncode"] = 125
                    profile_tmp.replace(failed_profile)
                else:
                    profile_tmp.replace(profile)
                    row["profile_sha256"] = sha256(profile)
                    row["profile_bytes"] = profile.stat().st_size
            elif profile_tmp.exists():
                # Keep failed/partial evidence for diagnosis, but outside the
                # *.profile.jsonl glob consumed by ranking tools.
                profile_tmp.replace(failed_profile)
        results.append(row)
        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    failures = [row for row in results
                if row.get("compile_returncode") != 0 or
                row.get("execute_returncode") != 0 or
                "profile_sha256" not in row]
    complete = len(results) - len(failures)
    print(json.dumps({"requested": len(programs), "complete": complete,
                      "failures": failures}, indent=2))
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
