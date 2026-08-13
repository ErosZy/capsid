#!/usr/bin/env python3
"""Generate the fail-closed txiki.js upgrade evidence report."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 1
REQUIRED_AUDIT_TESTS = {
    "txiki_vendor_patch_integrity",
    "txiki_overlay_audit_negative_controls",
    "txiki_overlay_key_canonicalization",
    "worker_binary_audit",
    "worker_binary_audit_negative_controls",
    "worker_global_surface",
    "worker_denies_public_native_module",
    "worker_denies_internal_native_module",
    "worker_denies_capability_manifest_modules",
    "wpt_metadata_manifest",
    "wpt_metadata_negative_controls",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(arguments: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return result.stdout.strip()


def cache_values(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def artifact(path: Path, root: Path) -> dict[str, Any]:
    try:
        display = str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        display = str(path.resolve())
    return {
        "path": display,
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
    }


def vendor_evidence(root: Path, build: Path) -> dict[str, Any]:
    vendor = root / "vendor/txiki.js"
    tag = run(
        ["git", "describe", "--tags", "--exact-match", "HEAD"],
        cwd=vendor,
    )
    commit = run(["git", "rev-parse", "HEAD"], cwd=vendor)
    status = run(
        [
            "git",
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
            "--ignore-submodules=none",
        ],
        cwd=vendor,
    )
    if status:
        raise RuntimeError(f"vendor checkout is dirty:\n{status}")
    submodules = run(
        ["git", "submodule", "status", "--recursive"],
        cwd=vendor,
    ).splitlines()
    bad_submodules = [
        line
        for line in submodules
        if line.startswith(("+", "U"))
        or (
            line.startswith("-")
            and " deps/quickjs/test262 " not in f" {line} "
        )
    ]
    if bad_submodules:
        raise RuntimeError(
            f"vendor submodules are not pinned: {bad_submodules}"
        )
    patches = sorted((root / "patches/txiki").glob("*.patch"))
    # Keep in sync with cmake/AuditTxikiVendor.cmake, which applies the
    # same directory in order and freezes the count in its header comment.
    if len(patches) != 16:
        raise RuntimeError(f"expected 16 patches, found {len(patches)}")
    run(["git", "apply", "--check", *map(str, patches)], cwd=vendor)

    stamp_path = build / "vendor-overlay/txiki.js/.capsid-overlay-key"
    stamp_lines = stamp_path.read_text(encoding="utf-8").splitlines()
    if (
        len(stamp_lines) != 3
        or stamp_lines[0] != "schema=capsid-txiki-overlay-stamp-v1"
        or not re.fullmatch(r"key=[0-9a-f]{64}", stamp_lines[1])
        or not re.fullmatch(r"manifest=[0-9a-f]{64}", stamp_lines[2])
    ):
        raise RuntimeError("invalid txiki overlay stamp")
    return {
        "tag": tag,
        "commit": commit,
        "clean": True,
        "submodules": submodules,
        "patches": [
            {
                "name": patch.name,
                "bytes": patch.stat().st_size,
                "sha256": sha256(patch),
            }
            for patch in patches
        ],
        "combined_apply_check": "passed",
        "overlay": {
            "key": stamp_lines[1].split("=", 1)[1],
            "manifest": stamp_lines[2].split("=", 1)[1],
            "stamp_sha256": sha256(stamp_path),
        },
    }


def run_cmake_audits(
    root: Path,
    build: Path,
    cache: dict[str, str],
) -> dict[str, Any]:
    current_tag = run(
        ["git", "describe", "--tags", "--exact-match", "HEAD"],
        cwd=root / "vendor/txiki.js",
    )
    vendor_command = [
        "cmake",
        f"-DCAPSID_TXIKI_VENDOR={root / 'vendor/txiki.js'}",
        f"-DCAPSID_TXIKI_PATCH_DIR={root / 'patches/txiki'}",
        f"-DCAPSID_TXIKI_EXPECTED_TAG={current_tag}",
        (
            "-DCAPSID_TXIKI_OVERLAY_STAMP="
            f"{build / 'vendor-overlay/txiki.js/.capsid-overlay-key'}"
        ),
        (
            "-DCAPSID_TXIKI_PREPARE_SCRIPT="
            f"{root / 'cmake/PrepareTxiki.cmake'}"
        ),
        "-P",
        str(root / "cmake/AuditTxikiVendor.cmake"),
    ]
    restricted_command = [
        "cmake",
        f"-DCAPSID_WORKER={build / 'capsid-worker'}",
        f"-DCAPSID_TJS_ARCHIVE={build / 'txiki-build/libtjs_core.a'}",
        f"-DCAPSID_NM={cache['CMAKE_NM']}",
        f"-DCAPSID_AR={cache['CMAKE_AR']}",
        f"-DCAPSID_EXPECT_LTO={cache['CAPSID_ENABLE_LTO']}",
        "-P",
        str(root / "cmake/AuditRestrictedWorker.cmake"),
    ]
    return {
        "vendor_source_patch_overlay": {
            "passed": True,
            "output": run(vendor_command),
        },
        "restricted_worker_symbols": {
            "passed": True,
            "output": run(restricted_command),
        },
    }


def source_configuration(
    root: Path,
    build: Path,
    cache: dict[str, str],
) -> dict[str, Any]:
    expected = {
        "BUILD_TJS_RESTRICTED_CORE": "ON",
        "BUILD_WITH_FFI": "OFF",
        "BUILD_WITH_SQLITE": "OFF",
        "CAPSID_BUILD_SQLITE_BENCHMARK": "OFF",
        "CAPSID_ENABLE_LTO": "ON",
        "CAPSID_USE_MIMALLOC": "OFF",
    }
    actual = {key: cache.get(key) for key in expected}
    if actual != expected:
        raise RuntimeError(
            f"restricted Release configuration mismatch: {actual}"
        )
    sources = [
        root / "src/txiki_restricted_core.c",
        root / "src/worker_runtime.cc",
        root / "js/bootstrap.js",
        root / "js/profile-manifest.js",
        root / "docs/capability-manifest.json",
        build / "generated/bootstrap.js",
    ]
    return {
        "passed": True,
        "cache": actual,
        "source_artifacts": [
            artifact(path, root) for path in sources
        ],
    }


def ctest_evidence(
    build: Path,
    junit_path: Path,
) -> dict[str, Any]:
    catalog = json.loads(
        run(["ctest", "--test-dir", str(build), "--show-only=json-v1"])
    )
    registered = {test["name"]: test for test in catalog["tests"]}
    if "wpt_conformance_not_configured" in registered:
        raise RuntimeError("full matrix was configured without pinned WPT")
    xml_root = ET.parse(junit_path).getroot()
    cases = {case.attrib["name"]: case for case in xml_root.findall("testcase")}
    missing = sorted(set(registered) - set(cases))
    extra = sorted(set(cases) - set(registered))
    if missing or extra:
        raise RuntimeError(
            f"JUnit/catalog mismatch: missing={missing}, extra={extra}"
        )
    failed: list[str] = []
    skipped: list[str] = []
    passed: list[str] = []
    allowed_skips = {
        name
        for name, test in registered.items()
        if any(
            prop.get("name") == "SKIP_RETURN_CODE"
            and str(prop.get("value")) == "77"
            for prop in test.get("properties", [])
        )
    }
    for name, case in cases.items():
        if case.find("failure") is not None or case.find("error") is not None:
            failed.append(name)
        elif case.find("skipped") is not None:
            skipped.append(name)
        else:
            passed.append(name)
    unexpected_skips = sorted(set(skipped) - allowed_skips)
    if failed or unexpected_skips:
        raise RuntimeError(
            f"full matrix failed={failed}, unexpected_skips={unexpected_skips}"
        )
    missing_audits = sorted(REQUIRED_AUDIT_TESTS - set(passed))
    if missing_audits:
        raise RuntimeError(
            f"required audit tests did not pass: {missing_audits}"
        )
    wpt = sorted(
        name for name in registered if name.startswith("worker_wpt_")
    )
    if len(wpt) < 80:
        raise RuntimeError(f"expected fixed WPT matrix, found {len(wpt)} tests")
    return {
        "catalog_sha256": hashlib.sha256(
            json.dumps(catalog, sort_keys=True).encode()
        ).hexdigest(),
        "junit": {
            "path": str(junit_path.resolve()),
            "sha256": sha256(junit_path),
        },
        "registered": len(registered),
        "passed": len(passed),
        "passed_tests": sorted(passed),
        "skipped": sorted(skipped),
        "allowed_environment_skips": sorted(allowed_skips),
        "failed": [],
        "wpt_files": len(wpt),
        "required_audit_tests": sorted(REQUIRED_AUDIT_TESTS),
    }


def deviations(root: Path, baseline: dict[str, Any]) -> dict[str, Any]:
    path = root / "docs/conformance-deviations.md"
    current: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("| CAPSID-D"):
            continue
        cells = [cell.strip() for cell in line.split("|")[1:-1]]
        current[cells[0]] = cells[2]
    previous = baseline["conformance_deviations"]
    added = {
        key: current[key] for key in sorted(set(current) - set(previous))
    }
    removed = {
        key: previous[key] for key in sorted(set(previous) - set(current))
    }
    changed = {
        key: {"before": previous[key], "after": current[key]}
        for key in sorted(set(previous) & set(current))
        if previous[key] != current[key]
    }
    return {
        "source_sha256": sha256(path),
        "current": current,
        "diff": {
            "added": added,
            "removed": removed,
            "changed": changed,
        },
        "unchanged": not added and not removed and not changed,
    }


def surface_inventory(root: Path) -> dict[str, Any]:
    profile_path = root / "js/profile-manifest.js"
    profile_text = profile_path.read_text(encoding="utf-8")
    profile_match = re.search(
        r"profileId\s*=\s*'([^']+)'", profile_text
    )
    block_match = re.search(
        r"profileGlobalNames\s*=\s*Object\.freeze\(\[(.*?)\]\.sort\(\)\)",
        profile_text,
        re.DOTALL,
    )
    if not profile_match or not block_match:
        raise RuntimeError("cannot parse profile manifest")
    globals_list = re.findall(r"'([^']+)'", block_match.group(1))
    if len(globals_list) != len(set(globals_list)):
        raise RuntimeError("duplicate profile globals")

    capability_path = root / "docs/capability-manifest.json"
    capability = json.loads(capability_path.read_text(encoding="utf-8"))
    native_path = root / "src/txiki_restricted_core.c"
    native_text = native_path.read_text(encoding="utf-8")
    native_helpers = sorted(set(re.findall(r"\bcapsid_[a-z0-9_]+(?=\s*\()", native_text)))
    if not native_helpers:
        raise RuntimeError("restricted native helper inventory is empty")
    return {
        "profile": profile_match.group(1),
        "globals": {
            "count": len(globals_list),
            "names": sorted(globals_list),
            "manifest_sha256": sha256(profile_path),
            "contract_test": "worker_global_surface",
        },
        "modules": {
            "application_visible": capability["modules"][
                "built_and_available"
            ],
            "known_not_built": capability["modules"][
                "known_but_not_built"
            ],
            "permanently_forbidden": capability["modules"][
                "permanently_forbidden"
            ],
            "manifest_sha256": sha256(capability_path),
            "contract_tests": [
                "worker_denies_public_native_module",
                "worker_denies_internal_native_module",
                "worker_denies_capability_manifest_modules",
                "worker_global_surface",
            ],
        },
        "native": {
            "restricted_helper_count": len(native_helpers),
            "restricted_helpers": native_helpers,
            "source_sha256": sha256(native_path),
            "application_native_module": {
                "specifier": "capsid:permissions",
                "exports": ["permissions"],
                "methods": ["permissions.query"],
            },
            "bootstrap_only_module": "tjs:internal/core",
        },
    }


def render_report(document: dict[str, Any]) -> str:
    tests = document["tests"]
    deviation = document["conformance"]["deviations"]
    inventory = document["surface_inventory"]
    return "\n".join(
        [
            "# txiki.js 升级门禁报告",
            "",
            f"> 生成时间：{document['generated_at']}",
            f"> vendor：`{document['vendor']['tag']}` / "
            f"`{document['vendor']['commit']}`",
            "",
            "## 结论",
            "",
            "门禁通过。vendor、patch、overlay、受限源码与符号、完整测试矩阵、"
            "合规偏差差异和运行时表面清单已汇总到同一份 JSON 证据。",
            "",
            "## 测试",
            "",
            f"- 注册：{tests['registered']}",
            f"- 通过：{tests['passed']}",
            f"- 环境型 skip：{len(tests['skipped'])} "
            f"（{', '.join(tests['skipped']) or '无'}）",
            f"- 固定 WPT 文件：{tests['wpt_files']}",
            "",
            "## 合规与表面",
            "",
            f"- 偏差：{len(deviation['current'])} 项；相对 baseline "
            f"{'无变化' if deviation['unchanged'] else '有变化，见 JSON diff'}；",
            f"- 全局：{inventory['globals']['count']} 个，"
            "`worker_global_surface` 验证精确集合；",
            f"- 应用可见原生模块："
            f"{', '.join(inventory['modules']['application_visible'])}；",
            f"- 受限原生 helper："
            f"{inventory['native']['restricted_helper_count']} 个。",
            "",
            "权威明细、哈希、patch 列表、审计输出、完整全局/模块/原生清单和 "
            "JUnit 摘要均在同名 JSON 文件中。",
            "",
        ]
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--junit", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-md", type=Path, required=True)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    root = args.source_root.resolve()
    build = args.build_dir.resolve()
    cache = cache_values(build / "CMakeCache.txt")
    baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    document: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "state": "complete",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "baseline": {
            "path": str(args.baseline),
            "sha256": sha256(args.baseline),
        },
        "vendor": vendor_evidence(root, build),
        "restricted_source": source_configuration(root, build, cache),
        "audits": run_cmake_audits(root, build, cache),
        "tests": ctest_evidence(build, args.junit),
        "conformance": {
            "deviations": deviations(root, baseline),
            "wpt_manifest": artifact(root / "tests/wpt/manifest.json", root),
        },
        "surface_inventory": surface_inventory(root),
        "artifacts": {
            "worker": artifact(build / "capsid-worker", root),
            "tjs_archive": artifact(
                build / "txiki-build/libtjs_core.a", root
            ),
            "runtime_archive": artifact(
                build / "libcapsid_runtime.a", root
            ),
        },
    }
    expected_vendor = baseline["vendor"]
    document["vendor"]["baseline_diff"] = {
        "tag_changed": document["vendor"]["tag"] != expected_vendor["tag"],
        "commit_changed": (
            document["vendor"]["commit"] != expected_vendor["commit"]
        ),
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_md.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    args.output_md.write_text(render_report(document), encoding="utf-8")


if __name__ == "__main__":
    main()
