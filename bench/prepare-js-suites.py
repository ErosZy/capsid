#!/usr/bin/env python3
"""Build bounded, self-contained classic scripts from pinned JS suites.

This script never edits upstream checkouts.  It concatenates the official
suite parts and appends only a deterministic execution trailer.  Generated
sources stay outside git (normally under /tmp or bench/results) and the
manifest records the exact upstream revisions and generated SHA-256 values.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path


OCTANE = {
    "richards": ["richards.js"],
    "deltablue": ["deltablue.js"],
    "crypto": ["crypto.js"],
    "raytrace": ["raytrace.js"],
    "earley-boyer": ["earley-boyer.js"],
    "regexp": ["regexp.js"],
    "splay": ["splay.js"],
    "navier-stokes": ["navier-stokes.js"],
    "pdfjs": ["pdfjs.js"],
    "mandreel": ["mandreel.js"],
    "gameboy": ["gbemu-part1.js", "gbemu-part2.js"],
    "code-load": ["code-load.js"],
    "box2d": ["box2d.js"],
    "zlib": ["zlib.js", "zlib-data.js"],
    "typescript": ["typescript.js", "typescript-input.js",
                   "typescript-compiler.js"],
}


def git_head(path: Path) -> str:
    return subprocess.run(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        check=True, text=True, stdout=subprocess.PIPE,
    ).stdout.strip()


def write_script(path: Path, parts: list[Path], trailer: str) -> str:
    digest = hashlib.sha256()
    with path.open("w", encoding="utf-8", newline="\n") as output:
        for part in parts:
            chunk = (f"\n/* upstream: {part.name} */\n" +
                     part.read_text(encoding="utf-8") + "\n")
            output.write(chunk)
            digest.update(chunk.encode())
        output.write(trailer)
        digest.update(trailer.encode())
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kraken", type=Path, required=True)
    parser.add_argument("--octane", type=Path, required=True)
    parser.add_argument("--sunspider", type=Path, required=True,
                        help="JetStream SunSpider/resources directory")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--octane-iterations-cap", type=int, default=32)
    args = parser.parse_args()
    if args.octane_iterations_cap <= 0:
        parser.error("--octane-iterations-cap must be positive")
    args.out.mkdir(parents=True, exist_ok=True)

    manifest: dict[str, object] = {
        "schema": "capsid-classic-suite-corpus-v1",
        "upstream": {
            "kraken": git_head(args.kraken),
            "octane": git_head(args.octane),
            "sunspider": git_head(args.sunspider),
        },
        "octane_iterations_cap": args.octane_iterations_cap,
        "programs": [],
    }
    programs: list[dict[str, object]] = manifest["programs"]  # type: ignore[assignment]

    kraken = args.kraken / "tests" / "kraken-1.1"
    for name in kraken.joinpath("LIST").read_text().split():
        parts = []
        data = kraken / f"{name}-data.js"
        if data.exists():
            parts.append(data)
        parts.append(kraken / f"{name}.js")
        filename = f"kraken-{name}.js"
        sha = write_script(args.out / filename, parts,
                           "\nglobalThis.__capsidSuiteOk = true;\n")
        programs.append({"suite": "kraken-1.1", "name": name,
                         "file": filename, "sha256": sha})

    for source in sorted(args.sunspider.glob("*.js")):
        name = source.stem
        filename = f"sunspider-{name}.js"
        # The classic SunSpider harness timed a test by executing its
        # top-level body; the modern WebKit tree splits that driver away
        # and the tests are plain scripts (no Benchmark global).  Wrap the
        # body in a function and drive it for a fixed iteration count, so
        # the classic runner's wall-clock measurement is deterministic.
        wrapped = (
            "\n(function () {\n  var __ssRun = function () {\n" +
            f"\n/* upstream: {source.name} */\n" +
            source.read_text(encoding="utf-8") + "\n" +
            """  };
  globalThis.__capsidSuiteOk = false;
  for (var __ssIter = 0; __ssIter < 8; __ssIter++) __ssRun();
  globalThis.__capsidSuiteOk = true;
})();
""")
        (args.out / filename).write_text(wrapped, encoding="utf-8",
                                         newline="\n")
        sha = hashlib.sha256(wrapped.encode()).hexdigest()
        programs.append({"suite": "sunspider-1.0", "name": name,
                         "file": filename, "sha256": sha})

    base = args.octane / "base.js"
    for name, names in OCTANE.items():
        filename = f"octane-{name}.js"
        trailer = f"""
BenchmarkSuite.ResetRNG();
for (var __suiteIndex = 0;
     __suiteIndex < BenchmarkSuite.suites.length; __suiteIndex++) {{
  var __suite = BenchmarkSuite.suites[__suiteIndex];
  for (var __benchmarkIndex = 0;
       __benchmarkIndex < __suite.benchmarks.length; __benchmarkIndex++) {{
    var __benchmark = __suite.benchmarks[__benchmarkIndex];
    __benchmark.Setup();
    var __iterations = Math.min(__benchmark.deterministicIterations,
                                {args.octane_iterations_cap});
    for (var __iteration = 0; __iteration < __iterations; __iteration++)
      __benchmark.run();
    __benchmark.TearDown();
  }}
}}
globalThis.__capsidSuiteOk = true;
"""
        parts = [base] + [args.octane / item for item in names]
        sha = write_script(args.out / filename, parts, trailer)
        programs.append({"suite": "octane-2.0", "name": name,
                         "file": filename, "sha256": sha})

    manifest_path = args.out / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n",
                             encoding="utf-8")
    print(f"generated {len(programs)} programs in {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
