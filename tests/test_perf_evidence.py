#!/usr/bin/env python3
"""Synthetic contract fixtures only; these numbers are never published.

Exercise the real collector and independently mutate raw samples, provenance,
digests and history. No benchmark process or network service is required.
"""
import argparse
from contextlib import contextmanager, redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--collector", type=Path, default=Path(__file__).resolve().parents[1] / "bench/collect-perf-point.py")
args, remaining = parser.parse_known_args()
sys.argv[1:] = remaining
sys.path.insert(0, str(args.collector.resolve().parent))
spec = importlib.util.spec_from_file_location("perf_collector", args.collector)
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)
import perf_evidence as contract
sys.path.insert(0, str(Path(__file__).resolve().parent / "helpers"))
from cold_measurement_fixture import synthetic_round
from qps_measurement_fixture import synthetic_memory
from density_measurement_fixture import synthetic_density
site_spec = importlib.util.spec_from_file_location("perf_site", args.collector.with_name("prepare-perf-site.py"))
site = importlib.util.module_from_spec(site_spec)
site_spec.loader.exec_module(site)


def encoded(value):
    return (json.dumps(value, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


def digest(data):
    return hashlib.sha256(data).hexdigest()


class Fixture:
    """Every required cell has independent raw and correctness files."""
    def __init__(self, root):
        self.root = root
        self.paths = {}
        self.manifests = {}
        self.saved = None
        origin = {"commit": "a" * 40, "tag": "v-test", "clean": True,
                  "version": "test", "release": "v-test"}
        machine = {"cpu": "fixture CPU", "kernel": "fixture kernel",
                   "architecture": "x86_64", "logicalCpus": 4, "hostId": "fixture-host"}
        for offset, lane in enumerate(("cold-start", "qps", "density")):
            path = root / "bench/results" / lane
            path.mkdir(parents=True)
            self.paths[lane] = path
            start = f"2026-10-20T0{offset}:00:00Z"
            m = {"schema": 1, "lane": lane, "session": "synthetic-test-session",
                 "status": "complete", "startedAt": start,
                 "completedAt": f"2026-10-20T0{offset}:10:00Z", "origin": dict(origin),
                 "machine": dict(machine), "protocol": dict(contract.PROTOCOL[lane], sutCpuSet="0-3"),
                 "posture": {"platform": "linux", "mode": "c-abi" if lane == "cold-start" else "managed",
                             "sandbox": "strict"},
                 "summary": "cells.txt", "isolation": "isolation.json", "binaries": {},
                 "command": f"fixture-only {lane}", "fixtures": {}, "records": []}
            self.manifests[lane] = m
            if lane == "density":
                m["protocol"]["caps"] = {"capsid": 2, "workerd": 2}
            asset = b"export default 'synthetic parser fixture';\n"
            self.write(path / "fixtures/app.mjs", asset)
            m["fixtures"]["shared-app"] = {"file": "fixtures/app.mjs", "sha256": digest(asset)}
            lines = []
            for index, key in enumerate(sorted(contract.expected_keys(lane))):
                stack, value = key[:2]
                record = {"stack": stack, "workload" if lane == "qps" else "size": value,
                          "binaries": sorted(contract.required_binaries(lane, stack)),
                          "fixtures": ["shared-app"], "samples": [], "correctness": []}
                for name in record["binaries"]:
                    binary = f"synthetic binary {name}".encode()
                    m["binaries"][name] = {"sha256": digest(binary), "commit": "a" * 40,
                                           "tag": "v-test", "version": "test",
                                           "file": f"binaries/{name}.bin"}
                    self.write(path / f"binaries/{name}.bin", binary)
                if lane == "qps":
                    synthetic_clients = synthetic_memory(stack, 'serving')['loadgenProcesses']
                    offset = contract.WORKLOADS.index(value) * 5
                    record['clientProcesses'] = synthetic_clients[offset:offset+5]
                    body = (b'b' * 341 + b'c' * 341 + b'd' * 342) if value == 'stream' else json.dumps(
                        {'data': 'x' * ((1024 if value == 'matrix-json-1k' else 16384) - 11)},
                        separators=(',', ':')).encode()
                    payload = {'workload': value, 'response_validation': 'exact-bytes-v2',
                               'payload_bytes': len(body), 'expected_body_sha256': digest(body)}
                    lines.append(f"{stack} {value} qps_med=100.0 p50_ms=1.0 p95_ms=2.0")
                    for round_no in range(1, 6):
                        rows = [dict(payload, **{"side": stack, "round": round_no, "phase": phase,
                                 "qps": 100.0, "p50_ms": 1.0, "p95_ms": 2.0, "p99_ms": 3.0,
                                 "duration_s": duration, "completed": duration * 100,
                                 "requests_attempted": duration * 100, "responses_checked": duration * 100,
                                 "errors": 0, "timeouts": 0, "connections": 64, "inflight": 64,
                                 "request_timeout_ms": 30000})
                                for phase, duration in (("warmup", 5), ("measured", 15))]
                        self.raw(path, record, f"raw.{index}.{round_no}.jsonl", rows)
                        self.verdict(path, record, f"correctness.{index}.{round_no}.json", 2000,
                                     dict(payload, requests_attempted=2000))
                elif lane == "cold-start":
                    head = stack.replace("capsid-", "capsid ")
                    lines.append(f"{head} {value} total_ms=100.00 ready_ms=80.00 warm_ms=1.000 pss_mb=1.0")
                    rows = [synthetic_round(stack, value, r) for r in range(1, 6)]
                    self.raw(path, record, f"raw.{index}.jsonl", rows)
                    self.verdict(path, record, f"correctness.{index}.json", 255)
                    discard = f"discarded.{index}.jsonl"
                    correct_discard = f"discarded-correctness.{index}.json"
                    record["discardedSamples"] = [discard]
                    record["discardedCorrectness"] = [correct_discard]
                    self.write(path / discard, encoded(synthetic_round(stack, value, 0)))
                    self.write(path / correct_discard, encoded({"ok": True, "errors_as_success": False,
                        "responses_checked": 51, "errors": 0, "timeouts": 0, "mismatches": 0}))
                else:
                    record["phase"] = key[2]
                    unit = "workers" if stack == "capsid" else "cells"
                    lines.append(f"{stack} {value} {unit}=2 pss_mb=1.0 phase={key[2]} note=cap-reached")
                    directory = f'density-probe-{index}'
                    row, verdict = synthetic_density(path, directory, stack, value, key[2], 2,
                                                     probe_index=index+1, write=self.write)
                    wrapper = directory + ('/runtime/apps/orders/v1/bundle.mjs' if stack=='capsid' else '/runtime/bundle.mjs')
                    fixture_id = 'density-' + directory
                    m['fixtures'][fixture_id] = {'file': wrapper, 'sha256': digest((path/wrapper).read_bytes())}
                    record['fixtures'].append(fixture_id)
                    self.raw(path, record, f"raw.{index}.jsonl", [row])
                    self.verdict(path, record, f"correctness.{index}.json", verdict['responses_checked'], verdict)
                m["records"].append(record)
            if lane == "qps":
                memory = {}
                for stack in contract.STACKS[lane]:
                    memory[stack] = {phase: synthetic_memory(stack, phase) for phase in ('lowLoad', 'serving')}
                    memory[stack]['serving']['records'] = [
                        {'workload': record['workload'], 'round': number + 1, 'samples': sample,
                         'correctness': record['correctness'][number], 'clientProcess': record['clientProcesses'][number],
                         'measurement': json.loads((path/sample).read_text().splitlines()[1])}
                        for record in m['records'] if record['stack'] == stack
                        for number,sample in enumerate(record['samples'])]
                    for phase in ('lowLoad', 'serving'):
                        proof = f'memory-proofs/{stack}-{phase}.json'
                        self.write(path/proof, encoded(memory[stack][phase]))
                        memory[stack][phase]['resultFile'] = proof
                    for kind, phase in (("serving", "serving"), ("lowqps", "lowLoad")):
                        roles = ';'.join(f'{role}={kb/1024:.1f}' for role,kb in memory[stack][phase]['roles_kb'].items())
                        lines.extend((f"{stack} {kind}_pss_mb=1.0", f"{stack} {kind}_pss_detail={roles}"))
                m["memory"] = "memory.json"
                self.write(path / "memory.json", encoded(memory))
                m['runtimeDependencies'] = 'runtime-dependencies.json'
                package_code=b'synthetic installed framework code; independent of shared app fixture'
                self.write(path/'dependencies/python/synthetic-module',package_code)
                self.write(path/'runtime-dependencies.json', encoded({'python': {
                    'version': 'synthetic Python fixture', 'executableSHA256': m['binaries']['python']['sha256'],
                    'packages': [{'name': name, 'version': 'synthetic'} for name in ('fastapi','uvicorn','starlette','flask','gunicorn')],
                    'files': [{'package': name, 'logicalPath': 'synthetic-module', 'file': 'dependencies/python/synthetic-module', 'sha256': digest(package_code)}
                              for name in ('fastapi','uvicorn','starlette','flask','gunicorn')]}}))
            self.write(path / "cells.txt", ("\n".join(lines) + "\n").encode())
            self.write(path / "isolation.json", encoded({"session": m["session"], "origin": origin,
                       "machine": machine, "posture": m["posture"], "quietSession": True,
                       "ownedPidsRemaining": 0}))
            self.manifest(lane)

    def write(self, path, data):
        if self.saved is not None and path not in self.saved:
            self.saved[path] = path.read_bytes() if path.exists() else None
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def raw(self, path, record, name, rows):
        record["samples"].append(name)
        self.write(path / name, b"".join(encoded(row) for row in rows))

    def verdict(self, path, record, name, count, extra=None):
        record["correctness"].append(name)
        self.write(path / name, encoded(dict(extra or {}, **{"ok": True, "errors_as_success": False,
                   "responses_checked": count, "mismatches": 0, "errors": 0, "timeouts": 0})))

    def seal(self, lane):
        path = self.paths[lane]
        entries = [(file.relative_to(path).as_posix(), digest(file.read_bytes()))
                   for file in path.rglob("*") if file.is_file() and file.name != "sha256sums.txt"]
        self.write(path / "sha256sums.txt", "".join(f"{sha}  {name}\n" for name, sha in sorted(entries)).encode())

    def manifest(self, lane):
        m = self.manifests[lane]
        self.write(self.paths[lane] / "lane.json", encoded(m))
        pairs = {"commit": m["origin"]["commit"], "tag": m["origin"]["tag"],
                 "cpu": m["machine"]["cpu"], "uname": m["machine"]["kernel"],
                 "nproc": m["machine"]["logicalCpus"], "generated_at": m["startedAt"], "command": m["command"]}
        pairs.update({f"binary.{k}.sha256": v["sha256"] for k, v in m["binaries"].items()})
        pairs.update({f"fixture.{k}.sha256": v["sha256"] for k, v in m["fixtures"].items()})
        self.write(self.paths[lane] / "manifest.txt", "".join(f"{k}: {v}\n" for k, v in pairs.items()).encode())
        self.seal(lane)

    def mutate_json(self, lane, name, change):
        path = self.paths[lane] / name
        data = json.loads(path.read_text())
        change(data)
        self.write(path, encoded(data))
        self.seal(lane)

    def mutate_raw(self, lane, record, change):
        path = self.paths[lane] / self.manifests[lane]["records"][record]["samples"][0]
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        change(rows)
        self.write(path, b"".join(encoded(row) for row in rows))
        self.seal(lane)

    def proof(self, lane):
        m = self.manifests[lane]
        self.write(self.paths[lane] / "isolation.json", encoded({
            "session": m["session"], "origin": m["origin"], "machine": m["machine"],
            "posture": m["posture"], "quietSession": True, "ownedPidsRemaining": 0}))
        self.manifest(lane)

    @contextmanager
    def transaction(self):
        previous = json.loads(json.dumps(self.manifests))
        self.saved = {}
        try:
            yield
        finally:
            for path, data in self.saved.items():
                if data is None:
                    path.unlink(missing_ok=True)
                else:
                    path.write_bytes(data)
            self.saved = None
            self.manifests = previous

    def argv(self, density=True):
        values = ["--repo", str(self.root), "--cold-start", str(self.paths["cold-start"]),
                  "--qps", str(self.paths["qps"]), "--version", "test", "--release", "v-test",
                  "--generated-at", "2026-10-20T04:00:00Z", "--data", str(self.root / "data.js")]
        return values + (["--density", str(self.paths["density"])] if density else [])


class PerformanceEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="capsid-perf-contract-")
        # macOS exposes its temporary directory through /var -> /private/var.
        # Keep the owned fixture under its canonical root; archive entries
        # still undergo the collector's complete link/junction validation.
        cls.fixture = Fixture(Path(cls.temp.name).resolve(strict=True))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        (self.fixture.root / "data.js").unlink(missing_ok=True)

    def collect(self, density=True):
        with redirect_stdout(io.StringIO()):
            collector.main(self.fixture.argv(density))
        return collector.load_existing(self.fixture.root / "data.js")

    def test_complete_fixture_and_determinism(self):
        f = self.fixture
        first = self.collect()
        bytes_before = (f.root / "data.js").read_bytes()
        self.assertEqual(first, self.collect())
        self.assertEqual(bytes_before, (f.root / "data.js").read_bytes())
        point = first["points"][0]
        self.assertEqual(point["git"]["commit"], "a" * 40)
        self.assertEqual(point["generation"]["collector"]["commit"], "")
        self.assertEqual(point["verification"]["status"], "verified")
        self.assertIn("density", point["origins"])
        for name, expected in point["evidence"].items():
            self.assertEqual(digest((f.root / name).read_bytes()), expected)
        self.assertEqual(len(point["coldStart"]["cells"]), 15)
        self.assertEqual(len(point["qps"]["cells"]), 15)
        self.assertTrue(any("/density/" in name for name in point["evidence"]))
        self.assertEqual(point["density"]["capsid"]["cold@10k"]["limit"], "cap")

    def test_density_is_optional_but_never_silently_ignored(self):
        point = self.collect(density=False)["points"][0]
        self.assertEqual(point["density"], {})
        self.assertNotIn("density", point["origins"])
        with self.fixture.transaction():
            self.fixture.write(self.fixture.paths["density"] / "cells.txt", b"capsid 10k workers=2 pss_mb=fail\n")
            with self.assertRaisesRegex(contract.EvidenceError, "invalid number"):
                self.collect()

    def test_cli_rejects_and_preserves_history(self):
        f = self.fixture
        old = b"not a valid history; must survive archive rejection\n"
        (f.root / "data.js").write_bytes(old)
        with f.transaction():
            file = f.paths["qps"] / "lane.json"
            f.write(file, file.read_bytes() + b" ")
            result = subprocess.run([sys.executable, "-B", str(args.collector.resolve()), *f.argv()],
                                    capture_output=True, text=True, timeout=15,
                                    env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("SHA-256 mismatch", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertEqual((f.root / "data.js").read_bytes(), old)

    def test_parser_boundaries_reject_duplicates_bad_numbers_and_rows(self):
        f = self.fixture
        for lane, parse in (("cold-start", collector.parse_cold_start),
                            ("qps", collector.parse_qps), ("density", collector.parse_density)):
            original = (f.paths[lane] / "cells.txt").read_bytes()
            for bad in (b"not a row\n", original.splitlines()[0] + b"\n" + original,
                        original.replace(b"=1.0", b"=NaN", 1),
                        original.replace(b"=1.0", b"=inf", 1),
                        original.replace(b"=1.0", b"=-1", 1)):
                with self.subTest(lane=lane, bad=bad[:65]), f.transaction():
                    f.write(f.paths[lane] / "cells.txt", bad)
                    with self.assertRaises(contract.EvidenceError):
                        parse(f.paths[lane])

    def test_manifest_and_provenance_rejections(self):
        f = self.fixture
        cases = [
            ("status", "running", "complete"), ("schema", 99, "manifest"),
            ("lane", "cold-start", "manifest"), ("session", "", "session"),
            ("posture", {}, "posture"),
            ("posture", {"platform": "linux", "mode": "static-pool", "sandbox": "strict"}, "posture"),
            ("startedAt", "2026-10-20T01:00:00", "timezone"),
            ("completedAt", "2026-10-19T01:00:00Z", "interval"),
            ("records", [], "missing raw records"), ("binaries", {}, "binary identities"),
            ("fixtures", {}, "fixture identities"), ("memory", "missing.json", "SHA-256 entry"),
        ]
        for key, value, error in cases:
            with self.subTest(key=key, value=value), f.transaction():
                f.manifests["qps"][key] = value
                f.manifest("qps")
                with self.assertRaisesRegex(contract.EvidenceError, error):
                    self.collect()
        for section, key, value, error in [
                ("origin", "commit", "bad", "source identity"),
                ("origin", "clean", False, "source identity"),
                ("origin", "version", "other", "source identity"),
                ("origin", "release", "other", "source identity"),
                ("machine", "cpu", "unknown", "identity"),
                ("machine", "logicalCpus", True, "integer"),
                ("protocol", "rounds", 3, "protocol"),
                ("protocol", "connections", 63, "protocol")]:
            with self.subTest(section=section, key=key), f.transaction():
                f.manifests["qps"][section][key] = value
                f.manifest("qps")
                with self.assertRaisesRegex(contract.EvidenceError, error):
                    self.collect()

    def test_raw_correctness_and_final_verification_rejections(self):
        f = self.fixture
        cases = [
            ("qps", lambda rows: rows.pop(), "warmup and measured"),
            ("qps", lambda rows: rows[1].update(round=2), "wrong stack or round"),
            ("qps", lambda rows: rows[1].update(qps=101), "qps"),
            ("qps", lambda rows: rows[1].update(errors=1), "errors"),
            ("qps", lambda rows: rows[1].update(connections=1), "connection"),
            ("qps", lambda rows: rows[1].update(duration_s=1), "duration"),
            ("qps", lambda rows: rows[1].update(p50_ms=9), "percentile"),
            ("cold-start", lambda rows: rows.pop(), "five distinct"),
            ("cold-start", lambda rows: rows[0].update(ok=False), "failed response"),
            ("cold-start", lambda rows: rows[0].update(pss_readings_kb=[1024, 1025]), "stable positive"),
            ("cold-start", lambda rows: rows[0].update(total_ms=10), "cold timing"),
            ("density", lambda rows: rows[0].update(status="error"), "final verification"),
            ("density", lambda rows: rows[0].update(finalVerification=False), "final verification"),
            ("density", lambda rows: rows[0].update(pss_readings_kb=[0, 0]), "PSS"),
            ("density", lambda rows: rows[0].update(pss_readings_kb=[3 * 1024 * 1024] * 2), "exceeded budget"),
            ("density", lambda rows: rows[0].update(requestsPerWorker={"only-one": 100}), "per-worker"),
        ]
        for lane, mutate, error in cases:
            with self.subTest(lane=lane, error=error), f.transaction():
                f.mutate_raw(lane, 0, mutate)
                with self.assertRaisesRegex(contract.EvidenceError, error):
                    self.collect()
        for field, value, error in (("ok", False, "verdict"), ("mismatches", 1, "nonzero"),
                                    ("errors_as_success", True, "verdict"),
                                    ("responses_checked", 1, "correctness count")):
            with self.subTest(field=field), f.transaction():
                name = f.manifests["qps"]["records"][0]["correctness"][0]
                f.mutate_json("qps", name, lambda d: d.update({field: value}))
                with self.assertRaisesRegex(contract.EvidenceError, error):
                    self.collect()

    def test_density_warmup_policy_and_actual_wave_must_agree(self):
        f = self.fixture
        for field, value in (("warmupConcurrencyLimit", 63), ("warmupConcurrencyLimit", True),
                             ("warmupConcurrencyLimit", 128),
                             ("maximumWaveRequests", 257), ("maximumWaveRequests", 0),
                             ("maximumRequestsAllowed", 999999), ("coverageTimeoutSeconds", 301)):
            with self.subTest(field=field, value=value), f.transaction():
                name = f.manifests["density"]["records"][0]["correctness"][0]
                f.mutate_json("density", name, lambda row: row.update({field: value}))
                with self.assertRaisesRegex(contract.EvidenceError, "warmup"):
                    self.collect()
        for field in ("warmupConcurrencyPolicy", "warmupConcurrencyFactor", "requestsPerInstancePerWave",
                      "capsidConditioning", "workerdConditioning"):
            with self.subTest(field=field), f.transaction():
                del f.manifests["density"]["protocol"][field]
                f.manifest("density")
                with self.assertRaisesRegex(contract.EvidenceError, "protocol"):
                    self.collect()
        with f.transaction():
            f.manifests["density"]["protocol"]["warmupConcurrencyFactor"] = 64
            f.manifest("density")
            with self.assertRaisesRegex(contract.EvidenceError, "protocol"):
                self.collect()

    def test_density_producer_and_publication_conditioning_limits_agree(self):
        from density_probe import DensityProbe
        protocol = contract.PROTOCOL["density"]
        self.assertEqual(128, protocol["warmupConcurrencyFactor"])
        probe = DensityProbe(host="unused", worker="unused", workerd="unused",
                             apps=self.fixture.root, out=self.fixture.root, cpuset="0-3")
        self.assertEqual(64, protocol["parallelRequests"])
        self.assertEqual(64, probe.parallel)
        for count, expected in ((1, 128), (2, 256), (65, 8320)):
            with self.subTest(count=count):
                self.assertEqual(expected, probe.warmup_concurrency(count))
                self.assertEqual(expected, max(protocol["parallelRequests"],
                                              count * protocol["warmupConcurrencyFactor"]))

    def density_probe_record(self, stack='capsid', phase='cold'):
        f = self.fixture
        index, record = next((i,r) for i,r in enumerate(f.manifests['density']['records'])
                             if r['stack']==stack and r['phase']==phase)
        rows = [json.loads(line) for line in (f.paths['density']/record['samples'][0]).read_text().splitlines()]
        return index, record, rows[0]

    def mutate_density_jsonl(self, name, change):
        f = self.fixture
        path = f.paths['density']/name
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        change(rows)
        f.write(path,b''.join(encoded(row) for row in rows))
        f.seal('density')

    def test_density_conditioning_summaries_cannot_replace_the_protocol(self):
        f = self.fixture
        _, record, _ = self.density_probe_record()
        for field, value in (('conditioningMethod','old-burst'), ('conditioningWaves',True),
                             ('conditioningWaves',2), ('conditioningSeconds',301),
                             ('conditioningSeconds',0), ('externalWorkerExecutionBarrier',False),
                             ('conditioningEvidence','../requests.jsonl'), ('barrierWaves',0),
                             ('maximumWaveRequests',1)):
            with self.subTest(field=field,value=value), f.transaction():
                f.mutate_json('density',record['correctness'][0],lambda item:item.update({field:value}))
                with self.assertRaisesRegex(contract.EvidenceError,'conditioning'):
                    self.collect()

    def test_density_resealed_response_rows_are_independently_checked(self):
        f = self.fixture
        _, _, row = self.density_probe_record()
        name = row['probeDirectory']+'/requests.jsonl'
        for field, value in (('status',503),('ok',False),('bodySHA256','0'*64),
                             ('sequence',True),('waveRequests',1),('port',1),
                             ('warmupConcurrencyLimit',True),('externalWorkerExecutionBarrier',False)):
            with self.subTest(field=field), f.transaction():
                self.mutate_density_jsonl(name,lambda rows:rows[0].update({field:value}))
                with self.assertRaisesRegex(contract.EvidenceError,'conditioning'):
                    self.collect()
        with f.transaction():
            self.mutate_density_jsonl(name,lambda rows:rows.pop())
            with self.assertRaisesRegex(contract.EvidenceError,'raw response count'):
                self.collect()

    def test_density_resealed_barriers_require_dispatch_and_every_resume(self):
        f = self.fixture
        _, _, row = self.density_probe_record(phase='warm')
        name = row['probeDirectory']+'/runtime/prewarm-barriers.jsonl'
        changes = [
            lambda rows: rows[0].update(rootPaused=True),
            lambda rows: rows[0].update(observedHttpInflightBeforeResume=1),
            lambda rows: rows[0].update(resumeSignalsIssued=1),
            lambda rows: rows[0].update(resumeSignalsAttempted=True),
            lambda rows: rows[0].update(resumeFailures=[{'message':'failed'}]),
            lambda rows: rows[0].update(pauseSeconds=5),
            lambda rows: rows[0].update(wave=2),
            lambda rows: rows[0]['workers'][0].update(start=999999),
            lambda rows: rows.pop(),
        ]
        for position, change in enumerate(changes):
            with self.subTest(change=position), f.transaction():
                self.mutate_density_jsonl(name,change)
                with self.assertRaisesRegex(contract.EvidenceError,'conditioning'):
                    self.collect()

    def test_density_resealed_pss_cannot_omit_processes_or_replace_the_samples(self):
        f = self.fixture
        index, _, _ = self.density_probe_record()
        changes = [
            lambda row: row['pssSamples'][0]['processes'].pop(),
            lambda row: row['pssSamples'][0]['processes'][0].update(start=999999),
            lambda row: row['pssSamples'][0]['processes'][0].update(pss_kb=True),
            lambda row: row['pssSamples'][0]['processes'][0].update(pss_kb=999999),
            lambda row: row.update(pssSamples=row['pssSamples'][:1]),
            lambda row: row['readyProcesses'].pop(),
            lambda row: row['readyProcesses'][0].update(state=None),
        ]
        for position, change in enumerate(changes):
            with self.subTest(change=position), f.transaction():
                f.mutate_raw('density',index,lambda rows:change(rows[0]))
                with self.assertRaisesRegex(contract.EvidenceError,'conditioning'):
                    self.collect()

    def test_density_business_logs_and_readiness_are_reparsed_after_resealing(self):
        f = self.fixture
        _, _, row = self.density_probe_record()
        path = f.paths['density']/row['probeDirectory']/'stderr.log'
        original = path.read_text().splitlines()
        def wrong_nonce(lines):
            value=json.loads(lines[-1].split('CAPSID_BENCH_DENSITY_V1 ',1)[1])
            value['nonce']='f'*32
            lines[-1]='CAPSID_BENCH_DENSITY_V1 '+json.dumps(value)
        def repeated_counter(lines):
            lines[-1]=lines[-2]
        def early_request(lines):
            lines.insert(1,lines.pop(2))
        for name, change in [('wrong-nonce',wrong_nonce),('repeated-counter',repeated_counter),
                             ('early-request',early_request),('missing-counter',lambda lines:lines.pop())]:
            with self.subTest(change=name), f.transaction():
                lines=list(original);change(lines)
                f.write(path,('\n'.join(lines)+'\n').encode());f.seal('density')
                with self.assertRaisesRegex(contract.EvidenceError,'conditioning'):
                    self.collect()

    def test_density_coupled_cleanup_forgery_still_needs_the_actual_resume_log(self):
        f = self.fixture
        index, _, row = self.density_probe_record()
        name = row['probeDirectory']+'/cleanup.json'
        def remove_resumes(cleanup):
            cleanup['signals']=[s for s in cleanup['signals'] if s['signal']!='SIGCONT']
        with f.transaction():
            f.mutate_json('density',name,remove_resumes)
            changed=json.loads((f.paths['density']/name).read_text())
            f.mutate_raw('density',index,lambda rows:rows[0].update(cleanup=changed))
            with self.assertRaisesRegex(contract.EvidenceError,'actual signal log'):
                self.collect()

    def test_qps_payload_claims_cannot_be_omitted_or_swapped(self):
        f = self.fixture
        claims = {'workload': 'stream', 'response_validation': 'three-position-check',
                  'payload_bytes': 1, 'expected_body_sha256': '0' * 64}
        for field, value in {**claims, 'request_timeout_ms': 1}.items():
            for missing in (False, True):
                with self.subTest(field=field, missing=missing, location='raw'), f.transaction():
                    def change(rows):
                        if missing: rows[1].pop(field)
                        else: rows[1][field] = value
                    f.mutate_raw('qps', 0, change)
                    with self.assertRaises(contract.EvidenceError): self.collect()
        for field, value in claims.items():
            for missing in (False, True):
                with self.subTest(field=field, missing=missing, location='verdict'), f.transaction():
                    name = f.manifests['qps']['records'][0]['correctness'][0]
                    def change(value_object):
                        if missing: value_object.pop(field)
                        else: value_object[field] = value
                    f.mutate_json('qps', name, change)
                    with self.assertRaises(contract.EvidenceError): self.collect()

    def test_qps_dispatched_and_checked_counts_must_equal_completed(self):
        f = self.fixture
        for index in (0, 1):
            for field in ('requests_attempted', 'responses_checked'):
                for missing in (False, True):
                    with self.subTest(index=index, field=field, missing=missing), f.transaction():
                        def change(rows):
                            if missing: rows[index].pop(field)
                            else: rows[index][field] = 0
                        f.mutate_raw('qps', 0, change)
                        with self.assertRaises(contract.EvidenceError): self.collect()
        for missing in (False, True):
            with self.subTest(missing=missing, location='verdict'), f.transaction():
                name = f.manifests['qps']['records'][0]['correctness'][0]
                def change(value):
                    if missing: value.pop('requests_attempted')
                    else: value['requests_attempted'] = 0
                f.mutate_json('qps', name, change)
                with self.assertRaises(contract.EvidenceError): self.collect()

    def test_qps_memory_needs_complete_process_requests_cleanup_and_phase_evidence(self):
        f = self.fixture
        mutations = [lambda d:d['capsid']['serving'].pop('measurementRoots'),
            lambda d:d['capsid']['serving'].update(measurementProcesses=[]),
            lambda d:d['capsid']['serving'].pop('pssSamples'),
            lambda d:d['capsid']['serving'].update(pss_readings_kb=[1024,2048]),
            lambda d:d['capsid']['serving']['cleanup'].update(ownedPidsRemaining=1),
            lambda d:d['capsid']['serving']['cleanup']['trees'].pop(),
            lambda d:d['capsid']['serving'].update(loadgenProcesses=[]),
            lambda d:d['capsid']['lowLoad']['requests'][4].update(bodySHA256='0'*64),
            lambda d:d['capsid']['lowLoad']['requests'].pop(),
            lambda d:d['capsid']['lowLoad'].update(cleanupCompletedMonotonic=100)]
        for number, mutate in enumerate(mutations):
            with self.subTest(number=number), f.transaction():
                memory=json.loads((f.paths['qps']/'memory.json').read_text());mutate(memory)
                # Keep the sealed probe copy consistent too; rejection must
                # check the semantics rather than just cross-file equality.
                for phase in ('lowLoad','serving'):
                    value=dict(memory['capsid'][phase]);proof=value.pop('resultFile')
                    f.write(f.paths['qps']/proof,encoded(value))
                f.write(f.paths['qps']/'memory.json',encoded(memory));f.seal('qps')
                with self.assertRaises(contract.EvidenceError):self.collect()

    def test_cold_discarded_run_and_fresh_processes_are_required(self):
        f = self.fixture
        record = dict(f.manifests["cold-start"]["records"][0])
        for field in ("discardedSamples", "discardedCorrectness"):
            with self.subTest(field=field), f.transaction():
                f.manifests["cold-start"]["records"][0].pop(field)
                f.manifest("cold-start")
                with self.assertRaises(contract.EvidenceError): self.collect()
        for field, value in (("round", 1), ("ok", False), ("warm_n", 49)):
            with self.subTest(field=field), f.transaction():
                f.mutate_json("cold-start", record["discardedSamples"][0], lambda row: row.update({field: value}))
                with self.assertRaises(contract.EvidenceError): self.collect()
        with f.transaction():
            f.mutate_json("cold-start", record["discardedCorrectness"][0], lambda row: row.update(responses_checked=50))
            with self.assertRaises(contract.EvidenceError): self.collect()
        with f.transaction():
            f.write(f.paths["cold-start"] / "failed.json", encoded({"status": "error"}))
            f.seal("cold-start")
            with self.assertRaisesRegex(contract.EvidenceError, "failed run"): self.collect()
        with f.transaction():
            def reused(rows):
                previous = rows[0]
                for key in ("measurementRoot", "measurementProcesses", "pssSamples", "worker_pid"):
                    if key in previous: rows[1][key] = previous[key]
            f.mutate_raw("cold-start", 0, reused)
            with self.assertRaises(contract.EvidenceError): self.collect()

    def test_cold_process_members_and_warm_latencies_are_required(self):
        f = self.fixture
        mutations = [lambda row: row.pop("measurementProcesses"),
            lambda row: row.update(probeProcess=dict(row["measurementRoot"])),
            lambda row: row["measurementProcesses"].append(dict(row["measurementRoot"])),
            lambda row: row["pssSamples"][0]["processes"][0].update(pss_kb=1025),
            lambda row: row["pssSamples"][0]["processes"][0].update(start=99999),
            lambda row: row.update(warm_latencies_ms=[2] * 50),
            lambda row: row.update(warm_latencies_ms=[1] * 49),
            lambda row: row["cleanup"].update(ownedPidsRemaining=1),
            lambda row: row["correctness"].update(responses_checked=50)]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index), f.transaction():
                f.mutate_raw("cold-start", 0, lambda rows: mutate(rows[0]))
                with self.assertRaises(contract.EvidenceError): self.collect()

    def test_missing_rows_records_digests_and_conflicting_text(self):
        f = self.fixture
        for lane in f.paths:
            with self.subTest(lane=lane), f.transaction():
                file = f.paths[lane] / "cells.txt"
                f.write(file, b"\n".join(file.read_bytes().splitlines()[1:]) + b"\n")
                f.seal(lane)
                with self.assertRaisesRegex(contract.EvidenceError, "summary rows"):
                    self.collect()
        for name, expected in (("lane.json", "lane.json"), ("manifest.txt", "manifest.txt"),
                               ("sha256sums.txt", "sha256sums")):
            with self.subTest(name=name), f.transaction():
                file = f.paths["qps"] / name
                f.write(file, file.read_bytes())
                file.unlink()
                with self.assertRaises(contract.EvidenceError):
                    self.collect()
        with f.transaction():
            file = f.paths["qps"] / "manifest.txt"
            f.write(file, file.read_bytes().replace(b"fixture CPU", b"another CPU"))
            f.seal("qps")
            with self.assertRaisesRegex(contract.EvidenceError, "conflicting cpu"):
                self.collect()

    def test_no_evidence_reuse_and_history_corruption(self):
        f = self.fixture
        for field in ("samples", "correctness"):
            with self.subTest(field=field), f.transaction():
                records = f.manifests["qps"]["records"]
                records[1][field] = records[0][field]
                f.manifest("qps")
                with self.assertRaisesRegex(contract.EvidenceError, "reused across cells"):
                    self.collect()
        for old in (b"bad history\n", b'window.CAPSID_PERF_DATA = {"schema":1,"points":[NaN]};\n'):
            (f.root / "data.js").write_bytes(old)
            with self.assertRaises((contract.EvidenceError, ValueError)):
                self.collect()
            self.assertEqual((f.root / "data.js").read_bytes(), old)

    def test_cross_lane_conflicts_include_density_and_binary_fixture_hashes(self):
        f = self.fixture
        for lane in ("qps", "density"):
            for key, value in (("session", "other-session"), ("cpu", "other CPU"),
                               ("kernel", "other kernel"), ("architecture", "other arch")):
                with self.subTest(lane=lane, key=key), f.transaction():
                    m = f.manifests[lane]
                    if key == "session":
                        m[key] = value
                    else:
                        m["machine"][key] = value
                    f.proof(lane)
                    with self.assertRaisesRegex(contract.EvidenceError, "conflicting (session|machine)"):
                        self.collect()
            with self.subTest(lane=lane, key="binary"), f.transaction():
                m = f.manifests[lane]
                binary = m["binaries"]["workerd"]
                data = b"other synthetic workerd binary"
                binary["sha256"] = digest(data)
                f.write(f.paths[lane] / binary["file"], data)
                f.manifest(lane)
                with self.assertRaisesRegex(contract.EvidenceError, "conflicting workerd binary"):
                    self.collect()
            with self.subTest(lane=lane, key="fixture"), f.transaction():
                m = f.manifests[lane]
                data = b"other synthetic fixture"
                m["fixtures"]["shared-app"]["sha256"] = digest(data)
                f.write(f.paths[lane] / "fixtures/app.mjs", data)
                f.manifest(lane)
                with self.assertRaisesRegex(contract.EvidenceError, "conflicting shared-app fixture"):
                    self.collect()
        with f.transaction():
            m = f.manifests["qps"]
            m["origin"]["commit"] = "b" * 40
            for name in ("capsid-host", "capsid-worker", "loadgen"):
                m["binaries"][name]["commit"] = "b" * 40
            f.proof("qps")
            with self.assertRaisesRegex(contract.EvidenceError, "conflicting origin"):
                self.collect()
        with f.transaction():
            f.manifests["qps"]["startedAt"] = "2026-10-20T00:05:00Z"
            f.manifest("qps")
            with self.assertRaisesRegex(contract.EvidenceError, "lanes overlap"):
                self.collect()

    def test_digest_inventory_and_path_boundaries(self):
        f = self.fixture
        file = f.paths["qps"] / "sha256sums.txt"
        original = file.read_bytes()
        for data, error in ((original + original.splitlines()[0] + b"\n", "duplicate"),
                            (b"\n".join(original.splitlines()[1:]) + b"\n", "inventory"),
                            (b"not a checksum\n", "malformed"),
                            (b"0" * 64 + b"  sha256sums.txt\n", "self-referencing")):
            with self.subTest(error=error), f.transaction():
                f.write(file, data)
                with self.assertRaisesRegex(contract.EvidenceError, error):
                    self.collect()
        for name in ("../outside.txt", "/tmp/outside.txt", "C:/outside.txt", "a\\b", "./cells.txt", "a//b"):
            with self.subTest(name=name), f.transaction():
                f.write(file, ("0" * 64 + "  " + name + "\n").encode())
                with self.assertRaisesRegex(contract.EvidenceError, "invalid archive-relative path"):
                    self.collect()
        with f.transaction():
            f.write(f.paths["qps"] / "unsealed.log", b"a late writer is still active")
            with self.assertRaisesRegex(contract.EvidenceError, "inventory"):
                self.collect()
        with f.transaction():
            f.mutate_json("qps", "isolation.json", lambda d: d.update(ownedPidsRemaining=1))
            with self.assertRaisesRegex(contract.EvidenceError, "isolation or cleanup proof"):
                self.collect()

    def test_json_and_memory_rejections(self):
        f = self.fixture
        for content in (b'{"schema":1,"schema":1}\n', b'{"schema":NaN}\n', b'not json\n'):
            with self.subTest(content=content), f.transaction():
                f.write(f.paths["qps"] / "lane.json", content)
                f.seal("qps")
                with self.assertRaises(contract.EvidenceError):
                    self.collect()
        for change, error in ((lambda d: d.pop("capsid"), "missing memory"),
                              (lambda d: d["capsid"]["lowLoad"].update(beforeMeasuredLoad=False), "fresh low-load"),
                              (lambda d: d["capsid"]["serving"].update(pss_kb=1), "role sum")):
            with self.subTest(error=error), f.transaction():
                f.mutate_json("qps", "memory.json", change)
                with self.assertRaisesRegex(contract.EvidenceError, error):
                    self.collect()

    def test_archive_recheck_rejects_late_writers(self):
        f = self.fixture
        for name, content in (("late.log", b"writer is not finished"),
                              ("sha256sums.txt", b"changed digest inventory"),
                              ("cells.txt", b"changed summary")):
            with self.subTest(name=name), f.transaction():
                archive = contract.Archive(f.paths["qps"], f.root)
                f.write(f.paths["qps"] / name, content)
                with self.assertRaisesRegex(contract.EvidenceError, "changed during validation"):
                    archive.recheck()
        with f.transaction():
            f.manifests["density"]["protocol"]["caps"]["capsid"] = 1
            f.manifest("density")
            with self.assertRaisesRegex(contract.EvidenceError, "configured cap"):
                self.collect()
    def test_append_keeps_older_points_and_replaces_only_requested_version(self):
        f = self.fixture
        older = {"version": "old", "date": "2000-01-01", "notes": "unqualified historical observation",
                 "git": {"commit": "c" * 40}, "qps": {"cells": [{"qps": 3.25}]}}
        (f.root / "data.js").write_text("window.CAPSID_PERF_DATA = " + json.dumps(
            {"schema": 1, "points": [older]}) + ";\n", encoding="utf-8")
        first = self.collect()
        second = self.collect()
        self.assertEqual(first, second)
        self.assertEqual(len(second["points"]), 2)
        self.assertEqual(second["points"][0], older)

    def test_site_staging_rechecks_data_and_copies_every_evidence_link(self):
        f = self.fixture
        self.collect()
        source = f.root / "perf-site"
        source.mkdir(exist_ok=True)
        (source / "index.html").write_text("<!doctype html><title>synthetic site fixture</title>", encoding="utf-8")
        output = f.root / "site-complete"
        report = site.prepare_site(f.root, f.root / "data.js", output)
        self.assertEqual(report["qualifiedPoints"], 1)
        self.assertEqual(report["historicalPoints"], 0)
        point = collector.load_existing(output / "data.js")["points"][0]
        self.assertEqual(report["evidenceFiles"], len(point["evidence"]))
        for name, expected in point["evidence"].items():
            self.assertEqual(digest((output / "evidence" / name).read_bytes()), expected)
        with self.assertRaisesRegex(contract.EvidenceError, "new directory"):
            site.prepare_site(f.root, f.root / "data.js", output)
        with f.transaction():
            history = collector.load_existing(f.root / "data.js")
            history["points"][0]["qps"]["cells"][0]["qps"] = 9999999
            f.write(f.root / "data.js", b"window.CAPSID_PERF_DATA = " + encoded(history) + b";\n")
            bad_output = f.root / "site-invalid-summary"
            with self.assertRaisesRegex(contract.EvidenceError, "qps disagrees"):
                site.prepare_site(f.root, f.root / "data.js", bad_output)
            self.assertFalse(bad_output.exists())
        with f.transaction():
            asset = f.paths["density"] / "fixtures/app.mjs"
            f.write(asset, b"changed after collection")
            bad_output = f.root / "site-invalid-hash"
            with self.assertRaisesRegex(contract.EvidenceError, "SHA-256 mismatch"):
                site.prepare_site(f.root, f.root / "data.js", bad_output)
            self.assertFalse(bad_output.exists())

    def test_site_keeps_legacy_observations_without_inventing_verified_links(self):
        f = self.fixture
        source = f.root / "perf-site"
        source.mkdir(exist_ok=True)
        (source / "index.html").write_text("<!doctype html><title>synthetic site fixture</title>", encoding="utf-8")
        legacy = {"schema": 1, "points": [{"version": "old", "date": "2000-01-01",
                    "evidence": {"nonexistent-old-alias/file": "0" * 64}}]}
        (f.root / "data.js").write_bytes(b"window.CAPSID_PERF_DATA = " + encoded(legacy) + b";\n")
        output = f.root / "site-legacy"
        report = site.prepare_site(f.root, f.root / "data.js", output)
        self.assertEqual(report["qualifiedPoints"], 0)
        self.assertEqual(report["historicalPoints"], 1)
        self.assertEqual(report["evidenceFiles"], 0)
        self.assertFalse((output / "evidence").exists())


if __name__ == "__main__":
    unittest.main()
