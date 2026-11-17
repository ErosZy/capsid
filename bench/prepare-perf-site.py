#!/usr/bin/env python3
"""Stage the static site and revalidated evidence in a new output directory.

Never modifies archives or perf-site/data.js, and never deploys anything.
Legacy points remain visible as unverified observations with no invented links.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

from perf_archive import materialize
from perf_evidence import EvidenceError, is_link, need, relative_name, validate_point

spec = importlib.util.spec_from_file_location(
    "perf_collector", Path(__file__).with_name("collect-perf-point.py")
)
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


def prepare_site(repo, data_path, output):
    repo, data_path, output = (
        Path(repo).resolve(),
        Path(data_path).resolve(),
        Path(output).absolute(),
    )
    source = repo / "perf-site"
    need(
        not output.exists() and not is_link(output) and not output.resolve().is_relative_to(source),
        "site output must be a new directory outside perf-site",
    )
    data_bytes = data_path.read_bytes()
    data = collector.load_existing(data_path)
    need(data_path.is_file(), "missing performance data")
    payloads = {}
    qualified, historical = 0, 0
    packed = None
    archive_repo = repo
    verified = []
    for point in data["points"]:
        verification = point.get("verification", {})
        need(isinstance(verification, dict), "invalid point verification status")
        if verification.get("status") == "verified":
            verified.append(point)
    if (source / "archive-index.json").exists():
        required = set()
        for point in verified:
            archives = point.get("verification", {}).get("archives")
            need(
                isinstance(archives, dict)
                and all(isinstance(value, str) for value in archives.values()),
                "missing publication archive paths",
            )
            required.update(archives.values())
        scratch_parent = Path(
            tempfile.mkdtemp(
                prefix="capsid-perf-replay-", dir=output.parent if output.parent.is_dir() else None
            )
        )
        packed = materialize(repo, scratch_parent / "archives", required)
        archive_repo = Path(packed["root"])
    verified_indexes = []
    replayed = 0
    for point in data["points"]:
        verification = point.get("verification", {})
        need(isinstance(verification, dict), "invalid point verification status")
        if verification.get("status") != "verified":
            historical += 1
            continue
        need(
            verification.get("contract") == "capsid-perf-lane-v1",
            "unknown performance evidence contract",
        )
        archives = verification.get("archives")
        need(
            isinstance(archives, dict)
            and {"cold-start", "qps"} <= set(archives) <= {"cold-start", "qps", "density"},
            "missing publication archive paths",
        )
        lanes = {}
        parsed = {"density": {}}
        for lane, prefix in archives.items():
            relative_name(prefix)
            path = archive_repo / prefix
            parse = {
                "cold-start": collector.parse_cold_start,
                "qps": collector.parse_qps,
                "density": collector.parse_density,
            }[lane]
            parsed[lane] = parse(path)
            lanes[lane] = path, parsed[lane]
        provenance = validate_point(lanes, archive_repo, point["version"], point["release"])
        for key, value in dict(
            provenance, coldStart=parsed["cold-start"], qps=parsed["qps"], density=parsed["density"]
        ).items():
            need(
                point.get(key) == value,
                f"{point['version']}: point {key} disagrees with archived measurements",
            )
        for name, expected in provenance["evidence"].items():
            path = archive_repo / name
            if packed is not None:
                # Same sealed-byte proof; transport compression never grants acceptance.
                need(
                    packed["files"].get(name) == expected,
                    f"{name}: packed evidence disagrees with validated point",
                )
                with path.open("rb") as stream:
                    current = hashlib.file_digest(stream, "sha256").hexdigest()
                need(current == expected, f"{name}: evidence changed while staging")
            else:
                content = path.read_bytes()
                need(
                    hashlib.sha256(content).hexdigest() == expected,
                    f"{name}: evidence changed while staging",
                )
                destination = "evidence/" + name
                need(
                    destination not in payloads or payloads[destination] == content,
                    f"{name}: conflicting evidence bytes across versions",
                )
                payloads[destination] = content
            replayed += 1
        verified_indexes.append(
            {
                "version": point["version"],
                "release": point["release"],
                "commit": point["git"]["commit"],
                "session": provenance["verification"]["session"],
                "evidence": provenance["evidence"],
            }
        )
        qualified += 1
    need((source / "index.html").is_file(), "missing performance site index")
    for path in source.rglob("*"):
        need(not is_link(path), f"{path}: unexpected site symlink")
        if path.is_file():
            name = path.relative_to(source).as_posix()
            need(
                name != "evidence" and not name.startswith("evidence/"),
                "perf-site contains stale staged evidence",
            )
            if name != "data.js":
                payloads[name] = path.read_bytes()
    need(data_path.read_bytes() == data_bytes, "performance data changed while staging")
    if packed is not None:
        # Rehash the exact downloadable transport bytes, after replay and copying.
        for bundle in packed["index"]["bundles"]:
            for part in bundle["parts"]:
                name = part["path"].removeprefix("perf-site/")
                need(
                    name in payloads
                    and len(payloads[name]) == part["bytes"]
                    and hashlib.sha256(payloads[name]).hexdigest() == part["sha256"],
                    "packed transport changed while staging",
                )
        payloads["evidence-index.json"] = (
            json.dumps(
                {
                    "schema": 1,
                    "transport": packed["index"],
                    "points": verified_indexes,
                },
                indent=2,
                sort_keys=True,
            )
            + "\n"
        ).encode("utf-8")
    payloads["data.js"] = data_bytes
    output.mkdir(parents=True)
    for name, content in sorted(payloads.items()):
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    return {
        "qualifiedPoints": qualified,
        "historicalPoints": historical,
        "evidenceFiles": replayed,
        "packedArchives": len(packed["index"]["bundles"]) if packed else 0,
        "output": str(output),
    }


def main():
    import json

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--data", type=Path)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    print(
        json.dumps(
            prepare_site(args.repo, args.data or args.repo / "perf-site/data.js", args.output),
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (EvidenceError, OSError, ValueError, TypeError, KeyError) as exc:
        print(f"performance site rejected: {exc}", file=sys.stderr)
        sys.exit(1)
