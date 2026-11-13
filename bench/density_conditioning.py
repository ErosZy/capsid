"""Per-instance density conditioning through the original exact HTTP client.

The Host keeps its production scheduler. Only the measured, identity-pinned
worker children pause while one real request per worker enters dispatch; all
children resume before responses, settling and whole-tree PSS sampling.
"""
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import os
import re
import signal
import sys
import time

from density_probe import DensityProbe, admin_request, validated_wave
from measurement_runtime import ProbeError

CAPSID_CONDITIONING = 'owned-worker-dispatch-v1'
WORKERD_CONDITIONING = 'targeted-cell-socket-v1'


def http_inflight(raw):
    matches = re.findall(rb'^capsid_channel_http_inflight\{scope="service",listener=""\} ([0-9]+)$',
                         raw, re.MULTILINE)
    if len(matches) != 1:
        raise ProbeError('barrier-metrics', 'one exact service HTTP inflight gauge is required')
    return int(matches[0])


@contextmanager
def paused_owned_workers(tree, worker, count):
    executable = Path(worker).resolve()
    members = tree.discover()
    selected = []
    for identity in members.values():
        if identity.pid == tree.root.pid or identity.state in ('Z', 'X'):
            continue
        try:
            actual = Path(os.readlink(tree.proc.root / str(identity.pid) / 'exe')).resolve()
        except (FileNotFoundError, ProcessLookupError):
            continue
        if actual == executable:
            selected.append(identity)
    if len(selected) != count or len(members) != count + 1:
        raise ProbeError('barrier-members', 'exact owned worker set is required',
                         expected=count, observed=len(selected), treeMembers=len(members))
    selected.sort(key=lambda p: p.pid)
    scope = {'workers': [{'pid': p.pid, 'start': p.start} for p in selected],
             'rootPid': tree.root.pid, 'rootPaused': False}
    started = time.monotonic()
    stopped = []
    try:
        for identity in selected:
            stopped.append(identity)
            tree._signal(identity, signal.SIGSTOP)
        deadline = time.monotonic() + .5
        while True:
            current = [tree.proc.identity(p.pid) for p in selected]
            if any(not p.same(now) for p, now in zip(selected, current)):
                raise ProbeError('barrier-members', 'worker identity changed while pausing')
            if all(p.state in ('T', 't') for p in current):
                break
            if time.monotonic() >= deadline:
                raise ProbeError('barrier-pause-timeout', 'owned workers did not pause promptly')
            time.sleep(.005)
        root = tree.proc.identity(tree.root.pid)
        if not tree.root.same(root) or root.state in ('T', 't', 'Z', 'X'):
            raise ProbeError('barrier-root', 'host must remain alive and running')
        yield scope
    finally:
        primary = sys.exc_info()[1]
        signal_start = len(tree.signals)
        failures = []
        # Attempt every resume even when one operation fails. The owner still
        # performs complete tree cleanup after a failed conditioning probe.
        for identity in stopped:
            try:
                tree._signal(identity, signal.SIGCONT)
            except BaseException as exc:
                failures.append({'pid': identity.pid, 'start': identity.start,
                                 'type': type(exc).__name__, 'message': str(exc)})
        issued = sum(s['signal'] == 'SIGCONT' for s in tree.signals[signal_start:])
        scope.update(pauseSeconds=time.monotonic() - started,
                     resumeSignalsAttempted=len(stopped), resumeSignalsIssued=issued,
                     resumeFailures=failures)
        if failures or issued != len(stopped):
            raise ProbeError('barrier-resume', 'every pinned worker must receive its resume signal',
                             attempted=len(stopped), issued=issued, failures=failures,
                             primary=None if primary is None else str(primary)) from primary


class ConditionedDensityProbe(DensityProbe):
    def prepare(self, folder, stack, size, count, ports, nonce):
        argv = super().prepare(folder, stack, size, count, ports, nonce)
        self.barrier_admin = folder / 'run/admin.sock'
        self.barrier_log = folder / 'prewarm-barriers.jsonl'
        return argv

    def initial_wave_requests(self, stack, count, maximum, concurrency):
        if stack == 'workerd':
            return min(concurrency, count)
        return super().initial_wave_requests(stack, count, maximum, concurrency)

    def cover(self, tree, counter, stack, ports, count, target, nonce, output):
        started = time.monotonic()
        if stack == 'workerd':
            if len(ports) != count or len(set(ports)) != count:
                raise ProbeError('conditioning-ports', 'each workerd cell requires its own socket')
            result = super().cover(tree, counter, stack, ports, count, target, nonce, output)
            if result['responses_checked'] != count * target or set(counter.coverage.counts.values()) != {target}:
                raise ProbeError('conditioning-coverage', 'each cell must complete one request in every wave')
            result.update(conditioningMethod=WORKERD_CONDITIONING, conditioningWaves=target,
                          conditioningSeconds=time.monotonic() - started,
                          externalWorkerExecutionBarrier=False)
            return result
        if stack != 'capsid':
            raise ProbeError('conditioning-stack', 'unsupported density stack')
        deadline = started + self.coverage_timeout
        responses = 0
        maximum = max(64, count * target * (32 if target > 1 else 8))
        with self.barrier_log.open('w', encoding='utf-8') as evidence:
            for ordinal in range(target):
                if time.monotonic() >= deadline or not tree.alive():
                    raise ProbeError('barrier-coverage', 'prewarm coverage deadline or host lifetime ended')
                if http_inflight(admin_request(self.barrier_admin, 'GET', '/metrics')) != 0:
                    raise ProbeError('barrier-not-idle', 'previous HTTP requests must finish before the next wave')
                selected = [ports[i % len(ports)] for i in range(count)]
                scope = None
                with ThreadPoolExecutor(max_workers=1) as sender:
                    try:
                        with paused_owned_workers(tree, self.worker, count) as scope:
                            future = sender.submit(validated_wave, selected, nonce, connect_parallel=self.parallel)
                            dispatch_deadline = min(deadline, time.monotonic() + 1)
                            while True:
                                observed = http_inflight(admin_request(self.barrier_admin, 'GET', '/metrics'))
                                if observed == count:
                                    scope['observedHttpInflightBeforeResume'] = observed
                                    break
                                if observed > count:
                                    raise ProbeError('barrier-dispatch-count', 'unexpected extra inflight requests')
                                if future.done():
                                    future.result()
                                    raise ProbeError('barrier-early-response', 'HTTP wave finished while workers were paused')
                                if time.monotonic() >= dispatch_deadline:
                                    raise ProbeError('barrier-dispatch-timeout', 'host did not dispatch the full wave promptly',
                                                     expected=count, observed=observed)
                                time.sleep(.005)
                        observations = future.result()
                    finally:
                        if scope is not None:
                            evidence.write(json.dumps({'wave': ordinal + 1, **scope}, sort_keys=True) + '\n')
                            evidence.flush()
                if len(observations) != count:
                    raise ProbeError('barrier-response-count', 'every request must have a validated response')
                for response in observations:
                    responses += 1
                    output.write(json.dumps(dict(response, sequence=responses, waveRequests=count,
                        warmupConcurrencyLimit=self.warmup_concurrency(count),
                        externalWorkerExecutionBarrier=True), sort_keys=True) + '\n')
                output.flush()
                log_deadline = min(deadline, time.monotonic() + 3)
                while True:
                    counter.poll()
                    if sum(counter.coverage.counts.values()) == responses:
                        break
                    if time.monotonic() >= log_deadline:
                        raise ProbeError('coverage-count', 'missing per-worker counter records after validated responses')
                    time.sleep(.01)
                if len(counter.coverage.counts) != count or any(n != ordinal + 1 for n in counter.coverage.counts.values()):
                    raise ProbeError('barrier-worker-coverage', 'each wave must reach each original worker once',
                                     expected=count, observed=counter.coverage.counts)
        if time.monotonic() >= deadline:
            raise ProbeError('coverage-deadline', 'coverage completed after its original deadline')
        counter.coverage.verify(target, responses)
        assert responses <= maximum
        return {'ok': True, 'errors_as_success': False, 'responses_checked': responses,
            'errors': 0, 'timeouts': 0, 'mismatches': 0,
            'warmupConcurrencyLimit': self.warmup_concurrency(count), 'maximumWaveRequests': count,
            'maximumRequestsAllowed': maximum, 'coverageTimeoutSeconds': self.coverage_timeout,
            'externalWorkerExecutionBarrier': True, 'barrierWaves': target,
            'conditioningMethod': CAPSID_CONDITIONING, 'conditioningWaves': target,
            'conditioningSeconds': time.monotonic() - started,
            'conditioningEvidence': 'runtime/prewarm-barriers.jsonl'}
