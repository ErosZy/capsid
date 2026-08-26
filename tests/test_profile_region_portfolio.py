#!/usr/bin/env python3
"""Directed parser contract for profile-region-portfolio.py."""

from __future__ import annotations

import importlib.util
import json
import resource
import sys
import tempfile
from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    path = root / "bench/profile-region-portfolio.py"
    spec = importlib.util.spec_from_file_location("capsid_region_portfolio", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    report = module.parse_report("""
bytecode region: census: 7 functions, rejected 1 functions / 9 insns, dynamic yes, missing sites 2
bytecode region:   u32_add_shr_dup candidates 3, insns 12, guards 6, slow bytes 12, predicted 15 (best 5)
bytecode region:     dynamic regions 101, insns 404, predicted 505
bytecode region:   length_lt candidates 4, insns 8, guards 8, slow bytes 8, predicted 4 (best 1)
bytecode region:     dynamic regions 20, insns 40, predicted 20
""")
    assert report["functions"] == 7
    assert report["rejected_functions"] == 1
    assert report["missing_profile_sites"] == 2
    assert report["templates"]["u32_add_shr_dup"]["dynamic_predicted"] == 505
    assert report["templates"]["length_lt"]["dynamic_regions"] == 20

    with tempfile.TemporaryDirectory() as temp:
        collection = Path(temp)
        manifest = {
            "programs": [{"file": "web-tooling-acorn.js"}],
            "results": [{"program": "web-tooling-acorn"}],
        }
        (collection / "manifest.json").write_text(json.dumps(manifest))
        names = module.source_names_from_manifest(collection)
        assert names == {
            "web-tooling-acorn": "file:///web-tooling-acorn.js"
        }
        manifest = {
            "source_name": "https://compat.example/framework-reference.js",
            "results": [{"framework": "hono"}, {"framework": "elysia"}],
        }
        (collection / "manifest.json").write_text(json.dumps(manifest))
        names = module.source_names_from_manifest(collection)
        assert names == {
            "hono": "https://compat.example/framework-reference.js",
            "elysia": "https://compat.example/framework-reference.js",
        }
    output = module.build_output(Path("/tmp/portfolio"), True, [
        {"program": "ok", "templates": report["templates"]},
        {"program": "bad", "error": "failed"},
    ])
    assert output["failures"] == 1
    assert output["aggregate"]["length_lt"]["dynamic_regions"] == 20
    limiter = module.analyzer_limit(256)
    assert limiter is not None
    # Do not apply it in the test process; verify the requested byte value by
    # temporarily replacing setrlimit.
    calls = []
    original = resource.setrlimit
    try:
        resource.setrlimit = lambda which, limits: calls.append((which, limits))
        limiter()
    finally:
        resource.setrlimit = original
    assert calls == [(resource.RLIMIT_AS, (256 * 1024 * 1024,) * 2)]
    print("test_profile_region_portfolio: all green")


if __name__ == "__main__":
    main()
