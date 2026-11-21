#!/usr/bin/env python3
"""Derive reader-facing status from committed source locks and captured evidence.

Does not contact GitHub, run a benchmark, qualify binaries, or change historical
records. Refresh release-status.json through a separately reviewed read-only
capture before regenerating. --check is the CI/read-only consistency mode.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def derive(root):
    release = load(root / "docs/data/release-status.json")
    beta = release["prerelease"]
    github_latest = release["githubLatest"]
    publication_path = beta["publicationEvidence"]
    publication = load(root / publication_path)
    wpt = load(root / "tests/wpt/manifest.json")
    reference = load(root / "tests/wpt/workerd-default-profile-v2.json")
    capability = load(root / "docs/capability-manifest.json")
    baseline = load(root / "docs/txiki-upgrade-baseline.json")
    review_path = "docs/data/performance-review-v030-beta5.json"
    review = load(root / review_path)
    measured_source = load(root / review["measuredSourceManifest"])
    header = (root / "include/capsid/runtime.h").read_text(encoding="utf-8")
    protocol = (root / "src/protocol.h").read_text(encoding="utf-8")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    version = re.search(r"\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", cmake)[1]
    abi = int(re.search(r"#define CAPSID_ABI_VERSION\s+(\d+)", header)[1])
    wire = int(re.search(r"\bkVersion\s*=\s*(\d+)", protocol)[1])
    assert release["readOnly"] is True and release["noReleaseOrTagMutation"] is True
    assert publication["published"] and publication["allRequiredCiGatesAndNativePackagesPassed"]
    assert publication["productCheckout"] == beta["sourceCommit"]
    assert publication["productTree"] == beta["sourceTree"]
    for key in ("tag", "releaseId", "draft", "prerelease", "githubLatest", "assets"):
        assert publication[key] == beta[key], (
            f"release capture differs from verified publication: {key}"
        )
    assert beta["draft"] is False and len(beta["assets"]) == 11
    assert beta["githubLatest"] == (github_latest["tag"] == beta["tag"])
    assert not (beta["prerelease"] and beta["githubLatest"])
    assert version == beta["runtimeVersion"] and abi == 7 and wire == 3
    # Bind the API/source description to its declared release, rather than just
    # relabeling a changed runtime tree with the old qualified commit.
    source_paths = ["src", "include", "js", "patches", "cmake", "CMakeLists.txt", "vendor"]
    source_manifest = load(root / "docs/data/runtime-source-manifest.json")
    assert source_manifest["schema"] == 2
    assert source_manifest["sourceCommit"] == publication["productCheckout"]
    assert source_manifest["sourceTree"] == publication["productTree"]
    checkout = source_manifest["checkout"]
    assert checkout["kind"] in {
        "docs-successor",
        "formatting-successor",
        "packaging-successor",
        "runtime-optimization-successor",
    }
    assert checkout["baseCommit"] == publication["productCheckout"]
    assert checkout["releaseQualificationInherited"] is False
    qualified_files = source_manifest["qualifiedReleaseFiles"]
    current_files = source_manifest["files"]
    declared_build_changes = checkout.get("buildInputChanges", {})
    declared_runtime_changes = checkout.get("runtimeInputChanges", {})
    if checkout["kind"] in {"docs-successor", "formatting-successor"}:
        assert not declared_build_changes, "docs/formatting cannot change build inputs"
        assert not declared_runtime_changes, "docs/formatting cannot change runtime inputs"
    elif checkout["kind"] == "runtime-optimization-successor":
        assert not declared_build_changes, "runtime JS optimization cannot change build inputs"
        assert declared_runtime_changes, (
            "runtime-optimization-successor requires explicit runtimeInputChanges"
        )
        assert all(name.startswith("js/") for name in declared_runtime_changes), (
            "runtime-optimization-successor scope is restricted to js/ hot-path files"
        )
        assert checkout.get("changeScope"), "runtime optimization scope must be described"
    else:
        assert not declared_runtime_changes, "packaging cannot change runtime inputs"
        assert set(declared_build_changes) == {
            "cmake/CapsidPackage.cmake",
            "cmake/CapsidPackagePlatform.cmake",
            "cmake/build_tests.cmake",
        }, "packaging changes require their explicit reviewed build-input scope"
        assert checkout.get("changeScope"), "packaging scope must be described separately"
    added_files = (set(declared_build_changes) | set(declared_runtime_changes)) - set(
        qualified_files
    )
    assert current_files.keys() == qualified_files.keys() | added_files
    actual_build_changes = set()
    actual_runtime_changes = set()
    for name, current in current_files.items():
        original = qualified_files.get(name)
        if original is not None:
            assert current["mode"] == original["mode"]
        if name.startswith(("patches/", "vendor/")):
            assert current == original, "locked vendor/patch input changed"
        elif name == "CMakeLists.txt" or name.startswith("cmake/"):
            if current != original:
                actual_build_changes.add(name)
                assert declared_build_changes.get(name) == {
                    "qualifiedGitObject": original["gitObject"] if original else None,
                    "currentGitObject": current["gitObject"],
                }, "build input differs from its reviewed change declaration"
        elif current != original:
            actual_runtime_changes.add(name)
            assert declared_runtime_changes.get(name) == {
                "qualifiedGitObject": original["gitObject"] if original else None,
                "currentGitObject": current["gitObject"],
            }, "runtime input differs from its reviewed change declaration"
    assert actual_build_changes == set(declared_build_changes)
    assert actual_runtime_changes == set(declared_runtime_changes)
    entries = subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "--stage", "-z", "--", *source_paths]
    ).decode("utf-8")
    current_entries = {}
    for record in entries.split("\0"):
        if record:
            fields, name = record.split("\t", 1)
            mode, oid, stage = fields.split()
            assert stage == "0", "unmerged runtime input"
            current_entries[name] = (mode, oid)
    assert current_entries == {
        name: (entry["mode"], entry["gitObject"])
        for name, entry in source_manifest["files"].items()
    }, "indexed runtime inputs differ from the reviewed development snapshot"
    check = subprocess.run(["git", "-C", str(root), "diff", "--quiet", "--", *source_paths])
    assert check.returncode == 0, (
        "working runtime inputs changed; update the qualification boundary explicitly"
    )
    sys.path.insert(0, str(root / "bench"))
    spec = importlib.util.spec_from_file_location(
        "documentation_perf_collector", root / "bench/collect-perf-point.py"
    )
    collector = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(collector)
    history = collector.load_existing(root / "perf-site/data.js")
    points = [p for p in history["points"] if p.get("verification", {}).get("status") == "verified"]
    assert points, "no validated measurement point to describe"
    point = max(points, key=lambda p: (p["date"], p["version"]))
    assert (
        review["wholeLocalSessionAccepted"] and review["productCheckout"] == point["git"]["commit"]
    )
    assert review["session"] == point["verification"]["session"]
    assert review["sourceTag"] == point["git"]["tag"] == measured_source["sourceTag"]
    assert measured_source["sourceCommit"] == point["git"]["commit"]
    assert measured_source["sourceTree"] == review["productTree"]
    assert measured_source["checkoutClean"] is True
    assert measured_source["gitEntries"] == review["measuredSourceFilesVerified"]
    assert review["pointVersion"] == point["version"] and review["pointRelease"] == point["release"]
    assert review["evidenceFiles"] == len(point["evidence"])
    assert review["measurementBuild"] == "local-rebuild"
    assert review["publishedReleaseAssetsMeasured"] is False
    matches_beta_source = (
        point["git"]["commit"] == beta["sourceCommit"]
        and point["git"]["tag"] == beta["tag"]
        and measured_source["sourceTree"] == beta["sourceTree"]
    )
    assert reference["wpt"]["commit"] == wpt["commit"]
    conflicts = [
        {"path": row["path"], "subtests": row["total"], "recordedConflicts": len(row["failures"])}
        for row in reference["conflicts"]
    ]
    runtime = {
        "sourceCommit": publication["productCheckout"],
        "sourceTree": publication["productTree"],
        "version": version,
        "publicAbi": abi,
        "defaultWireVersion": wire,
        "negotiatedWireVersion": 4,
        "txiki": baseline["vendor"],
        "quickjsCommit": baseline["quickjs"]["commit"],
        "sdkPatchCount": len(list((root / "patches/txiki").glob("*.patch"))),
        "userModules": capability["modules"]["built_and_available"],
        "capabilityPolicyVersion": capability["policy_version"],
        "headerSHA256": source_manifest["qualifiedReleaseDigests"]["headerSHA256"],
        "protocolSHA256": source_manifest["qualifiedReleaseDigests"]["protocolSHA256"],
        "sourceScope": f"qualified 0.3.0-beta interface baseline ({beta['tag']}); "
        + (
            "current checkout has declared JS runtime changes"
            if actual_runtime_changes
            else "current checkout runtime inputs match"
        ),
        "sourceManifest": "docs/data/runtime-source-manifest.json",
        "appPidLimitFieldEnforced": False,
    }
    qps = {
        cell["workload"]: cell["qps"] for cell in point["qps"]["cells"] if cell["stack"] == "capsid"
    }
    status = {
        "schema": 1,
        "asOfUTC": release["observedAtUTC"],
        "developmentBranch": release["developmentBranch"],
        "sourceChangeScope": checkout.get(
            "changeScope",
            "mechanical source formatting and development tooling; no new runtime binary qualification",
        ),
        "checkout": {
            **checkout,
            "headerSHA256": hashlib.sha256(header.encode("utf-8")).hexdigest(),
            "protocolSHA256": hashlib.sha256(protocol.encode("utf-8")).hexdigest(),
            "inputManifest": "docs/data/runtime-source-manifest.json",
        },
        "stable": release["stable"],
        "githubLatest": github_latest,
        "prerelease": beta,
        "runtime": runtime,
        "qualification": {
            "sourceCommit": publication["productCheckout"],
            "sourceFiles": publication["sourceFileCount"],
            "allRequiredCiGatesAndNativePackagesPassed": publication[
                "allRequiredCiGatesAndNativePackagesPassed"
            ],
            "sourceQualificationRuns": publication["sourceQualificationRuns"],
            "ordinarySuitePassed": publication["ordinarySuitePassed"],
            "standardsConformanceClaimed": publication["standardsConformanceClaimed"],
            "evidence": publication_path,
        },
        "conformance": {
            "profile": wpt["profile"],
            "wptCommit": wpt["commit"],
            "executedFiles": len(wpt["executedProfile"]),
            "referenceProfile": reference["id"],
            "rawFailingFiles": conflicts,
            "standardsConformanceClaimed": False,
        },
        "performance": {
            "date": point["date"],
            "sourceCommit": point["git"]["commit"],
            "sourceTree": review["productTree"],
            "sourceTag": point["git"]["tag"],
            "archiveReleaseLabel": point["release"],
            "displayLabel": f"{point['release']} local rebuild · {point['git']['commit'][:7]}",
            "measurementBuild": review["measurementBuild"],
            "matchesPublishedBetaSource": matches_beta_source,
            "representsPublishedRelease": review["publishedReleaseAssetsMeasured"],
            "publishedBetaPerformanceMeasured": review["publishedReleaseAssetsMeasured"]
            and matches_beta_source,
            "session": point["verification"]["session"],
            "machine": point["machine"],
            "cpuSet": point["origins"]["cold-start"]["protocol"]["sutCpuSet"],
            "physicalHostExclusivityClaimed": not review[
                "physicalWindowsHostExclusivityNotClaimed"
            ],
            "contract": point["verification"]["contract"],
            "evidenceFiles": len(point["evidence"]),
            "qps": qps,
            "lowLoadPssMiB": point["qps"]["lowLoadPssMb"]["capsid"],
            "servingPssMiB": point["qps"]["servingPssMb"]["capsid"],
            "density": point["density"]["capsid"],
            "independentReview": review_path,
            "measuredSourceManifest": review["measuredSourceManifest"],
        },
        "historicalPerformancePoints": [
            {
                "version": p["version"],
                "release": p.get("release"),
                "date": p["date"],
                "status": p.get("verification", {}).get(
                    "status", "unverified-historical-observation"
                ),
            }
            for p in history["points"]
            if p is not point
        ],
    }
    return status


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    root = args.repo.resolve()
    status = derive(root)
    encoded = json.dumps(status, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    archive_index = load(root / "perf-site/archive-index.json")
    assert archive_index["measurementCommit"] == status["performance"]["sourceCommit"]
    assert archive_index["session"] == status["performance"]["session"]
    outputs = {
        root / "docs/current-status.json": encoded,
        root
        / "perf-site/status.js": "// Generated by tools/refresh-documentation-data.py; not a new measurement.\nwindow.CAPSID_SITE_STATUS = "
        + encoded.rstrip()
        + ";\n",
        root
        / "perf-site/archive-index.js": "// Generated from archive-index.json; transport alone is not acceptance.\nwindow.CAPSID_PERF_ARCHIVES = "
        + json.dumps(archive_index, indent=2, sort_keys=True)
        + ";\n",
    }
    for path, content in outputs.items():
        if args.check:
            assert path.read_text(encoding="utf-8") == content, (
                f"{path}: stale generated documentation data"
            )
        else:
            path.write_text(content, encoding="utf-8", newline="\n")
    print(
        json.dumps(
            {
                "status": "consistent" if args.check else "generated",
                "runtimeVersion": status["runtime"]["version"],
                "measuredCommit": status["performance"]["sourceCommit"],
                "rawWptFailureFiles": len(status["conformance"]["rawFailingFiles"]),
            },
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
