"""Cold controller rules and owned three-generation HTTP/compiler fixtures."""
import argparse
import copy
import http.server
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--module', type=Path, default=Path(__file__).resolve().parents[1] / 'bench/cold_control.py')
args, rest = parser.parse_known_args()
sys.argv[1:] = rest
sys.path.insert(0, str(args.module.resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / 'helpers'))
import cold_control as control
import cold_probe as probe
from measurement_runtime import ProbeError, ProcFS
from cold_measurement_fixture import synthetic_round, peer_executable, compiler_executable


def generator():
    spec = importlib.util.spec_from_file_location('cold_test_generator', args.module.with_name('gen-cold-start-apps.py'))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class ControlTests(unittest.TestCase):
    def test_cpu_ranges_are_bounded_by_the_available_indices(self):
        self.assertEqual({1, 2, 4, 5}, control.cpu_selection('1-2,4,5', {1, 2, 4, 5}))
        for text in ('', '-1', '1,,2', '1-99999999999', '3-1', '0', '1-5'):
            with self.subTest(text=text), self.assertRaises(ProbeError): control.cpu_selection(text, {1, 2, 4, 5})

    def test_valid_rows_require_real_samples_and_even_warm_median(self):
        for stack in ('capsid-source', 'capsid-bytecode', 'node', 'deno', 'workerd'):
            row = synthetic_round(stack, '10k', 1, warm=2)
            row.update(warm_latencies_ms=[1, 3], warm_ms=2)
            self.assertIs(row, control.checked_round(row, 2, observed={(201, 10001)}))

    def test_bad_timings_counts_and_correctness_are_rejected(self):
        mutations = [lambda r: r.update(total_ms=79), lambda r: r.update(ready_ms=float('nan')),
            lambda r: r.update(warm_n=49), lambda r: r.update(warm_ms=True),
            lambda r: r.update(warm_latencies_ms=[2] * 50), lambda r: r.update(warm_latencies_ms=[1] * 49),
            lambda r: r['correctness'].update(ok=False), lambda r: r['correctness'].update(errors=1),
            lambda r: r['correctness'].update(responses_checked=50), lambda r: r['cleanup'].update(ownedPidsRemaining=1)]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                row = synthetic_round('node', '10k', 1)
                mutate(row)
                with self.assertRaises(ProbeError): control.checked_round(row, 50)

    def test_pinned_members_and_recursive_sum_cannot_be_replaced_by_a_total(self):
        mutations = [lambda r: r.update(probeProcess=dict(r['measurementRoot'])),
            lambda r: r['measurementRoot'].update(start=999), lambda r: r['measurementProcesses'].append(dict(r['measurementRoot'])),
            lambda r: r['pssSamples'][0]['processes'][0].update(pss_kb=0),
            lambda r: r['pssSamples'][0]['processes'][0].update(pss_kb=1025),
            lambda r: r['pssSamples'][0]['processes'][0].update(start=999),
            lambda r: r.update(pss_readings_kb=[1024, 1025]), lambda r: r.update(pss_kb=0),
            lambda r: r.update(transport='unknown'), lambda r: r.update(posture='off')]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                row = synthetic_round('capsid-source', '10k', 1)
                mutate(row)
                with self.assertRaises(ProbeError): control.checked_round(row, 50)
        with self.assertRaisesRegex(ProbeError, 'never observed'):
            control.checked_round(synthetic_round('node', '10k', 1), 50, observed={(999, 1)})

    def test_http_checks_every_status_and_body_byte(self):
        for status, body in ((200, control.EXPECTED_BODY), (500, control.EXPECTED_BODY),
                             (200, b'wrong'), (200, control.EXPECTED_BODY + b'x'), (200, control.EXPECTED_BODY[:-1])):
            with self.subTest(status=status, body=body):
                class Handler(http.server.BaseHTTPRequestHandler):
                    def do_GET(self):
                        self.send_response(status)
                        self.send_header('Content-Length', str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)
                    def log_message(self, *_): pass
                with http.server.HTTPServer(('127.0.0.1', 0), Handler) as server:
                    thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .01}, daemon=True)
                    thread.start()
                    try:
                        if status == 200 and body == control.EXPECTED_BODY:
                            self.assertTrue(probe.exact_request(server.server_port)['ok'])
                        else:
                            with self.assertRaises(ProbeError): probe.exact_request(server.server_port)
                    finally:
                        server.shutdown()
                        thread.join()

    def test_private_port_rewrite_does_not_mutate_shared_fixtures(self):
        gen = generator()
        with tempfile.TemporaryDirectory(prefix='cold-listener-fixture-') as folder:
            root = Path(folder)
            apps = root / 'apps'
            apps.mkdir()
            expected = {}
            for stack in ('node', 'deno', 'workerd'):
                data = gen.body_to(10000)[0] + getattr(gen, stack + '_entry')()
                file = apps / ('app-' + stack + '-10k.mjs')
                file.write_text(data, encoding='utf-8')
                expected[file] = file.read_bytes()
            adapter = probe.ColdProbe(binaries={s: root / s for s in ('node', 'deno', 'workerd')}, apps=apps,
                                      compiled={}, out=root, cpuset='0-3')
            for index, stack in enumerate(('node', 'deno', 'workerd')):
                directory = root / stack
                directory.mkdir()
                adapter.prepare_http(stack, '10k', directory, 30000 + index)
                data = (directory / 'runtime/bundle.mjs').read_text()
                self.assertTrue(data.startswith(gen.body_to(10000)[0]))
                if stack != 'workerd': self.assertIn(str(30000 + index), data)
                else: self.assertIn('127.0.0.1:30002', (directory / 'runtime/server.capnp').read_text())
            self.assertTrue(all(file.read_bytes() == data for file, data in expected.items()))

    def test_ready_marker_alone_and_foreign_listener_never_qualify(self):
        class Owner:
            def alive(self): return True
            def listeners_owned(self, _): return False
        with tempfile.TemporaryDirectory() as folder:
            stdout = Path(folder) / 'stdout'
            stdout.write_bytes(b'READY\n')
            with self.assertRaisesRegex(ProbeError, 'did not become ready'):
                probe.wait_ready(Owner(), 1, stdout, True, time.monotonic() + .02)
            with patch.object(Owner, 'listeners_owned', side_effect=ProbeError('foreign-listener', 'foreign listener')):
                with self.assertRaisesRegex(ProbeError, 'foreign listener'):
                    probe.wait_ready(Owner(), 1, stdout, True, time.monotonic() + 1)


@unittest.skipUnless(sys.platform == 'linux', 'owned process-tree and native compile tests require Linux')
class LinuxProbeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='capsid-cold-probe-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.apps = self.root / 'apps'
        self.apps.mkdir()
        gen = generator()
        (self.apps / 'app-node-10k.mjs').write_text(gen.body_to(10000)[0] + gen.node_entry(), encoding='utf-8')

    def adapter(self, mode):
        out = self.root / ('archive-' + mode)
        out.mkdir()
        return probe.ColdProbe(binaries={'node': peer_executable(self.root, mode)}, apps=self.apps, compiled={},
            out=out, cpuset=','.join(str(p) for p in sorted(os.sched_getaffinity(0))[:4]), warm_requests=2,
            settle_seconds=.02, pss_interval=.02, round_timeout=10)

    def test_actual_grandchildren_are_sampled_and_reaped(self):
        adapter = self.adapter('ok')
        row = adapter(stack='node', size='10k', round_no=1)
        self.assertEqual(3, len(row['measurementProcesses']))
        self.assertEqual(3, row['correctness']['responses_checked'])
        self.assertEqual(0, row['cleanup']['ownedPidsRemaining'])
        self.assertTrue(all(ProcFS().identity(p['pid']) is None for p in row['measurementProcesses']))

    def test_wrong_first_warm_exit_and_unreadable_pss_are_fatal_and_clean(self):
        for mode in ('bad-first', 'bad-warm', 'exit-warm', 'pss-failed'):
            with self.subTest(mode=mode):
                adapter = self.adapter(mode)
                context = patch.object(probe, 'stable_pss', side_effect=ProbeError('pss-unreadable', 'unreadable PSS'))
                from contextlib import nullcontext
                with context if mode == 'pss-failed' else nullcontext():
                    with self.assertRaises(ProbeError): adapter(stack='node', size='10k', round_no=1)
                result = next(adapter.out.glob('probe-*/result.json'))
                record = json.loads(result.read_text())
                self.assertEqual('error', record['status'])
                cleanup = json.loads(next(adapter.out.glob('probe-*/cleanup.json')).read_text())
                self.assertEqual(0, cleanup['ownedPidsRemaining'])
                self.assertTrue(all(ProcFS().identity(p['pid']) is None for p in cleanup['processes']))

    def test_compile_uses_private_native_outputs_and_checks_their_identity(self):
        source = self.apps / 'app-node-10k.mjs'
        file = probe.compile_fixture(compiler_executable(self.root), source, self.root / 'compile', '0-3')
        self.assertEqual(b'synthetic bytecode test fixture', file.read_bytes())
        launch = json.loads((file.parent / 'launch.json').read_text())
        output = Path(launch['command'][launch['command'].index('--bytecode-out') + 1])
        self.assertTrue(output.is_relative_to('/tmp'))
        self.assertFalse(output.exists(), 'native temporary outputs were not removed')
        self.assertEqual(0, json.loads((file.parent / 'cleanup.json').read_text())['ownedPidsRemaining'])

    def test_compile_failure_is_not_a_partial_success(self):
        with self.assertRaisesRegex(ProbeError, 'compiler failed'):
            probe.compile_fixture(compiler_executable(self.root, bad=True), self.apps / 'app-node-10k.mjs',
                                  self.root / 'compile-bad', '0-3')
        self.assertFalse((self.root / 'compile-bad/bundle.qjsb').exists())
        self.assertEqual(0, json.loads((self.root / 'compile-bad/cleanup.json').read_text())['ownedPidsRemaining'])


if __name__ == '__main__': unittest.main()
