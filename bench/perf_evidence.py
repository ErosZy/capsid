"""Publication contract for newly measured performance archives.

This module deliberately does not upgrade legacy archives. A lane must write
lane.json and its raw evidence during measurement, then seal every file with
repository-relative SHA-256 entries. Parser fixtures are not publication input.
"""
from pathlib import Path, PurePosixPath
import datetime
import hashlib
import json
import math
import re
from cold_control import checked_round as check_cold_round, identity as cold_identity
from qps_control import checked_round as check_qps_round, checked_memory_pair, identity as qps_identity
from measurement_runtime import ProbeError


class EvidenceError(ValueError):
    pass


SIZES = ("10k", "100k", "1m")
WORKLOADS = ("matrix-json-1k", "matrix-json-16k", "stream")
STACKS = {
    "cold-start": ("capsid-source", "capsid-bytecode", "node", "deno", "workerd"),
    "qps": ("capsid", "workerd", "fastapi", "flask", "php"),
    "density": ("capsid", "workerd"),
}
PROTOCOL = {
    "cold-start": {"rounds": 5, "discardedRuns": 1, "warmRequests": 50, "httpClient": "python-http-exact-v1"},
    "qps": {"rounds": 5, "warmupSeconds": 5, "durationSeconds": 15,
            "connections": 64, "inflight": 64, "lowLoadRate": 10,
            "lowLoadSeconds": 15, "latencyRound": "last",
            "responseValidation": "exact-bytes-v2", "requestTimeoutMilliseconds": 30000,
            "memoryProtocol": "fresh-owned-forest-v1", "readinessProtocol": "exact-http-before-traffic-v1",
            "pssStableReads": 2, "pssMaxReads": 8, "pssIntervalSeconds": 1, "pssSettleSeconds": 1},
    "density": {"budgetMb": 2048, "warmRequests": 100, "stableReads": 2,
                "verificationRetries": 2},
}
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
COMMIT = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")


def need(condition, message):
    if not condition:
        raise EvidenceError(message)


def finite(value, label, minimum=0):
    need(type(value) in (int, float) and math.isfinite(value) and value >= minimum,
         f"{label}: expected a finite number >= {minimum}")
    return value


def integer(value, label, minimum=1):
    need(type(value) is int and value >= minimum,
         f"{label}: expected an integer >= {minimum}")
    return value


def text(value, label):
    need(isinstance(value, str) and value.strip() and
         value.strip().lower() not in ("absent", "unknown", "n/a"),
         f"{label}: missing identity")
    return value


def object_pairs(pairs):
    result = {}
    for key, value in pairs:
        need(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"),
                          object_pairs_hook=object_pairs,
                          parse_constant=lambda v: (_ for _ in ()).throw(
                              EvidenceError(f"{path}: non-finite JSON {v}")))
    except (OSError, ValueError) as exc:
        raise EvidenceError(f"{path}: {exc}") from exc


def timestamp(value, label):
    text(value, label)
    try:
        parsed = datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise EvidenceError(f"{label}: invalid timestamp") from exc
    need(parsed.tzinfo is not None and parsed.utcoffset() is not None,
         f"{label}: timezone is required")
    return parsed.astimezone(datetime.timezone.utc)


def relative_name(value):
    text(value, "evidence path")
    path = PurePosixPath(value)
    need(not path.is_absolute() and "\\" not in value and ":" not in value and
         all(part not in ("", ".", "..") for part in value.split("/")),
         f"invalid archive-relative path: {value}")
    return value


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def is_link(path):
    return path.is_symlink() or getattr(path, "is_junction", lambda: False)()


class Archive:
    def __init__(self, path, repo):
        supplied = Path(path).absolute()
        self.root = Path(path).resolve()
        self.repo = Path(repo).resolve()
        need(self.root.is_relative_to(self.repo / "bench" / "results"),
             f"{self.root}: publication archives must live under bench/results")
        for component in (supplied, *supplied.parents):
            if component == self.repo:
                break
            need(not is_link(component),
                 f"{component}: archive symlink or junction")
        self.prefix = self.root.relative_to(self.repo).as_posix()
        self.digests = {}
        sums = self.file("sha256sums.txt")
        checksum_bytes = sums.read_bytes()
        self.checksum_digest = hashlib.sha256(checksum_bytes).hexdigest()
        for line in checksum_bytes.decode("utf-8").splitlines():
            match = re.fullmatch(r"([0-9a-f]{64}) [ *](.+)", line)
            need(match is not None, f"{sums}: malformed SHA-256 entry")
            digest, name = match.groups()
            relative_name(name)
            need(name != "sha256sums.txt" and name not in self.digests,
                 f"{sums}: duplicate or self-referencing entry: {name}")
            need(sha256(self.file(name)) == digest,
                 f"{self.root / name}: SHA-256 mismatch")
            self.digests[name] = digest
        # Seal all files, not just the summary. Unlisted logs and samples often
        # expose an interrupted or accidentally combined run.
        need(self.inventory() == set(self.digests),
             f"{self.root}: SHA-256 inventory does not match archive files")
        self.manifest = self.json("lane.json")

    def inventory(self):
        actual = set()
        for path in self.root.rglob("*"):
            need(not is_link(path),
                 f"{path}: archive symlinks or junctions are not supported")
            if path.is_file() and path != self.root / "sha256sums.txt":
                actual.add(path.relative_to(self.root).as_posix())
        return actual

    def file(self, name):
        relative_name(name)
        path = self.root / name
        # Check every component before opening, including symlinks to files
        # which happen to resolve within the archive.
        current = self.root
        for part in PurePosixPath(name).parts:
            current = current / part
            need(not is_link(current), f"{current}: archive symlink or junction")
        need(path.resolve().is_relative_to(self.root) and path.is_file(),
             f"{path}: missing archive file")
        return path

    def sealed(self, name):
        relative_name(name)
        need(name in self.digests, f"{self.root / name}: missing SHA-256 entry")
        return self.file(name)

    def json(self, name):
        return read_json(self.sealed(name))

    def jsonl(self, name):
        path = self.sealed(name)
        rows = []
        for index, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            need(bool(line.strip()), f"{path}:{index}: empty sample")
            try:
                row = json.loads(line, object_pairs_hook=object_pairs,
                                 parse_constant=lambda v: (_ for _ in ()).throw(
                                     EvidenceError(f"non-finite JSON {v}")))
            except ValueError as exc:
                raise EvidenceError(f"{path}:{index}: {exc}") from exc
            need(isinstance(row, dict), f"{path}:{index}: sample must be an object")
            rows.append(row)
        need(bool(rows), f"{path}: no raw samples")
        return rows

    def evidence(self):
        result = {f"{self.prefix}/{k}": v for k, v in self.digests.items()}
        result[f"{self.prefix}/sha256sums.txt"] = self.checksum_digest
        return result

    def recheck(self):
        need(sha256(self.file("sha256sums.txt")) == self.checksum_digest and
             self.inventory() == set(self.digests),
             f"{self.root}: archive inventory changed during validation")
        for name, expected in self.digests.items():
            need(sha256(self.sealed(name)) == expected,
                 f"{self.root / name}: archive changed during validation")


def cell_key(lane, cell):
    need(isinstance(cell, dict), f"{lane}: cell must be an object")
    stack = cell.get("stack")
    need(stack in STACKS[lane], f"{lane}: unknown stack {stack}")
    field = "workload" if lane == "qps" else "size"
    value = cell.get(field)
    need(value in (WORKLOADS if lane == "qps" else SIZES),
         f"{lane}: unknown {field} {value}")
    if lane == "density":
        need(cell.get("phase") in ("cold", "warm"), "density: phase is required")
        return stack, value, cell["phase"]
    return stack, value


def expected_keys(lane):
    return {(s, v, p) if lane == "density" else (s, v)
            for s in STACKS[lane]
            for v in (WORKLOADS if lane == "qps" else SIZES)
            for p in (("cold", "warm") if lane == "density" else (None,))}


def required_binaries(lane, stack):
    if stack.startswith("capsid"):
        names = ["capsid-worker", "cold-start"] if lane == "cold-start" else [
            "capsid-host", "capsid-worker"]
        if stack == "capsid-bytecode":
            names.append("capsid-bytecode-compile")
    else:
        names = {"fastapi": ["python"], "flask": ["python"],
                 "php": ["php-fpm", "nginx"]}.get(stack, [stack])
    return set(names + (["loadgen"] if lane == "qps" else []))


def close(actual, expected, label, rounding=0):
    finite(actual, label)
    finite(expected, label)
    # Summary files print rounded values; they must still reconstruct from raw.
    need(abs(actual - expected) <= rounding + max(1e-9, abs(expected) * 1e-12),
         f"{label}: summary disagrees with raw samples ({actual} vs {expected})")


def median(values):
    ordered = sorted(values)
    need(bool(ordered), "missing samples for median")
    return ordered[(len(ordered) - 1) // 2]


def correctness(archive, names, expected):
    need(isinstance(names, list) and len(names) == expected and
         len(set(names)) == len(names), "missing or duplicate correctness files")
    verdicts = []
    for name in names:
        verdict = archive.json(name)
        need(isinstance(verdict, dict) and verdict.get("ok") is True and
             verdict.get("errors_as_success") is False,
             f"{name}: missing exact-response correctness verdict")
        integer(verdict.get("responses_checked"), f"{name}: responses_checked")
        for key in ("mismatches", "errors", "timeouts"):
            integer(verdict.get(key), f"{name}: {key}", 0)
            need(verdict[key] == 0, f"{name}: {key} is nonzero")
        verdicts.append(verdict)
    return verdicts


def validate_samples(archive, lane, record, summary):
    key = cell_key(lane, record)
    names = record.get("samples")
    rounds = 5 if lane != "density" else 1
    need(isinstance(names, list) and len(names) == (5 if lane == "qps" else 1) and
         len(set(names)) == len(names), f"{key}: missing or duplicate raw sample files")
    verdicts = correctness(archive, record.get("correctness"), rounds if lane == "qps" else 1)
    if lane == "qps":
        qps = []
        last = None
        for index, (name, verdict) in enumerate(zip(names, verdicts), 1):
            rows = archive.jsonl(name)
            try:
                last = check_qps_round(rows, verdict, key[0], key[1], index)
            except ProbeError as exc:
                raise EvidenceError(f"{name}: {exc}") from exc
            qps.append(last["qps"])
        close(summary["qps"], median(qps), f"{key}: qps", .05)
        close(summary["p50Ms"], last["p50_ms"], f"{key}: p50")
        close(summary["p95Ms"], last["p95_ms"], f"{key}: p95")
    elif lane == "cold-start":
        rows = archive.jsonl(names[0])
        need(len(rows) == 5 and all(type(r.get("round")) is int for r in rows) and
             [r["round"] for r in rows] == list(range(1, 6)),
             f"{key}: expected five distinct measured rounds")
        for row in rows:
            need(cell_key(lane, row) == key, f"{key}: wrong cold stack or size")
            need(row.get("ok") is True and type(row.get("warm_n")) is int and row["warm_n"] == 50,
                 f"{key}: failed response or incomplete warm phase")
            for field in ("total_ms", "ready_ms", "warm_ms"):
                finite(row.get(field), f"{key}: {field}")
            need(row["total_ms"] >= row["ready_ms"], f"{key}: invalid cold timing")
            finite(row.get("pss_kb"), f"{key}: pss_kb", 1)
            readings = row.get("pss_readings_kb")
            need(isinstance(readings, list) and len(readings) == 2 and
                 readings[0] == readings[1] == row["pss_kb"],
                 f"{key}: missing stable positive PSS readings")
            try:
                check_cold_round(row, 50)
            except ProbeError as exc:
                raise EvidenceError(f"cold-start {key}: {exc}") from exc
        discarded = record.get("discardedSamples")
        need(isinstance(discarded, list) and len(discarded) == 1, f"{key}: missing discarded cold run")
        discarded_rows = archive.jsonl(discarded[0])
        need(len(discarded_rows) == 1 and type(discarded_rows[0].get("round")) is int and discarded_rows[0]["round"] == 0 and
             cell_key(lane, discarded_rows[0]) == key, f"{key}: invalid discarded cold run")
        try:
            check_cold_round(discarded_rows[0], 50)
        except ProbeError as exc:
            raise EvidenceError(f"cold-start {key}: invalid discarded run: {exc}") from exc
        discard_verdict = correctness(archive, record.get("discardedCorrectness"), 1)[0]
        need(discard_verdict["responses_checked"] == 51, f"{key}: discarded correctness count disagrees with requests")
        roots = [cold_identity(row["measurementRoot"]) for row in discarded_rows + rows]
        need(len(set(roots)) == 6, f"{key}: cold worker process reused across rounds")
        need(verdicts[0]["responses_checked"] == 5 * 51,
             f"{key}: correctness count disagrees with cold/warm requests")
        for field, raw, rounding in (("totalMs", "total_ms", .005),
                                     ("readyMs", "ready_ms", .005),
                                     ("warmMs", "warm_ms", .0005),
                                     ("pssMb", "pss_kb", .05)):
            value = median([r[raw] for r in rows])
            close(summary[field], value / 1024 if field == "pssMb" else value,
                  f"{key}: {field}", rounding)
    else:
        rows = archive.jsonl(names[0])
        # This file is the final verification, not a search probe selected
        # afterwards. Search failures remain in the separately sealed log.
        need(len(rows) == 1, f"{key}: expected one final verification")
        row = rows[0]
        need(cell_key(lane, row) == key and row.get("status") == "ok" and
             row.get("finalVerification") is True, f"{key}: failed final verification")
        integer(row.get("count"), f"{key}: count")
        need(row["count"] == summary["count"] and row.get("unit") == summary["unit"],
             f"{key}: final pool identity disagrees with summary")
        readings = row.get("pss_readings_kb")
        need(isinstance(readings, list) and len(readings) == 2 and readings[0] == readings[1],
             f"{key}: unstable PSS")
        finite(readings[0], f"{key}: PSS", 1)
        need(readings[0] / 1024 <= 2048, f"{key}: final verification exceeded budget")
        close(summary["pssMb"], readings[0] / 1024, f"{key}: PSS", .05)
        coverage = row.get("requestsPerWorker")
        need(isinstance(coverage, dict) and len(coverage) == row["count"],
             f"{key}: missing per-worker request coverage")
        for worker, count in coverage.items():
            text(worker, f"{key}: worker identity")
            integer(count, f"{key}: requests per worker", 100 if key[2] == "warm" else 1)
        need(verdicts[0]["responses_checked"] == sum(coverage.values()),
             f"{key}: correctness count disagrees with worker coverage")
        need(row.get("limit") in ("budget", "cap"), f"{key}: missing search limit status")
        cap = archive.manifest["protocol"]["caps"][key[0]]
        need(row["count"] <= cap and (row["limit"] != "cap" or row["count"] == cap),
             f"{key}: final pool disagrees with configured cap")
        summary["limit"] = row["limit"]


def validate_qps_memory(archive, parsed):
    memory = archive.json(archive.manifest.get("memory"))
    need(isinstance(memory, dict) and set(memory) == set(STACKS["qps"]),
         "qps: missing memory sample records")
    for output in ("servingPssMb", "servingPss", "lowLoadPssMb", "lowLoadPss"):
        need(set(parsed.get(output, {})) == set(STACKS["qps"]),
             f"qps: missing {output} rows")
    intervals, all_processes, probes = [], set(), set()
    for stack, phases in memory.items():
        need(isinstance(phases, dict) and set(phases) == {"lowLoad", "serving"},
             f"qps: {stack}: missing memory phase")
        try:
            checked_memory_pair(phases, stack)
        except ProbeError as exc:
            raise EvidenceError(f"qps: {stack}: {exc}") from exc
        for sample in phases.values():
            intervals.append((sample['startedMonotonic'],sample['cleanupCompletedMonotonic']))
            members={qps_identity(p) for p in sample['measurementProcesses']}
            need(not all_processes & members,'qps: serving process reused across stacks/phases')
            all_processes.update(members);probes.add(qps_identity(sample['probeProcess']))
            proof=archive.json(sample.get('resultFile'))
            need(proof=={k:v for k,v in sample.items() if k!='resultFile'},'qps: memory differs from sealed probe result')
        for phase, sample in phases.items():
            need(isinstance(sample, dict) and sample.get("method") == "recursive-process-tree-pss",
                 f"qps: {stack}: missing recursive PSS sample")
            finite(sample.get("pss_kb"), f"qps: {stack}: PSS", 1)
            roles = sample.get("roles_kb")
            need(isinstance(roles, dict) and bool(roles), f"qps: {stack}: missing PSS roles")
            for role, value in roles.items():
                text(role, "PSS role")
                finite(value, f"qps: {stack}: {role} PSS", 1)
            close(sample["pss_kb"], sum(roles.values()), f"qps: {stack}: role sum")
            prefix = "lowLoad" if phase == "lowLoad" else "serving"
            close(parsed[prefix + "PssMb"][stack], sample["pss_kb"] / 1024,
                  f"qps: {stack}: {phase} PSS", .05)
            detail = parsed[prefix + "Pss"][stack]
            need({p["role"] for p in detail} == set(roles), f"qps: {stack}: role mismatch")
            for part in detail:
                close(part["mb"], roles[part["role"]] / 1024, f"qps: {stack}: role PSS", .05)
            if phase == "lowLoad":
                need(sample.get("beforeMeasuredLoad") is True and type(sample.get("validatedRequests")) is int and
                     sample.get("validatedRequests") == 150,
                     f"qps: {stack}: missing fresh low-load phase")
                finite(sample.get("duration_s"), f"qps: {stack}: low-load duration", 15)
        serving=phases['serving']; rounds=serving.get('records')
        need(isinstance(rounds,list) and len(rounds)==15,'qps: missing per-client load records')
        seen_clients=set()
        for record in archive.manifest['records']:
            if record['stack']!=stack:continue
            clients=record.get('clientProcesses')
            need(isinstance(clients,list) and len(clients)==5,'qps: missing per-round client identities')
            for index,client in enumerate(clients):
                client_id=qps_identity(client)
                need(client_id not in seen_clients,'qps: client process reused across rounds')
                seen_clients.add(client_id)
                matches=[v for v in rounds if isinstance(v,dict) and v.get('workload')==record['workload'] and v.get('round')==index+1]
                need(len(matches)==1,'qps: missing or duplicate client load record')
                item=matches[0]
                need(qps_identity(item.get('clientProcess'))==client_id and item.get('samples')==record['samples'][index] and
                     item.get('correctness')==record['correctness'][index] and
                     item.get('measurement')==archive.jsonl(record['samples'][index])[1],
                     'qps: client record differs from raw load-generator evidence')
        need(seen_clients=={qps_identity(p) for p in serving['loadgenProcesses']},'qps: memory omits measured clients')
    ordered=sorted(intervals)
    need(len(probes)==1 and all(a[1]<b[0] for a,b in zip(ordered,ordered[1:])),
         'qps: serving forests overlap or have different controllers')


def validate_qps_dependencies(archive):
    dependencies=archive.json(archive.manifest.get('runtimeDependencies'))
    need(isinstance(dependencies,dict) and set(dependencies)=={'python'},'qps: missing Python framework provenance')
    python=dependencies['python']
    need(isinstance(python,dict) and python.get('executableSHA256')==archive.manifest['binaries']['python']['sha256'],
         'qps: framework interpreter conflicts with measured binary')
    text(python.get('version'),'Python runtime version')
    packages,files=python.get('packages'),python.get('files')
    need(isinstance(packages,list) and packages and isinstance(files,list) and files,'qps: missing installed framework files')
    normalize=lambda value:value.lower().replace('_','-').replace('.','-')
    names=set()
    for package in packages:
        need(isinstance(package,dict),'qps: invalid package identity')
        name=normalize(text(package.get('name'),'framework package'));text(package.get('version'),'framework version')
        need(name not in names,'qps: duplicate installed package');names.add(name)
    required={'fastapi','uvicorn','starlette','flask','gunicorn'}
    need(required<=names,'qps: required reference framework is absent')
    covered=set()
    for file in files:
        need(isinstance(file,dict),'qps: invalid installed package file')
        name=normalize(text(file.get('package'),'file package'))
        need(name in names and SHA256.fullmatch(str(file.get('sha256',''))),'qps: unknown package file identity')
        text(file.get('logicalPath'),'package logical path')
        need(sha256(archive.sealed(file.get('file')))==file['sha256'],'qps: installed framework file digest mismatch')
        covered.add(name)
    need(required<=covered,'qps: installed framework code is missing')


def validate_text_manifest(archive):
    m = archive.manifest
    values = {}
    for line in archive.sealed("manifest.txt").read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition(":")
        if separator:
            need(key not in values, f"manifest.txt: duplicate field {key}")
            values[key] = value.strip()
    expected = {"commit": m["origin"]["commit"], "tag": m["origin"]["tag"],
                "cpu": m["machine"]["cpu"], "uname": m["machine"]["kernel"],
                "nproc": str(m["machine"]["logicalCpus"]), "generated_at": m["startedAt"]}
    expected.update({"binary." + k + ".sha256": v["sha256"] for k, v in m["binaries"].items()})
    expected.update({"fixture." + k + ".sha256": v["sha256"] for k, v in m["fixtures"].items()})
    for key, value in expected.items():
        need(values.get(key) == value, f"manifest.txt: missing or conflicting {key}")
    text(values.get("command"), "manifest.txt: command")
    need(values["command"] == m.get("command"), "manifest.txt: conflicting command")


def summaries(lane, parsed):
    if lane != "density":
        return {cell_key(lane, cell): cell for cell in parsed["cells"]}
    result = {}
    for stack, cells in parsed.items():
        for label, cell in cells.items():
            phase, size = label.split("@")
            key = cell_key(lane, {"stack": stack, "size": size, "phase": phase})
            result[key] = cell
    return result


def validate_lane(path, repo, lane, parsed, version, release):
    archive = Archive(path, repo)
    need("failed.json" not in archive.digests, f"{lane}: failed run cannot qualify publication")
    m = archive.manifest
    need(isinstance(m, dict) and type(m.get("schema")) is int and m["schema"] == 1 and m.get("lane") == lane and
         m.get("status") == "complete", f"{path}: missing complete {lane} manifest")
    text(m.get("session"), f"{lane}: session")
    start = timestamp(m.get("startedAt"), f"{lane}: startedAt")
    end = timestamp(m.get("completedAt"), f"{lane}: completedAt")
    need(end >= start, f"{lane}: invalid measurement interval")
    origin = m.get("origin", {})
    need(isinstance(origin, dict) and COMMIT.fullmatch(str(origin.get("commit", ""))) and
         origin.get("tag") in ("", release) and origin.get("clean") is True and
         origin.get("version") == version and origin.get("release") == release,
         f"{lane}: missing or conflicting measured source identity")
    machine = m.get("machine", {})
    need(isinstance(machine, dict), f"{lane}: missing machine identity")
    for key in ("cpu", "kernel", "architecture", "hostId"):
        text(machine.get(key), f"{lane}: machine.{key}")
    integer(machine.get("logicalCpus"), f"{lane}: logicalCpus")
    protocol = m.get("protocol", {})
    need(isinstance(protocol, dict), f"{lane}: missing protocol")
    for key, value in PROTOCOL[lane].items():
        need(type(protocol.get(key)) is type(value) and protocol[key] == value,
             f"{lane}: unsupported protocol {key}")
    text(protocol.get("sutCpuSet"), f"{lane}: sutCpuSet")
    if lane == "density":
        caps = protocol.get("caps")
        need(isinstance(caps, dict) and set(caps) == set(STACKS[lane]), "density: missing configured caps")
        for stack, cap in caps.items():
            integer(cap, f"density: {stack} cap")
    posture = {"platform": "linux", "mode": "c-abi" if lane == "cold-start" else "managed",
               "sandbox": "strict"}
    need(m.get("posture") == posture, f"{lane}: unknown or non-production posture")
    proof = archive.json(m.get("isolation"))
    need(isinstance(proof, dict) and proof.get("session") == m["session"] and
         proof.get("posture") == posture and proof.get("origin") == origin and
         proof.get("machine") == machine and proof.get("quietSession") is True and
         type(proof.get("ownedPidsRemaining")) is int and proof["ownedPidsRemaining"] == 0,
         f"{lane}: missing isolation or cleanup proof")
    need(m.get("summary") == "cells.txt", f"{lane}: unsupported summary file")
    archive.sealed("cells.txt")
    archive.sealed("manifest.txt")
    binaries = m.get("binaries", {})
    need(isinstance(binaries, dict) and binaries, f"{lane}: missing binary identities")
    for name, binary in binaries.items():
        need(isinstance(binary, dict) and SHA256.fullmatch(str(binary.get("sha256", ""))),
             f"{lane}: invalid {name} binary SHA-256")
        text(binary.get("version"), f"{lane}: {name} version")
        need(isinstance(binary.get("tag"), str) and isinstance(binary.get("commit"), str) and
             (binary["commit"] == "" or COMMIT.fullmatch(binary["commit"])) and
             (binary["commit"] or binary["tag"].strip()),
             f"{lane}: missing {name} source commit/tag")
        if not binary["commit"]:
            text(binary["tag"], f"{lane}: {name} source tag")
        if name.startswith("capsid-") or name in ("cold-start", "loadgen"):
            need(binary["commit"] == origin["commit"] and binary["version"] == version and
                 binary["tag"] == origin["tag"],
                 f"{lane}: {name} binary conflicts with measured source")
        if "file" in binary:
            need(sha256(archive.sealed(binary["file"])) == binary["sha256"],
                 f"{lane}: {name} archived binary SHA-256 mismatch")
    fixtures = m.get("fixtures", {})
    need(isinstance(fixtures, dict) and fixtures, f"{lane}: missing fixture identities")
    for name, fixture in fixtures.items():
        need(isinstance(fixture, dict) and SHA256.fullmatch(str(fixture.get("sha256", ""))),
             f"{lane}: invalid {name} fixture identity")
        need(sha256(archive.sealed(fixture.get("file"))) == fixture["sha256"],
             f"{lane}: {name} fixture SHA-256 mismatch")
    validate_text_manifest(archive)
    summary = summaries(lane, parsed)
    need(set(summary) == expected_keys(lane), f"{lane}: missing or unexpected summary rows")
    records = m.get("records")
    need(isinstance(records, list), f"{lane}: missing raw sample records")
    seen = set()
    used_files = set()
    for record in records:
        key = cell_key(lane, record)
        need(key not in seen, f"{lane}: duplicate record {key}")
        seen.add(key)
        names = record.get("binaries", [])
        need(isinstance(names, list) and all(isinstance(n, str) for n in names) and
             len(set(names)) == len(names) and
             required_binaries(lane, key[0]) <= set(names) <= set(binaries),
             f"{key}: missing measured binary identities")
        names = record.get("fixtures", [])
        need(isinstance(names, list) and bool(names) and all(isinstance(n, str) for n in names) and
             len(set(names)) == len(names) and
             set(names) <= set(fixtures), f"{key}: missing fixture identities")
        for field in (("samples", "correctness", "discardedSamples", "discardedCorrectness")
                      if lane == "cold-start" else ("samples", "correctness")):
            names = record.get(field)
            need(isinstance(names, list) and all(isinstance(n, str) for n in names),
                 f"{key}: missing {field}")
            need(not (used_files & set(names)), f"{key}: evidence reused across cells")
            used_files.update(names)
        need(key in summary, f"{lane}: unexpected raw record {key}")
        validate_samples(archive, lane, record, summary[key])
    need(seen == set(summary), f"{lane}: missing raw records for summary rows")
    if lane == "qps":
        try:
            validate_qps_memory(archive, parsed)
            validate_qps_dependencies(archive)
        except ProbeError as exc:
            raise EvidenceError(f'qps: {exc}') from exc
    return archive


def validate_point(lanes, repo, version, release):
    archives = {lane: validate_lane(path, repo, lane, parsed, version, release)
                for lane, (path, parsed) in lanes.items()}
    need({"cold-start", "qps"} <= set(archives), "cold-start and QPS lanes are required")
    first = archives["cold-start"].manifest
    binaries = {}
    fixtures = {}
    for lane, archive in archives.items():
        m = archive.manifest
        for key in ("session", "origin", "machine"):
            need(m[key] == first[key], f"{lane}: conflicting {key} across lanes")
        need(m["protocol"]["sutCpuSet"] == first["protocol"]["sutCpuSet"],
             f"{lane}: conflicting CPU affinity")
        for name, binary in m["binaries"].items():
            identity = {k: binary[k] for k in ("sha256", "commit", "tag", "version")}
            need(name not in binaries or binaries[name] == identity,
                 f"{lane}: conflicting {name} binary identity")
            binaries[name] = identity
        for name, fixture in m["fixtures"].items():
            need(name not in fixtures or fixtures[name] == fixture["sha256"],
                 f"{lane}: conflicting {name} fixture identity")
            fixtures[name] = fixture["sha256"]
    intervals = sorted((timestamp(a.manifest["startedAt"], "startedAt"),
                        timestamp(a.manifest["completedAt"], "completedAt"))
                       for a in archives.values())
    need(all(left[1] <= right[0] for left, right in zip(intervals, intervals[1:])),
         "lanes overlap; formal publication requires one quiet session")
    evidence = {}
    for archive in archives.values():
        need(not (evidence.keys() & archive.evidence().keys()), "duplicate archive paths")
        evidence.update(archive.evidence())
        archive.recheck()
    machine = dict(first["machine"], uname=first["machine"]["kernel"],
                   nproc=first["machine"]["logicalCpus"])
    for binary, key in (("node", "node"), ("deno", "deno"), ("workerd", "workerd_version"),
                        ("python", "python"), ("php-fpm", "php_fpm"), ("nginx", "nginx")):
        if binary in binaries:
            machine[key] = binaries[binary]["version"]
    return {
        "git": {key: first["origin"][key] for key in ("commit", "tag")},
        "machine": machine,
        "date": max(end for _, end in intervals).date().isoformat(),
        "origins": {lane: archive.manifest for lane, archive in archives.items()},
        "evidence": evidence,
        "verification": {"status": "verified", "contract": "capsid-perf-lane-v1",
                         "session": first["session"], "binaries": binaries,
                         "archives": {lane: a.prefix for lane, a in archives.items()}},
    }
