#!/usr/bin/env python3
"""Inject failure probes into the actual search/coverage controller."""
import argparse
import importlib.util
import http.server
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

ap = argparse.ArgumentParser(add_help=False)
ap.add_argument("--module", type=Path, default=Path(__file__).resolve().parents[1] / "bench/density_control.py")
ap.add_argument("--node", required=True)
args, remaining = ap.parse_known_args()
sys.argv[1:] = remaining
sys.path.insert(0, str(args.module.resolve().parent))
import density_control as control
from density_probe import EXPECTED_BODY, instrument_fixture, validated_request
from measurement_runtime import ProbeError


def result_for(call, pss=None):
    count, phase = call["count"], call["phase"]
    target = 1 if phase == "cold" else 100
    value = count * 1024 if pss is None else pss
    return {"stack": call["stack"], "size": call["size"], "phase": phase, "count": count,
            "unit": "workers" if call["stack"] == "capsid" else "cells", "status": "ok",
            "pss_readings_kb": [value, value], "requestsPerWorker": {f"w{i}": target for i in range(count)},
            "correctness": {"ok": True, "errors_as_success": False, "responses_checked": count * target,
                            "errors": 0, "mismatches": 0, "timeouts": 0},
            "cleanup": {"ownedPidsRemaining": 0}}


class SearchTests(unittest.TestCase):
    def search(self, change=None, **options):
        self.calls, self.rows = [], []
        def probe(**call):
            self.calls.append(call)
            call["release_ports"]()
            result = result_for(call)
            if change is not None:
                updated = change(call, result)
                if updated is not None:
                    result = updated
            return result
        return control.DensitySearch(probe, self.rows.append, **options)

    def test_search_replays_budget_boundary_and_a_new_final_probe(self):
        search = self.search(budget_mb=3)
        result = search.fill("capsid", "10k", "warm", 8)
        self.assertEqual(result["count"], 3)
        self.assertEqual(result["limit"], "budget")
        self.assertEqual([c["count"] for c in self.calls], [1, 2, 4, 3, 3])
        self.assertTrue(self.calls[-1]["final"])
        self.assertTrue(result["finalVerification"])
        ports = [set(row["ports"]) for row in self.rows]
        self.assertTrue(all(not a & b for a, b in zip(ports, ports[1:])))

    def test_non_power_of_two_cap_is_measured_and_distinguished(self):
        search = self.search(budget_mb=10)
        result = search.fill("workerd", "10k", "cold", 5)
        self.assertEqual([c["count"] for c in self.calls], [1, 2, 4, 5, 5])
        self.assertEqual(result["count"], 5)
        self.assertEqual(result["limit"], "cap")
        self.assertEqual(len(self.rows[-1]["ports"]), 5)

    def test_failed_touch_or_read_is_not_an_over_budget_boundary(self):
        for code in ("correctness", "process-exited", "pss-unreadable", "pss-nonpositive", "coverage-deadline"):
            with self.subTest(code=code):
                def fail(call, result):
                    if call["count"] == 2:
                        raise ProbeError(code, "injected failure")
                search = self.search(fail)
                with self.assertRaises(ProbeError) as raised:
                    search.fill("capsid", "10k", "warm", 8)
                self.assertEqual(raised.exception.code, code)
                self.assertEqual(self.rows[-1]["status"], "error")
                self.assertFalse(any(c["final"] for c in self.calls))
                json.dumps(self.rows, allow_nan=False)

    def test_no_positive_pool_fitting_budget_cannot_generate_a_zero_cell(self):
        search = self.search(budget_mb=.5)
        with self.assertRaises(ProbeError) as raised:
            search.fill("workerd", "10k", "cold", 8)
        self.assertEqual(raised.exception.code, "no-pool-fit")
        self.assertEqual(len(self.calls), 1)

    def test_final_one_worker_failure_and_bounded_retry(self):
        def unstable(call, result):
            if call["final"]:
                result["pss_readings_kb"] = [1024, 1025]
        search = self.search(unstable, final_retries=2)
        with self.assertRaises(ProbeError) as raised:
            search.fill("capsid", "10k", "warm", 1)
        self.assertEqual(raised.exception.code, "final-verification")
        self.assertEqual(sum(c["final"] for c in self.calls), 3)
        self.assertTrue(all(row["status"] == "error" for row in self.rows[-3:]))

    def test_final_over_budget_steps_down_and_reprobes_that_actual_count(self):
        def grows(call, result):
            if call["final"] and call["count"] == 3:
                result["pss_readings_kb"] = [4096, 4096]
        result = self.search(grows, budget_mb=3).fill("capsid", "10k", "warm", 8)
        self.assertEqual(result["count"], 2)
        self.assertEqual(result["pss_readings_kb"], [2048, 2048])
        self.assertTrue(self.calls[-1]["final"])
        self.assertEqual(self.calls[-1]["count"], 2)

    def test_step_down_deadline_cannot_emit_an_unverified_last_reading(self):
        def over(call, result):
            if call["final"]:
                result["pss_readings_kb"] = [21 * 1024, 21 * 1024]
        search = self.search(over, budget_mb=20, final_steps=2)
        with self.assertRaises(ProbeError) as raised:
            search.fill("capsid", "10k", "cold", 16)
        self.assertEqual(raised.exception.code, "final-verification")
        self.assertEqual([c["count"] for c in self.calls if c["final"]], [16, 15, 14])

    def test_search_deadline_stops_before_launching_the_next_probe(self):
        now = [0]
        def advance(call, result):
            now[0] = 2
        search = self.search(advance, clock=lambda: now[0], deadline_seconds=1)
        with self.assertRaises(ProbeError) as raised:
            search.fill("capsid", "10k", "cold", 8)
        self.assertEqual(raised.exception.code, "search-timeout")
        self.assertEqual(len(self.calls), 1)


class CoverageTests(unittest.TestCase):
    def test_real_http_wrong_body_and_status_cannot_pass_correctness(self):
        response = [200, EXPECTED_BODY]
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(response[0])
                self.send_header("Content-Length", str(len(response[1])))
                self.end_headers()
                self.wfile.write(response[1])
            def log_message(self, *_):
                pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=.01))
        thread.start()
        try:
            self.assertTrue(validated_request(server.server_port, "test")["ok"])
            for status, body in ((200, b"wrong"), (503, EXPECTED_BODY), (200, EXPECTED_BODY + b"extra")):
                with self.subTest(status=status, body=body):
                    response[:] = status, body
                    with self.assertRaises(ProbeError) as raised:
                        validated_request(server.server_port, "test")
                    self.assertEqual(raised.exception.code, "correctness")
        finally:
            server.shutdown()
            thread.join(timeout=2)
            server.server_close()
            self.assertFalse(thread.is_alive())

    def test_large_total_does_not_prove_each_worker(self):
        coverage = control.Coverage(2, "run")
        for count in range(1, 1001):
            coverage.add({"nonce": "run", "worker": "only-one", "count": count})
        self.assertFalse(coverage.complete(100))
        with self.assertRaises(ProbeError) as raised:
            coverage.verify(100, 1000)
        self.assertEqual(raised.exception.code, "coverage-incomplete")

    def test_missing_duplicate_replacement_and_wrong_probe_are_rejected(self):
        for record in ({"nonce": "other", "worker": "w", "count": 1},
                       {"nonce": "run", "worker": "w", "count": 2},
                       {"nonce": "run", "worker": "w", "count": True}):
            with self.subTest(record=record), self.assertRaises(ProbeError):
                control.Coverage(1, "run").add(record)
        coverage = control.Coverage(1, "run")
        coverage.add({"nonce": "run", "worker": "w", "count": 1})
        with self.assertRaises(ProbeError):
            coverage.add({"nonce": "run", "worker": "w", "count": 1})
        with self.assertRaises(ProbeError) as raised:
            coverage.add({"nonce": "run", "worker": "replacement", "count": 1})
        self.assertEqual(raised.exception.code, "coverage-replacement")

    def test_partial_structured_and_plain_counter_records(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "stderr.log"
            coverage = control.Coverage(1, "run")
            reader = control.CounterLog(path, coverage)
            first = json.dumps({"message": control.MARKER + '{"nonce":"run","worker":"w","count":1}'}) + "\n"
            path.write_text(first[:20])
            reader.poll()
            self.assertEqual(coverage.counts, {})
            with path.open("a") as stream:
                stream.write(first[20:] + control.MARKER + '{"nonce":"run","worker":"w","count":2}\n')
            reader.poll()
            coverage.verify(2, 2)
            self.assertEqual(coverage.counts, {"w": 2})

    def test_instrumented_generated_fixture_keeps_exact_public_response(self):
        generator_path = args.module.with_name("gen-cold-start-apps.py")
        spec = importlib.util.spec_from_file_location("cold_fixture", generator_path)
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            fixture_paths = []
            for entry in (generator.capsid_entry(), generator.workerd_entry()):
                original = generator.body_to(10000)[0] + entry
                file = folder / f"app-{len(fixture_paths)}.mjs"
                file.write_text(instrument_fixture(original, "fixture-run"), encoding="utf-8")
                fixture_paths.append(str(file))
            runner = folder / "check.mjs"
            runner.write_text('''import {pathToFileURL} from 'node:url';
for (const file of process.argv.slice(2)) {
  const app=(await import(pathToFileURL(file).href)).default;
  for (const headers of [{}, {'x-capsid-bench-nonce':'fixture-run'}]) {
    const response=await app.fetch(new Request('http://fixture.invalid/', {headers}));
    console.log(JSON.stringify({status:response.status,headers:[...response.headers],body:await response.text()}));
  }
}
''', encoding="utf-8")
            run = subprocess.run([args.node, str(runner), *fixture_paths], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)
            responses = [json.loads(line) for line in run.stdout.splitlines()]
            self.assertEqual(len(responses), 4)
            for response in responses:
                self.assertEqual(response["status"], 200)
                self.assertEqual(response["body"].encode(), EXPECTED_BODY)
                self.assertEqual(response["headers"], [["content-type", "application/json"]])
            counter_lines = [line for line in run.stderr.splitlines() if control.MARKER in line]
            self.assertEqual(len(counter_lines), 2, "ordinary health requests must not alter benchmark counters")


if __name__ == "__main__":
    unittest.main()
