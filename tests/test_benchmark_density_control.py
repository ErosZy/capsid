#!/usr/bin/env python3
"""Inject failure probes into the actual search/coverage controller."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import http.server
import socketserver
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ap = argparse.ArgumentParser(add_help=False)
ap.add_argument("--module", type=Path, default=Path(__file__).resolve().parents[1] / "bench/density_control.py")
ap.add_argument("--node", required=True)
args, remaining = ap.parse_known_args()
sys.argv[1:] = remaining
sys.path.insert(0, str(args.module.resolve().parent))
import density_control as control
from density_probe import EXPECTED_BODY, instrument_fixture, validated_request, DensityProbe
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


class LoopbackHTTPServer(http.server.ThreadingHTTPServer):
    def server_bind(self):
        # These peers bind an explicit numeric loopback address. Their HTTP
        # server metadata must not require system reverse DNS to become ready.
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


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
    def test_numeric_loopback_fixture_does_not_require_reverse_dns(self):
        with patch('socket.getfqdn', side_effect=AssertionError('unexpected reverse DNS')):
            with self.assertRaisesRegex(AssertionError, 'unexpected reverse DNS'):
                http.server.ThreadingHTTPServer(('127.0.0.1', 0), http.server.BaseHTTPRequestHandler)
            server = LoopbackHTTPServer(('127.0.0.1', 0), http.server.BaseHTTPRequestHandler)
            try:
                self.assertEqual('127.0.0.1', server.server_name)
                self.assertEqual(server.server_address[1], server.server_port)
                self.assertGreater(server.server_port, 0)
            finally:
                server.server_close()

    def test_clients_connect_before_the_synchronized_request_wave(self):
        received=[]
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                received.append(self.headers['x-capsid-bench-nonce'])
                self.send_response(200);self.send_header('Content-Length',str(len(EXPECTED_BODY)))
                self.end_headers();self.wfile.write(EXPECTED_BODY)
            def log_message(self,*_):pass
        server=LoopbackHTTPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=lambda:server.serve_forever(poll_interval=.01));thread.start()
        try:
            with ThreadPoolExecutor(max_workers=4) as executor:
                start=threading.Barrier(5)
                futures=[executor.submit(validated_request,server.server_port,'wave',start=start) for _ in range(4)]
                deadline=time.monotonic()+3
                while start.n_waiting<4 and time.monotonic()<deadline:time.sleep(.001)
                self.assertEqual(4,start.n_waiting,'all clients connected before release')
                self.assertEqual([],received,'no HTTP request before the connected-client barrier')
                start.wait(timeout=2)
                self.assertTrue(all(future.result(timeout=3)['ok'] for future in futures))
            self.assertEqual(['wave']*4,received)
        finally:
            server.shutdown();thread.join(timeout=2);server.server_close()
            self.assertFalse(thread.is_alive())

    def test_broken_connected_client_wave_is_a_failure(self):
        server=LoopbackHTTPServer(('127.0.0.1',0),http.server.BaseHTTPRequestHandler)
        thread=threading.Thread(target=lambda:server.serve_forever(poll_interval=.01));thread.start()
        try:
            with ThreadPoolExecutor(max_workers=1) as executor:
                start=threading.Barrier(2)
                future=executor.submit(validated_request,server.server_port,'wave',start=start)
                deadline=time.monotonic()+3
                while start.n_waiting<1 and time.monotonic()<deadline:time.sleep(.001)
                self.assertEqual(1,start.n_waiting);start.abort()
                with self.assertRaises(ProbeError) as raised:future.result(timeout=3)
                self.assertEqual('request-failed',raised.exception.code)
        finally:
            server.shutdown();thread.join(timeout=2);server.server_close()
            self.assertFalse(thread.is_alive())

    def test_starved_wave_expands_without_exceeding_configured_concurrency(self):
        coverage=control.Coverage(8,'wave');pending=[];sequence={};waves=[];lock=threading.Lock()
        def request(port,nonce,*,start):
            start.wait(timeout=2)
            with lock:
                # A synthetic fast-service scheduler only exposes three
                # identities at the small wave. The larger connected wave
                # exposes all eight; no real benchmark is claimed here.
                worker='w'+str(len(waves)%(3 if start.parties==8 else 8))
                waves.append(start.parties);sequence[worker]=sequence.get(worker,0)+1
                pending.append(dict(nonce=nonce,worker=worker,count=sequence[worker]))
            return dict(port=port,status=200,ok=True,bodySHA256='a'*64)
        def poll():
            while pending:coverage.add(pending.pop(0))
        with tempfile.TemporaryDirectory() as temporary:
            probe=DensityProbe(host=args.node,worker=args.node,workerd=args.node,apps=temporary,out=temporary,
                               cpuset='0',parallel=64,coverage_timeout=5)
            with patch('density_probe.validated_request',side_effect=request):
                result=probe.cover(SimpleNamespace(alive=lambda:True),SimpleNamespace(coverage=coverage,poll=poll),
                                   'capsid',[12345],8,1,'wave',io.StringIO())
        self.assertEqual(24,result['responses_checked']);coverage.verify(1,24)
        self.assertEqual([8,16],sorted(set(waves)));self.assertLessEqual(max(waves),64)

    def test_large_pool_conditioning_can_exceed_64_without_changing_qps(self):
        coverage=control.Coverage(65,'large');pending=[];sequence={};waves=[];lock=threading.Lock()
        def wave(ports,nonce,*,connect_parallel):
            self.assertEqual(64,connect_parallel)
            observations=[]
            for port in ports:
                identities=49 if len(ports)<130 else 65
                worker='w'+str(len(waves)%identities)
                waves.append(len(ports));sequence[worker]=sequence.get(worker,0)+1
                pending.append(dict(nonce=nonce,worker=worker,count=sequence[worker]))
                observations.append(dict(port=port,status=200,ok=True,bodySHA256='a'*64))
            return observations
        def poll():
            while pending:coverage.add(pending.pop(0))
        with tempfile.TemporaryDirectory() as temporary:
            probe=DensityProbe(host=args.node,worker=args.node,workerd=args.node,apps=temporary,out=temporary,
                               cpuset='0',parallel=64,coverage_timeout=5)
            with patch('density_probe.validated_wave',side_effect=wave):
                result=probe.cover(SimpleNamespace(alive=lambda:True),SimpleNamespace(coverage=coverage,poll=poll),
                                   'capsid',[12345],65,1,'large',io.StringIO())
        coverage.verify(1,result['responses_checked'])
        self.assertEqual(520,result['responses_checked'])
        self.assertEqual([520],sorted(set(waves)))
        self.assertEqual(4160,result['warmupConcurrencyLimit'])
        self.assertEqual(520,result['maximumWaveRequests'])
        self.assertEqual(64,probe.parallel)

    def test_large_wave_sends_all_requests_before_waiting_for_responses(self):
        from density_probe import validated_wave
        received=[];complete=threading.Event()
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                received.append(self.headers['x-capsid-bench-nonce'])
                if len(received)==3:complete.set()
                if not complete.wait(3):return
                self.send_response(200);self.send_header('Content-Length',str(len(EXPECTED_BODY)))
                self.end_headers();self.wfile.write(EXPECTED_BODY)
            def log_message(self,*_):pass
        server=LoopbackHTTPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=lambda:server.serve_forever(poll_interval=.01));thread.start()
        try:
            observations=validated_wave([server.server_port]*3,'burst',connect_parallel=2)
            self.assertEqual(['burst']*3,received)
            self.assertEqual(3,len(observations));self.assertTrue(all(row['ok'] for row in observations))
        finally:
            complete.set();server.shutdown();thread.join(timeout=2);server.server_close()
            self.assertFalse(thread.is_alive())

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
        server = LoopbackHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=.01))
        thread.start()
        try:
            self.assertTrue(validated_request(server.server_port, "test")["ok"])
            for status, body in ((200, b"wrong"), (503, EXPECTED_BODY), (200, EXPECTED_BODY + b"extra")):
                with self.subTest(status=status, body=body):
                    response[:] = status, body
                    from density_probe import validated_wave
                    for validate in (lambda: validated_request(server.server_port, "test"),
                                     lambda: validated_wave([server.server_port], "test")):
                        with self.assertRaises(ProbeError) as raised:
                            validate()
                        self.assertEqual(raised.exception.code, "correctness")
        finally:
            server.shutdown()
            thread.join(timeout=2)
            server.server_close()
            self.assertFalse(thread.is_alive())

    def test_failed_wave_connection_closes_the_already_connected_peer(self):
        import socket
        import socketserver
        from density_probe import validated_wave
        closed=threading.Event();received=[]
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                received.append(self.rfile.readline())
                closed.set()
        server=socketserver.ThreadingTCPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=lambda:server.serve_forever(poll_interval=.01));thread.start()
        try:
            with socket.socket() as refused:
                refused.bind(('127.0.0.1',0))
                with self.assertRaises(ProbeError) as raised:
                    validated_wave([server.server_address[1],refused.getsockname()[1]],'failed',connect_parallel=1)
                self.assertEqual('request-failed',raised.exception.code)
                self.assertTrue(closed.wait(3),'connected peer observes close after another connection fails')
                self.assertEqual([b''],received,'a failed connection wave sends no HTTP request')
        finally:
            server.shutdown();thread.join(timeout=2);server.server_close()
            self.assertFalse(thread.is_alive())

    def test_completed_last_wave_cannot_hide_an_expired_coverage_deadline(self):
        now=[0];coverage=control.Coverage(1,'late');pending=[]
        def request(port,nonce,*,start):
            now[0]=6;pending.append(dict(nonce=nonce,worker='one',count=1))
            return dict(port=port,status=200,ok=True,bodySHA256='a'*64)
        def poll():
            while pending:coverage.add(pending.pop(0))
        with tempfile.TemporaryDirectory() as temporary:
            probe=DensityProbe(host=args.node,worker=args.node,workerd=args.node,apps=temporary,out=temporary,
                               cpuset='0',parallel=64,coverage_timeout=5)
            with patch('density_probe.time.monotonic',side_effect=lambda:now[0]), \
                 patch('density_probe.validated_request',side_effect=request):
                with self.assertRaises(ProbeError) as raised:
                    probe.cover(SimpleNamespace(alive=lambda:True),SimpleNamespace(coverage=coverage,poll=poll),
                                'capsid',[12345],1,1,'late',io.StringIO())
                self.assertEqual('coverage-deadline',raised.exception.code)
        self.assertTrue(coverage.complete(1),'coverage alone cannot replace the elapsed-time bound')

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
