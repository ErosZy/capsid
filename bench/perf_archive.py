#!/usr/bin/env python3
"""Restore complete sealed performance archives from bounded, hashed transports.

Compression is only transport: callers must still run the ordinary lane/point
validators. No measured metadata, raw bytes, checksums or budgets are repaired.
The destination is new and retained on failure for inspection.
"""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import tarfile

from perf_evidence import EvidenceError, SHA256, is_link, need, object_pairs, relative_name, sha256

MAX_FILES = 20000
MAX_MEMBER_BYTES = 256 * 1024 * 1024
MAX_TOTAL_BYTES = 8 * 1024 * 1024 * 1024
MAX_TRANSPORT_BYTES = 512 * 1024 * 1024


def positive(value, label, maximum):
    need(type(value) is int and 0 < value <= maximum, f"{label}: invalid bound")
    return value


def load_index(repo):
    repo = Path(repo).resolve()
    path = repo / "perf-site/archive-index.json"
    need(
        path.is_file()
        and not is_link(path)
        and path.resolve().is_relative_to(repo)
        and not is_link(path.parent)
        and path.stat().st_size <= 1024 * 1024,
        "missing or oversized packed performance index",
    )
    index = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=object_pairs)
    need(
        isinstance(index, dict)
        and index.get("schema") == 1
        and index.get("contract") == "capsid-perf-packed-archives-v1",
        "unsupported packed performance contract",
    )
    bundles = index.get("bundles")
    need(
        isinstance(bundles, list) and 0 < len(bundles) <= 30,
        "invalid packed performance bundle list",
    )
    prefixes, parts_seen = set(), set()
    total_files = total_bytes = 0
    for bundle in bundles:
        need(isinstance(bundle, dict), "invalid packed bundle")
        prefix = bundle.get("prefix")
        relative_name(prefix)
        need(
            prefix.startswith("bench/results/") and prefix not in prefixes,
            "duplicate or non-performance archive prefix",
        )
        prefixes.add(prefix)
        need(bundle.get("lane") in ("cold-start", "qps", "density"), "unknown packed lane")
        total_files += positive(bundle.get("files"), "packed file count", MAX_FILES)
        total_bytes += positive(
            bundle.get("uncompressedBytes"), "uncompressed bytes", MAX_TOTAL_BYTES
        )
        transport_bytes = positive(bundle.get("bytes"), "compressed bytes", MAX_TRANSPORT_BYTES)
        need(SHA256.fullmatch(str(bundle.get("sha256", ""))), "invalid compressed digest")
        parts = bundle.get("parts")
        need(isinstance(parts, list) and 0 < len(parts) <= 32, "invalid transport parts")
        size = 0
        for part in parts:
            need(isinstance(part, dict), "invalid transport part")
            name = part.get("path")
            relative_name(name)
            need(
                name.startswith("perf-site/archives/") and name not in parts_seen,
                "duplicate or misplaced transport part",
            )
            parts_seen.add(name)
            size += positive(part.get("bytes"), "part bytes", MAX_TRANSPORT_BYTES)
            need(SHA256.fullmatch(str(part.get("sha256", ""))), "invalid part digest")
        need(size == transport_bytes, "transport part lengths disagree")
    need(
        total_files <= MAX_FILES and total_bytes <= MAX_TOTAL_BYTES,
        "packed archive aggregate exceeds bound",
    )
    return index


def materialize(repo, destination, required_prefixes=None):
    """Restore regular files to a new owned root; never execute/extract links."""
    repo, destination = Path(repo).resolve(), Path(destination).absolute()
    index = load_index(repo)
    prefixes = {b["prefix"] for b in index["bundles"]}
    if required_prefixes is not None:
        need(prefixes == set(required_prefixes), "packed archives do not match verified points")
    need(not destination.exists() and not is_link(destination), "archive destination must be new")
    destination.mkdir(parents=True)
    joined_root = destination / ".transport"
    joined_root.mkdir()
    inventory, reports, portable_inventory = {}, [], set()
    for number, bundle in enumerate(index["bundles"]):
        joined = joined_root / (str(number) + ".tar.gz")
        combined = hashlib.sha256()
        with joined.open("xb") as target:
            for part in bundle["parts"]:
                source = repo / part["path"]
                need(
                    source.is_file()
                    and not is_link(source)
                    and source.resolve().is_relative_to(repo)
                    and not any(
                        is_link(parent) for parent in source.parents if parent.is_relative_to(repo)
                    )
                    and source.stat().st_size == part["bytes"],
                    "missing or changed transport part",
                )
                current = hashlib.sha256()
                with source.open("rb") as stream:
                    while data := stream.read(1024 * 1024):
                        current.update(data)
                        combined.update(data)
                        target.write(data)
                need(current.hexdigest() == part["sha256"], "transport part digest mismatch")
        need(
            joined.stat().st_size == bundle["bytes"] and combined.hexdigest() == bundle["sha256"],
            "combined transport digest mismatch",
        )
        names, portable_names, total = set(), set(), 0
        try:
            with joined.open("rb") as raw, gzip.GzipFile(fileobj=raw, mode="rb") as decoded:
                # Bound padding/headers and trailing decompression, not just files.
                maximum = bundle["uncompressedBytes"] + bundle["files"] * 4096 + 1024 * 1024

                class BoundedStream:
                    size = 0

                    def read(self, count):
                        need(count >= 0, "unbounded archive read")
                        data = decoded.read(min(count, maximum - self.size + 1))
                        self.size += len(data)
                        need(self.size <= maximum, "decompressed transport exceeds bound")
                        return data

                bounded = BoundedStream()

                class BoundedTarInfo(tarfile.TarInfo):
                    # CPython's processing extension point runs before PAX
                    # payload allocation. Some patched 3.12 builds bypass the
                    # public frombuf method internally; do not hook only that.
                    pending_pax = False
                    headers = 0

                    def _proc_member(self, archive):
                        cls = type(self)
                        cls.headers += 1
                        need(cls.headers <= bundle["files"] * 2, "too many packed headers")
                        if self.type == tarfile.XHDTYPE:
                            need(
                                not cls.pending_pax and 0 <= self.size <= 8192,
                                "oversized or repeated packed metadata header",
                            )
                            cls.pending_pax = True
                        else:
                            need(
                                self.type in (tarfile.REGTYPE, tarfile.AREGTYPE),
                                "non-regular packed member",
                            )
                            need(0 <= self.size <= MAX_MEMBER_BYTES, "packed member too large")
                            cls.pending_pax = False
                        return super()._proc_member(archive)

                with tarfile.open(fileobj=bounded, mode="r|", tarinfo=BoundedTarInfo) as archive:
                    for member in archive:
                        name = member.name
                        relative_name(name)
                        need(
                            name.startswith(bundle["prefix"] + "/"),
                            "member outside declared archive",
                        )
                        need(member.isfile() and not member.issparse(), "non-regular packed member")
                        need(
                            name not in names
                            and name.casefold() not in portable_names
                            and name.casefold() not in portable_inventory,
                            "duplicate or case-aliased packed member",
                        )
                        need(0 <= member.size <= MAX_MEMBER_BYTES, "packed member too large")
                        names.add(name)
                        portable_names.add(name.casefold())
                        portable_inventory.add(name.casefold())
                        total += member.size
                        need(
                            len(names) <= bundle["files"] and total <= bundle["uncompressedBytes"],
                            "packed archive exceeds declared bounds",
                        )
                        target = destination / name
                        target.parent.mkdir(parents=True, exist_ok=True)
                        need(
                            target.resolve().is_relative_to(destination.resolve())
                            and not any(
                                is_link(parent)
                                for parent in target.parents
                                if parent.is_relative_to(destination)
                            ),
                            "unsafe archive destination",
                        )
                        stream = archive.extractfile(member)
                        need(stream is not None, "missing packed member content")
                        digest, written = hashlib.sha256(), 0
                        with stream, target.open("xb") as output:
                            while data := stream.read(1024 * 1024):
                                written += len(data)
                                need(written <= member.size, "packed member size mismatch")
                                digest.update(data)
                                output.write(data)
                        need(
                            written == member.size and sha256(target) == digest.hexdigest(),
                            "restored member bytes changed",
                        )
                        inventory[name] = digest.hexdigest()
                # Consume the gzip trailer and verify its CRC too.
                while tail := bounded.read(1024 * 1024):
                    need(not any(tail), "unexpected non-padding archive trailer")
        except (tarfile.TarError, gzip.BadGzipFile, EOFError) as exc:
            raise EvidenceError("invalid packed archive stream") from exc
        need(
            len(names) == bundle["files"] and total == bundle["uncompressedBytes"],
            "packed archive omitted declared files or bytes",
        )
        reports.append({"prefix": bundle["prefix"], "files": len(names), "bytes": total})
    return {"root": str(destination), "files": inventory, "bundles": reports, "index": index}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    report = materialize(args.repo, args.output)
    print(
        json.dumps({"restoredFiles": len(report["files"]), "root": report["root"]}, sort_keys=True)
    )


if __name__ == "__main__":
    try:
        main()
    except (EvidenceError, OSError, ValueError, TypeError, KeyError) as exc:
        raise SystemExit(f"packed performance archive rejected: {exc}") from exc
