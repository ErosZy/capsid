#!/usr/bin/env python3
"""Directed contract test for bench/profile_sequences.py."""

import importlib.util
import json
import pathlib
import sys
import tempfile


def load_module(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("capsid_profile_sequences", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def main() -> int:
    root = pathlib.Path(sys.argv[1]).resolve()
    module = load_module(root / "bench/profile_sequences.py")
    sizes = module.load_opcode_sizes(
        root / "vendor/txiki.js/deps/quickjs/quickjs-opcode.h")
    assert sizes["get_loc"] == 3
    assert sizes["get_field"] == 5
    assert sizes["push_1"] == 1

    sites = [
        {"function": 7, "pc": 0, "op": "get_loc", "exec": 100},
        {"function": 7, "pc": 3, "op": "get_field", "exec": 90,
         "prop": {"direct": 88, "accessor_or_generic": 2}},
        {"function": 7, "pc": 8, "op": "push_1", "exec": 90},
        {"function": 7, "pc": 9, "op": "add", "exec": 80},
        {"function": 7, "pc": 10, "op": "put_loc", "exec": 80},
        {"function": 7, "pc": 13, "op": "return_undef", "exec": 1},
        # Physically adjacent but separated by a control-flow boundary: no
        # sequence is allowed to include either side of the jump.
        {"function": 8, "pc": 0, "op": "push_1", "exec": 50},
        {"function": 8, "pc": 1, "op": "goto8", "exec": 50},
        {"function": 8, "pc": 3, "op": "add", "exec": 50},
    ]
    app_hash = module.source_hash("file:///app/fixture.js")
    for site in sites:
        site["source_hash"] = app_hash
    sites.append({
        "function": 99, "pc": 0, "op": "add", "exec": 1_000_000,
        "source_hash": module.source_hash("tjs:bootstrap"),
    })
    obj = {
        "schema": "quickjs-ng-opcode-profile-v3",
        "runtime": 11,
        "sites": sites,
        "site_overflow": 3,
    }
    with tempfile.TemporaryDirectory() as tmp:
        profile = pathlib.Path(tmp) / "fixture.opt.profile.jsonl"
        profile.write_text(json.dumps(obj) + "\n", encoding="utf-8")
        report = module.census(module.load_profiles(pathlib.Path(tmp), "opt"),
                               sizes, source_name="file:///app/fixture.js")

    assert report["schema"] == "capsid-opcode-sequence-census-v1"
    assert report["stats"]["runtimes"] == 1
    assert report["stats"]["site_overflow"] == 3
    assert report["stats"]["foreign_source_sites"] == 1
    rows = {tuple(row["pattern"]): row for row in report["sequences"]}
    full = ("get_loc", "get_field", "push_1", "add", "put_loc")
    assert full in rows
    assert rows[full]["region_exec"] == 80
    assert rows[full]["avoided_dispatches"] == 320
    assert rows[full]["ext_avoided_dispatches"] == 240
    assert rows[full]["property_direct"] == 88
    assert rows[full]["property_slow"] == 2
    assert rows[full]["program_count"] == 1
    assert rows[full]["program_files"] == ["fixture.opt.profile.jsonl"]
    assert rows[full]["evidence"][0]["function"] == 7
    assert not any("goto8" in pattern for pattern in rows)
    assert max(row["length"] for row in report["sequences"]) == 5
    print("test_profile_sequences: all green")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
