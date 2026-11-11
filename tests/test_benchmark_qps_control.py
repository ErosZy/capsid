"""Portable QPS contract tests with explicitly synthetic round records."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys
import unittest

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--module', type=Path, default=Path(__file__).resolve().parents[1] / 'bench/qps_control.py')
args, remaining = parser.parse_known_args()
sys.argv[1:] = remaining
sys.path.insert(0, str(args.module.resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / 'helpers'))
import qps_control as control
from measurement_runtime import ProbeError
from qps_measurement_fixture import synthetic_memory


def fixture(stack='capsid', workload='stream', round_no=1):
    body = b'b' * 341 + b'c' * 341 + b'd' * 342 if workload == 'stream' else json.dumps(
        {'data': 'x' * ((1024 if workload == 'matrix-json-1k' else 16384) - 11)}, separators=(',', ':')).encode()
    claims = {'workload': workload, 'response_validation': 'exact-bytes-v2',
              'expected_body_sha256': hashlib.sha256(body).hexdigest(), 'payload_bytes': len(body)}
    rows = [dict(claims, phase=phase, side=stack, round=round_no, connections=64, inflight=64,
                 request_timeout_ms=30000, duration_s=seconds, completed=seconds * 100,
                 requests_attempted=seconds * 100, responses_checked=seconds * 100,
                 errors=0, timeouts=0, qps=100.0, p50_ms=1., p95_ms=2., p99_ms=3.)
            for phase, seconds in (('warmup', 5), ('measured', 15))]
    verdict = dict(claims, ok=True, errors_as_success=False, requests_attempted=2000,
                   responses_checked=2000, errors=0, timeouts=0, mismatches=0)
    return rows, verdict


class QpsControlTests(unittest.TestCase):
    def test_all_five_stacks_three_workloads_and_five_rounds(self):
        for stack in ('capsid', 'workerd', 'fastapi', 'flask', 'php'):
            for workload in ('matrix-json-1k', 'matrix-json-16k', 'stream'):
                for round_no in range(1, 6):
                    rows, verdict = fixture(stack, workload, round_no)
                    self.assertIs(rows[1], control.checked_round(rows, verdict, stack, workload, round_no))

    def test_frozen_payload_bytes_include_every_stream_third(self):
        self.assertEqual(b'b' * 341 + b'c' * 341 + b'd' * 342, control.expected_body('stream'))
        for workload, size in (('matrix-json-1k', 1024), ('matrix-json-16k', 16384)):
            body = control.expected_body(workload)
            self.assertEqual(size, len(body))
            self.assertEqual({'data': 'x' * (size - 11)}, json.loads(body))
        for value in ('', None, 'matrix-json-4k', 'stream8k'):
            with self.subTest(value=value), self.assertRaises(ProbeError): control.expected_body(value)

    def test_payload_claims_cannot_be_missing_swapped_or_retyped(self):
        for location in (0, 1, 2):
            for field, bad in (('workload', 'matrix-json-1k'), ('response_validation', 'legacy-v1'),
                               ('payload_bytes', True), ('expected_body_sha256', '0' * 64)):
                for missing in (False, True):
                    with self.subTest(location=location, field=field, missing=missing):
                        rows, verdict = fixture()
                        item = [*rows, verdict][location]
                        if missing: item.pop(field)
                        else: item[field] = bad
                        with self.assertRaises(ProbeError): control.checked_round(rows, verdict, 'capsid', 'stream', 1)

    def test_dispatched_checked_and_completed_counts_are_all_required(self):
        for location in (0, 1, 2):
            for field in ('requests_attempted', 'responses_checked'):
                for bad in (0, 1, True, 2000., -1, None):
                    with self.subTest(location=location, field=field, value=bad):
                        rows, verdict = fixture()
                        [*rows, verdict][location][field] = bad
                        with self.assertRaises(ProbeError): control.checked_round(rows, verdict, 'capsid', 'stream', 1)

    def test_failures_in_either_phase_and_false_verdicts_cannot_qualify(self):
        for location in (0, 1, 2):
            for field in ('errors', 'timeouts'):
                for bad in (1, True, None):
                    with self.subTest(location=location, field=field, value=bad):
                        rows, verdict = fixture()
                        [*rows, verdict][location][field] = bad
                        with self.assertRaises(ProbeError): control.checked_round(rows, verdict, 'capsid', 'stream', 1)
        for field, bad in (('ok', False), ('ok', 1), ('errors_as_success', True), ('mismatches', 1)):
            rows, verdict = fixture(); verdict[field] = bad
            with self.subTest(field=field, value=bad), self.assertRaises(ProbeError):
                control.checked_round(rows, verdict, 'capsid', 'stream', 1)

    def test_phase_identity_round_connections_and_request_deadline_are_frozen(self):
        for index in (0, 1):
            for field, bad in (('side', 'workerd'), ('round', True), ('round', 2), ('connections', 1),
                               ('inflight', True), ('request_timeout_ms', 60)):
                rows, verdict = fixture(); rows[index][field] = bad
                with self.subTest(index=index, field=field), self.assertRaises(ProbeError):
                    control.checked_round(rows, verdict, 'capsid', 'stream', 1)
        rows, verdict = fixture()
        for bad in (None, [], rows[:1], list(reversed(rows)), [*rows, rows[0]], [None, rows[1]]):
            with self.subTest(rows=bad), self.assertRaises(ProbeError):
                control.checked_round(bad, verdict, 'capsid', 'stream', 1)

    def test_nonfinite_values_percentiles_and_rate_duration_conflicts_fail(self):
        for field, bad in (('qps', 99), ('qps', float('nan')), ('duration_s', 14), ('duration_s', 17),
                           ('duration_s', True), ('completed', 0), ('p95_ms', .5), ('p50_ms', -1),
                           ('p99_ms', float('inf'))):
            rows, verdict = fixture(); rows[1][field] = bad
            with self.subTest(field=field, value=bad), self.assertRaises(ProbeError):
                control.checked_round(rows, verdict, 'capsid', 'stream', 1)

    def test_observation_parameters_are_explicit_and_cannot_change_defaults(self):
        rows, verdict = fixture()
        for row in rows: row.update(connections=2, inflight=2, request_timeout_ms=60)
        with self.assertRaises(ProbeError): control.checked_round(rows, verdict, 'capsid', 'stream', 1)
        self.assertIs(rows[1], control.checked_round(rows, verdict, 'capsid', 'stream', 1,
            connections=2, inflight=2, request_timeout_ms=60))
        for kwargs in ({'warmup': -1}, {'duration': 0}, {'connections': True}, {'request_timeout_ms': 0}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ProbeError):
                control.checked_round(*fixture(), 'capsid', 'stream', 1, **kwargs)

    def test_actual_memory_shapes_require_all_roles_roots_and_fresh_phases(self):
        for stack in ('capsid','workerd','fastapi','flask','php'):
            phases = {phase: synthetic_memory(stack, phase) for phase in ('lowLoad','serving')}
            self.assertIs(phases, control.checked_memory_pair(phases, stack))
            for phase in phases:
                self.assertEqual(1024, sum(phases[phase]['roles_kb'].values()))

    def test_missing_unstable_or_incomplete_memory_never_becomes_a_zero_sample(self):
        for mutate in (lambda d:d.pop('measurementRoots'), lambda d:d.update(measurementProcesses=[]),
                lambda d:d.pop('pssSamples'), lambda d:d['pssSamples'][-1].update(pss_kb=0),
                lambda d:d.update(pss_readings_kb=[1024,2048]), lambda d:d['measurementProcesses'][0].update(state='Z'),
                lambda d:d.update(roles_kb={'process':True}), lambda d:d['measurementRoots'][0].update(parent=1),
                lambda d:d['pssSamples'][0]['processes'].pop(), lambda d:d['measurementProcesses'].pop()):
            value = synthetic_memory('capsid','serving');mutate(value)
            with self.subTest(mutate=mutate), self.assertRaises(ProbeError): control.checked_memory(value,'capsid','serving')

    def test_clients_cleanup_and_every_trickle_response_need_independent_proof(self):
        for phase, mutate in (
            ('serving',lambda d:d.update(loadgenProcesses=[])),
            ('serving',lambda d:d['cleanup'].update(ownedPidsRemaining=1)),
            ('serving',lambda d:d['cleanup']['trees'].pop()),
            ('serving',lambda d:d['cleanup']['trees'][-1].update(returncode=7)),
            ('serving',lambda d:d.update(cleanup=None)),
            ('lowLoad',lambda d:d.update(beforeMeasuredLoad=False)),
            ('lowLoad',lambda d:d['requests'][4].update(bodySHA256='0'*64)),
            ('lowLoad',lambda d:d['requests'][4].update(started_s=1)),
            ('lowLoad',lambda d:d['requests'].pop()),
            ('lowLoad',lambda d:d.update(duration_s=14)),
            ('lowLoad',lambda d:d['cleanup']['trees'][0]['processes'].pop())):
            value = synthetic_memory('php',phase);mutate(value)
            with self.subTest(phase=phase,mutate=mutate), self.assertRaises(ProbeError): control.checked_memory(value,'php',phase)

    def test_loaded_trees_cannot_reuse_low_load_processes_or_precede_cleanup(self):
        for mode in ('reused','overlap','different-controller','missing-phase'):
            phases={phase:synthetic_memory('capsid',phase) for phase in ('lowLoad','serving')}
            if mode=='reused':phases['serving']=synthetic_memory('capsid','serving',base_pid=200)
            if mode=='overlap':phases['serving']['startedMonotonic']=30
            if mode=='different-controller':phases['serving']['probeProcess']['pid']=42
            if mode=='missing-phase':phases.pop('lowLoad')
            with self.subTest(mode=mode),self.assertRaises(ProbeError):control.checked_memory_pair(phases,'capsid')


if __name__ == '__main__': unittest.main()
