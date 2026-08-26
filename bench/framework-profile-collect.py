#!/usr/bin/env python3
"""Collect exact-site opcode profiles from all supported frameworks.

The existing differential suites are the workload driver. This deliberately
does not invent a one-route microbenchmark: each suite executes its complete
correctness vector set against a CONFIG_OPCODE_PROFILE worker, and the worker's
teardown dump is extracted from stderr. Instrumented timings are never used as
performance evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import subprocess
import sys
from pathlib import Path
from typing import Iterable


PROFILE_SCHEMAS = {"quickjs-ng-opcode-profile-v4"}
FRAMEWORK_SOURCE_NAME = "https://compat.example/framework-reference.js"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_hash(name: str) -> str:
    """Return the profile v3 64-bit FNV-1a source identity."""
    value = 14695981039346656037
    for byte in name.encode("utf-8")[:255]:
        value ^= byte
        value = (value * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


def extract_profiles(lines: Iterable[str]) -> list[dict]:
    profiles: list[dict] = []
    for raw in lines:
        line = raw.strip()
        if not line.startswith("{"):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if value.get("schema") in PROFILE_SCHEMAS:
            profiles.append(value)
    return profiles


def framework_matrix(root: Path, build: Path) -> list[dict[str, object]]:
    generated = build / "generated"
    return [
        {
            "name": "hono",
            "script": root / "tests/hono/differential.mjs",
            "driver": build / "test-hono-worker-driver",
            "bundle": generated / "test-hono-DEFAULT.js",
            "extra": [],
        },
        {
            "name": "elysia",
            "script": root / "tests/elysia/differential.mjs",
            "driver": build / "test-elysia-worker-driver",
            "bundle": generated / "test-elysia-DEFAULT.js",
            "extra": [],
        },
        {
            "name": "h3-v2",
            "script": root / "tests/h3-v2/differential.mjs",
            "driver": build / "test-h3-v2-worker-driver",
            "bundle": generated / "test-h3-v2-DEFAULT_APP.js",
            "extra": [],
        },
        {
            "name": "itty-router",
            "script": root / "tests/itty-router/differential.mjs",
            "driver": build / "test-itty-router-worker-driver",
            "bundle": generated / "test-itty-router-AUTOROUTER.js",
            "extra": ["--variant", "autorouter"],
        },
    ]


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=root / "build-profile")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--node", default="node")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--framework", action="append", default=[])
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    build = args.build.resolve()
    worker = build / "capsid-worker"
    compiler = build / "capsid-bytecode-compile"
    matrix = framework_matrix(root, build)
    selected = set(args.framework)
    if selected:
        unknown = selected - {str(row["name"]) for row in matrix}
        if unknown:
            parser.error("unknown framework(s): " + ", ".join(sorted(unknown)))
        matrix = [row for row in matrix if row["name"] in selected]

    required = [worker, compiler]
    for row in matrix:
        required.extend((Path(row["script"]), Path(row["driver"]),
                         Path(row["bundle"])))
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        parser.error("missing built input(s): " + ", ".join(missing))

    args.out.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, object] = {
        "schema": "capsid-framework-opcode-profile-v1",
        "command": sys.argv,
        "git_head": subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=root, check=True, text=True,
            stdout=subprocess.PIPE,
        ).stdout.strip(),
        "git_diff_sha256": hashlib.sha256(subprocess.run(
            ["git", "diff", "--no-ext-diff", "--binary"], cwd=root,
            check=True, stdout=subprocess.PIPE,
        ).stdout).hexdigest(),
        "platform": platform.platform(),
        "source_name": FRAMEWORK_SOURCE_NAME,
        "source_hash": source_hash(FRAMEWORK_SOURCE_NAME),
        "worker": {"path": str(worker), "sha256": sha256(worker)},
        "compiler": {"path": str(compiler), "sha256": sha256(compiler)},
        "mode": "rewritten-bytecode",
        "results": [],
    }
    manifest_path = args.out / "manifest.json"
    results: list[dict[str, object]] = manifest["results"]  # type: ignore[assignment]
    failures = 0
    bytecode_dir = args.out / "bytecode"
    bytecode_dir.mkdir(exist_ok=True)

    for index, row in enumerate(matrix, 1):
        name = str(row["name"])
        script = Path(row["script"])
        driver = Path(row["driver"])
        bundle = Path(row["bundle"])
        bytecode = bytecode_dir / f"{name}.rewrite.qjsb"
        compile_command = [
            str(compiler), "--source", str(bundle),
            "--source-name", FRAMEWORK_SOURCE_NAME,
            "--application", f"profile-{name}",
            "--version", "profile-v1", "--key-id", "profile-key",
            "--bytecode-out", str(bytecode),
            "--attestation-out", str(bytecode_dir / f"{name}.attestation.json"),
            "--signing-message-out", str(bytecode_dir / f"{name}.signing.bin"),
        ]
        compile_stdout = args.out / f"{name}.compile.stdout"
        compile_stderr = args.out / f"{name}.compile.stderr"
        with compile_stdout.open("w", encoding="utf-8") as stdout, \
                compile_stderr.open("w", encoding="utf-8") as stderr:
            compiled = subprocess.run(
                compile_command, cwd=root, check=False, text=True,
                stdout=stdout, stderr=stderr, timeout=args.timeout,
            )
        if compiled.returncode != 0:
            failures += 1
            results.append({
                "framework": name,
                "compile_returncode": compiled.returncode,
                "complete": False,
            })
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n",
                                     encoding="utf-8")
            print(f"{name}: bytecode compilation failed", file=sys.stderr)
            continue
        command = [
            args.node, str(script), "--driver", str(driver),
            "--worker", str(worker), "--bundle", str(bundle),
            "--runtime-bundle", str(bytecode),
            *[str(value) for value in row["extra"]],  # type: ignore[union-attr]
        ]
        stdout_path = args.out / f"{name}.stdout"
        stderr_path = args.out / f"{name}.stderr"
        profile_path = args.out / f"{name}.opt.profile.jsonl"
        profile_path.unlink(missing_ok=True)
        print(f"[{index}/{len(matrix)}] {name}: full differential profile",
              flush=True)
        try:
            with stdout_path.open("w", encoding="utf-8") as stdout, \
                    stderr_path.open("w", encoding="utf-8") as stderr:
                completed = subprocess.run(
                    command, cwd=root, check=False, text=True,
                    stdout=stdout, stderr=stderr, timeout=args.timeout,
                )
            returncode = completed.returncode
        except subprocess.TimeoutExpired:
            returncode = 124

        with stderr_path.open(encoding="utf-8", errors="replace") as stream:
            profiles = extract_profiles(stream)
        expected_hash = str(manifest["source_hash"])
        source_sites = sum(
            1 for profile in profiles for site in profile.get("sites", [])
            if str(site.get("source_hash", "")).lower() == expected_hash
        )
        overflows = sum(int(profile.get("site_overflow", 0))
                        for profile in profiles)
        if profiles:
            profile_path.write_text(
                "".join(json.dumps(profile, separators=(",", ":")) + "\n"
                        for profile in profiles),
                encoding="utf-8",
            )

        ok = (returncode == 0 and len(profiles) == 1 and source_sites > 0 and
              overflows == 0)
        result: dict[str, object] = {
            "framework": name,
            "compile_returncode": compiled.returncode,
            "returncode": returncode,
            "profile_count": len(profiles),
            "source_site_count": source_sites,
            "site_overflow": overflows,
            "complete": ok,
            "driver": {"path": str(driver), "sha256": sha256(driver)},
            "bundle": {"path": str(bundle), "sha256": sha256(bundle)},
            "bytecode": {"path": str(bytecode), "sha256": sha256(bytecode)},
        }
        if profile_path.exists():
            result["profile_sha256"] = sha256(profile_path)
            result["profile_bytes"] = profile_path.stat().st_size
        results.append(result)
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n",
                                 encoding="utf-8")
        if not ok:
            failures += 1
            print(f"{name}: incomplete: rc={returncode} profiles={len(profiles)} "
                  f"source_sites={source_sites} overflow={overflows}",
                  file=sys.stderr)

    print(json.dumps({"frameworks": len(matrix),
                      "complete": len(matrix) - failures,
                      "failures": failures}, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
