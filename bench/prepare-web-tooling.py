#!/usr/bin/env python3
"""Build a pinned V8 Web Tooling Benchmark corpus for classic-bytecode.

The upstream CLI measures all targets through Benchmark.js.  Capsid instead
builds one static target per script and executes its workload function once;
classic-suite-ab.py supplies the fresh-runtime repetitions and paired timing.
This avoids bundling every library into every target and avoids nested timing
samples while preserving the upstream library, payloads, and workload body.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any


TARGETS = (
    "acorn",
    "babel",
    "babel-minify",
    "babylon",
    "buble",
    "chai",
    "coffeescript",
    "espree",
    "esprima",
    "jshint",
    "lebab",
    "postcss",
    "prepack",
    "prettier",
    "source-map",
    "terser",
    "typescript",
    "uglify-js",
)


def run(command: list[str], cwd: Path, timeout: int = 300,
        env: dict[str, str] | None = None) -> str:
    result = subprocess.run(
        command, cwd=cwd, env=env, text=True, check=False,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
    )
    if result.returncode != 0:
        rendered = " ".join(command)
        raise RuntimeError(
            f"command failed ({result.returncode}): {rendered}\n{result.stdout}"
        )
    return result.stdout


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_output(root: Path, *args: str) -> str:
    return run(["git", *args], root).strip()


def committed_file(root: Path, name: str) -> bytes:
    result = subprocess.run(
        ["git", "show", f"HEAD:{name}"], cwd=root, check=True,
        stdout=subprocess.PIPE,
    )
    return result.stdout


def dependency_identity(root: Path) -> dict[str, Any]:
    output = run(["npm", "ls", "--depth=0", "--json"], root)
    tree = json.loads(output)
    dependencies = {
        name: row.get("version")
        for name, row in sorted(tree.get("dependencies", {}).items())
    }
    canonical = json.dumps(dependencies, sort_keys=True,
                           separators=(",", ":")).encode()
    return {
        "sha256": sha256_bytes(canonical),
        "dependencies": dependencies,
    }


def ensure_checkout(root: Path) -> dict[str, Any]:
    required = [
        root / "package.json",
        root / "package-lock.json",
        root / "webpack.config.js",
        root / "src",
        root / "third_party",
        root / "node_modules" / ".bin" / "webpack",
    ]
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise RuntimeError(
            "web-tooling checkout is not prepared; missing: " +
            ", ".join(missing) +
            ". Install its dependencies before building the corpus."
        )

    source_diff = git_output(
        root, "diff", "--name-only", "HEAD", "--", "src",
        "third_party", "webpack.config.js", "package.json",
    )
    if source_diff:
        raise RuntimeError(
            "refusing modified benchmark sources/config: " +
            ", ".join(source_diff.splitlines())
        )

    package = json.loads((root / "package.json").read_text(encoding="utf-8"))
    lock = root / "package-lock.json"
    return {
        "repository": "https://github.com/v8/web-tooling-benchmark",
        "git_head": git_output(root, "rev-parse", "HEAD"),
        "git_status": git_output(
            root, "status", "--porcelain", "--untracked-files=no"
        ).splitlines(),
        "version": package["version"],
        "package_lock_head_sha256": sha256_bytes(
            committed_file(root, "package-lock.json")
        ),
        "package_lock_worktree_sha256": sha256(lock),
    }


def ensure_helpers(root: Path, targets: list[str], timeout: int) -> dict[str, str]:
    helpers: list[tuple[str, Path]] = []
    if "terser" in targets:
        helpers.append(("build:terser-bundled", root / "build" /
                        "terser-bundled.js"))
    if "uglify-js" in targets:
        helpers.append(("build:uglify-js-bundled", root / "build" /
                        "uglify-js-bundled.js"))
    for script, path in helpers:
        if not path.exists():
            run(["npm", "run", script], root, timeout=timeout)
    return {path.name: sha256(path) for _script, path in helpers}


def node_environment(root: Path) -> tuple[dict[str, str], dict[str, str]]:
    node = run(["node", "--version"], root).strip()
    npm = run(["npm", "--version"], root).strip()
    major = int(node.removeprefix("v").split(".", 1)[0])
    env = os.environ.copy()
    if major >= 17:
        current = env.get("NODE_OPTIONS", "").strip()
        legacy = "--openssl-legacy-provider"
        if legacy not in current.split():
            env["NODE_OPTIONS"] = f"{current} {legacy}".strip()
    return env, {"node": node, "npm": npm}


def entry_source(target: str, target_path: Path) -> str:
    return f'''// Generated by Capsid's prepare-web-tooling.py.
const target = require({json.dumps(str(target_path))});
globalThis.__capsidSuiteOk = false;
globalThis.__capsidWebToolingResult = target.fn();
globalThis.__capsidSuiteOk = true;
'''


def config_source(upstream_config: Path, entry: Path, output: Path,
                  filename: str) -> str:
    return f'''const configs = require({json.dumps(str(upstream_config))});
module.exports = env => {{
  const config = configs(env)[0];
  config.entry = {json.dumps(str(entry))};
  config.output.path = {json.dumps(str(output))};
  config.output.filename = {json.dumps(filename)};
  return config;
}};
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--web-tooling", type=Path, required=True,
                        help="prepared v8/web-tooling-benchmark checkout")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--workload", action="append", choices=TARGETS, default=[],
        help="target to build; repeat to select (default: all)",
    )
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds allowed for each webpack/helper build")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    root = args.web_tooling.resolve()
    targets = list(dict.fromkeys(args.workload or TARGETS))
    upstream = ensure_checkout(root)
    env, tools = node_environment(root)
    helpers = ensure_helpers(root, targets, args.timeout)
    dependencies = dependency_identity(root)
    args.out.mkdir(parents=True, exist_ok=True)
    output = args.out.resolve()

    programs: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="capsid-web-tooling-") as temp:
        temporary = Path(temp)
        entry = temporary / "entry.js"
        config = temporary / "webpack.config.js"
        for target in targets:
            filename = f"web-tooling-{target}.js"
            target_path = root / "src" / f"{target}-benchmark.js"
            if not target_path.is_file():
                raise RuntimeError(f"missing upstream workload: {target_path}")
            entry.write_text(entry_source(target, target_path),
                             encoding="utf-8", newline="\n")
            config.write_text(
                config_source(root / "webpack.config.js", entry, output,
                              filename),
                encoding="utf-8", newline="\n",
            )
            log = run(
                [str(root / "node_modules" / ".bin" / "webpack"),
                 "--config", str(config), "--env.only", target],
                root, timeout=args.timeout, env=env,
            )
            (output / f"{filename}.webpack.log").write_text(
                log, encoding="utf-8", newline="\n"
            )
            bundle = output / filename
            programs.append({
                "suite": f"web-tooling-{upstream['version']}",
                "name": target,
                "file": filename,
                "sha256": sha256(bundle),
                "bytes": bundle.stat().st_size,
                "execution": "one upstream workload fn call per process",
            })
            print(f"built {target}: {bundle.stat().st_size} bytes")

    manifest = {
        "schema": "capsid-web-tooling-corpus-v1",
        "upstream": upstream,
        "environment": {**tools, "top_level_dependencies": dependencies},
        "generated_helpers": helpers,
        "harness": {
            "entry": "static per-workload webpack bundle",
            "iterations_per_process": 1,
            "timing": "external classic-suite-ab.py wall clock",
            "success_marker": "__capsidSuiteOk",
        },
        "programs": programs,
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(f"generated {len(programs)} programs in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
