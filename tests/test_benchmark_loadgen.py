"""Real repository load generator against this test's loopback HTTP fixture.

Short runs and two connections test correctness only. They never qualify QPS.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--loadgen', type=Path, required=True)
parser.add_argument('--artifacts', type=Path)
args, remaining = parser.parse_known_args()
sys.argv[1:] = remaining
sequence = 0
if args.artifacts:
    args.artifacts.mkdir(parents=True, exist_ok=False)


def payload(workload):
    if workload == 'stream': return b'b' * 341 + b'c' * 341 + b'd' * 342
    size = {'matrix-json-1k': 1024, 'matrix-json-16k': 16384}[workload]
    return json.dumps({'data': 'x' * (size - 11)}, separators=(',', ':')).encode()


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = True


class LoadgenTests(unittest.TestCase):
    def run_fixture(self, mode='valid', workload='stream', *, warm=0, extra=None):
        global sequence
        sequence += 1
        temp = None if args.artifacts else tempfile.TemporaryDirectory(prefix='capsid-loadgen-')
        folder = args.artifacts / f'{sequence:03d}-{mode}-{workload}' if args.artifacts else Path(temp.name)
        if args.artifacts: folder.mkdir()
        expected = payload(workload)
        phase = folder / 'phase.txt'
        requests = []
        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *_): pass
            def do_GET(self):
                requests.append(self.path)
                if mode == 'reset':
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.close_connection = True
                    return
                body, status = expected, 200
                if mode == 'timeout': time.sleep(.2)
                if mode == 'middle' or mode == 'warm-failure' and not phase.exists() or mode == 'late-failure' and phase.exists():
                    # All three positions checked by the old stream verifier
                    # remain correct; the complete byte sequence is wrong.
                    body = body[:100] + b'!' + body[101:]
                if mode == 'suffix': body += b'!'
                if mode == 'short': body = body[:-1]
                if mode == 'status': status = 503
                self.send_response(status)
                self.send_header('Content-Length', str(len(body) + (1 if mode == 'truncated' else 0)))
                self.end_headers()
                if mode == 'timeout-body': time.sleep(.2)
                try:
                    self.wfile.write(body)
                    self.wfile.flush()
                    if mode == 'truncated':
                        self.close_connection = True
                        self.connection.shutdown(socket.SHUT_WR)
                except (BrokenPipeError, ConnectionResetError): pass
        server = Server(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .01})
        thread.start()
        env = dict(os.environ, CAPSID_BENCH_TARGET=f'http://127.0.0.1:{server.server_port}',
            CAPSID_BENCH_WORKLOAD=workload, CAPSID_BENCH_SIDE='fixture', CAPSID_BENCH_ROUND='1',
            CAPSID_BENCH_WARMUP_S=str(warm), CAPSID_BENCH_DURATION_S='1',
            CAPSID_BENCH_CONNECTIONS='2', CAPSID_BENCH_INFLIGHT='2', CAPSID_BENCH_TIMEOUT_MS='60',
            CAPSID_BENCH_SAMPLES_OUT=str(folder / 'samples.jsonl'),
            CAPSID_BENCH_CORRECTNESS_OUT=str(folder / 'correctness.json'), CAPSID_BENCH_PHASE_OUT=str(phase))
        env.update(extra or {})
        try:
            run = subprocess.run([str(args.loadgen.resolve())], env=env, capture_output=True, timeout=15)
            rows = [json.loads(line) for line in (folder / 'samples.jsonl').read_text().splitlines()] if (folder / 'samples.jsonl').exists() else []
            verdict = json.loads((folder / 'correctness.json').read_text()) if (folder / 'correctness.json').exists() else None
            if args.artifacts:
                (folder / 'stdout.log').write_bytes(run.stdout)
                (folder / 'stderr.log').write_bytes(run.stderr)
                (folder / 'invocation.json').write_text(json.dumps({'exitCode': run.returncode,
                    'mode': mode, 'workload': workload, 'warmupSeconds': warm, 'durationSeconds': 1,
                    'connections': 2, 'inflight': 2, 'requestTimeoutMs': 60, 'requestsObserved': len(requests),
                    'environmentOverrides': extra or {},
                    'expectedBodySHA256': hashlib.sha256(expected).hexdigest(),
                    'loadgenSHA256': hashlib.sha256(args.loadgen.read_bytes()).hexdigest(),
                    'scope': 'Local correctness fixture; not a performance observation or publishable cell.'}, indent=2) + '\n')
            return run, rows, verdict, phase.exists(), len(requests)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
            if temp: temp.cleanup()

    def test_valid_frozen_payloads_and_independent_byte_claims(self):
        for workload in ('matrix-json-1k', 'matrix-json-16k', 'stream'):
            with self.subTest(workload=workload):
                run, rows, verdict, _, observed = self.run_fixture(workload=workload, warm=1)
                self.assertEqual(0, run.returncode, run.stderr.decode(errors='replace'))
                self.assertEqual(['warmup', 'measured'], [r['phase'] for r in rows])
                self.assertTrue(verdict['ok'])
                for item in [*rows, verdict]:
                    self.assertEqual('exact-bytes-v2', item['response_validation'])
                    self.assertEqual(workload, item['workload'])
                    self.assertEqual(len(payload(workload)), item['payload_bytes'])
                    self.assertEqual(hashlib.sha256(payload(workload)).hexdigest(), item['expected_body_sha256'])
                self.assertEqual(observed, verdict['requests_attempted'])
                self.assertEqual(observed, verdict['responses_checked'])
                self.assertEqual(observed, sum(row['completed'] for row in rows))

    def test_internal_stream_corruption_fails_full_body_check(self):
        run, _, verdict, _, _ = self.run_fixture('middle')
        self.assertFalse(verdict['ok'], verdict)
        self.assertGreater(verdict['mismatches'], 0)
        self.assertNotEqual(0, run.returncode)

    def test_transport_errors_cannot_report_success(self):
        run, _, verdict, _, observed = self.run_fixture('reset')
        self.assertFalse(verdict['ok'], verdict)
        self.assertEqual(0, verdict['responses_checked'])
        self.assertGreater(verdict['errors'], 0)
        self.assertEqual(0, verdict['timeouts'], 'connection reset is not a deadline timeout')
        self.assertGreaterEqual(observed, 1)
        self.assertNotEqual(0, run.returncode)

    def test_real_deadline_timeouts_cannot_report_success(self):
        for mode in ('timeout', 'timeout-body'):
            with self.subTest(mode=mode):
                run, rows, verdict, _, _ = self.run_fixture(mode)
                self.assertFalse(verdict['ok'], verdict)
                self.assertGreater(verdict['timeouts'], 0)
                self.assertEqual(0, verdict['responses_checked'])
                self.assertEqual(sum(r['requests_attempted'] for r in rows), verdict['requests_attempted'])
                self.assertNotEqual(0, run.returncode)

    def test_invalid_status_aborts_warmup(self):
        run, rows, verdict, measured_started, _ = self.run_fixture('status', warm=1)
        self.assertFalse(verdict['ok'])
        self.assertNotEqual(0, run.returncode)
        self.assertEqual(['warmup'], [r['phase'] for r in rows])
        self.assertFalse(measured_started)

    def test_wrong_length_or_truncated_body_is_never_success(self):
        for mode in ('suffix', 'short', 'truncated'):
            with self.subTest(mode=mode):
                run, _, verdict, _, _ = self.run_fixture(mode)
                self.assertFalse(verdict['ok'])
                self.assertNotEqual(0, run.returncode)
                self.assertGreater(verdict['errors'], 0)

    def test_wrong_warm_or_measured_response_fails_its_phase(self):
        for mode, phases in (('warm-failure', ['warmup']), ('late-failure', ['warmup', 'measured'])):
            with self.subTest(mode=mode):
                run, rows, verdict, _, _ = self.run_fixture(mode, warm=1)
                self.assertFalse(verdict['ok'])
                self.assertNotEqual(0, run.returncode)
                self.assertEqual(phases, [r['phase'] for r in rows])

    def test_invalid_parameters_fail_before_dispatch(self):
        for field, value in (('CONNECTIONS', '0'), ('INFLIGHT', '-1'), ('WARMUP_S', 'bad'),
                             ('DURATION_S', '0'), ('ROUND', '0'), ('TIMEOUT_MS', '0')):
            with self.subTest(field=field):
                run, rows, verdict, _, observed = self.run_fixture(extra={'CAPSID_BENCH_' + field: value})
                self.assertNotEqual(0, run.returncode)
                self.assertEqual(0, observed)
                self.assertEqual([], rows)
                self.assertIsNone(verdict)


if __name__ == '__main__': unittest.main()
