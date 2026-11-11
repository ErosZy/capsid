"""Frozen QPS payload and round rules shared by the producer and validator."""
import hashlib
import math

from measurement_runtime import ProbeError

RESPONSE_VALIDATION = 'exact-bytes-v2'
WORKLOADS = ('matrix-json-1k', 'matrix-json-16k', 'stream')
ROOT_ROLES = {'capsid': {'host': 'worker'}, 'workerd': {'process': 'process'},
              'fastapi': {'process': 'process'}, 'flask': {'master': 'worker'},
              'php': {'nginx': 'nginx-worker', 'fpm-master': 'fpm-worker'}}


def expected_body(workload):
    if workload == 'stream': return b'b' * 341 + b'c' * 341 + b'd' * 342
    if workload not in WORKLOADS:
        raise ProbeError('qps-workload', 'unknown frozen QPS workload')
    size = 1024 if workload == 'matrix-json-1k' else 16384
    return b'{"data":"' + b'x' * (size - 11) + b'"}'


def number(value, label, minimum=0):
    if type(value) not in (int, float) or not math.isfinite(value) or value < minimum:
        raise ProbeError('qps-number', f'{label}: expected a finite number >= {minimum}')
    return value


def integer(value, label, minimum=0):
    if type(value) is not int or value < minimum:
        raise ProbeError('qps-integer', f'{label}: expected an integer >= {minimum}')
    return value


def close(actual, expected, label):
    number(actual, label)
    if abs(actual - expected) > max(1e-9, abs(expected) * 1e-12):
        raise ProbeError('qps-mismatch', f'{label}: raw value disagrees with request counts')


def checked_round(rows, verdict, stack, workload, round_no, *, warmup=5, duration=15,
                  connections=64, inflight=64, request_timeout_ms=30000):
    for value, label, minimum in ((round_no, 'round', 1), (warmup, 'warmup', 0),
            (duration, 'duration', 1), (connections, 'connections', 1),
            (inflight, 'inflight', 1), (request_timeout_ms, 'request timeout', 1)):
        integer(value, label, minimum)
    expected = expected_body(workload)
    claims = {'response_validation': RESPONSE_VALIDATION, 'workload': workload,
              'payload_bytes': len(expected), 'expected_body_sha256': hashlib.sha256(expected).hexdigest()}
    if not isinstance(rows, list) or len(rows) != 2 or [r.get('phase') for r in rows if isinstance(r, dict)] != ['warmup', 'measured']:
        raise ProbeError('qps-round', 'expected warmup and measured samples per round')
    if not isinstance(verdict, dict) or verdict.get('ok') is not True or verdict.get('errors_as_success') is not False:
        raise ProbeError('qps-correctness', 'missing exact-response correctness verdict')
    for item in [*rows, verdict]:
        for key, value in claims.items():
            if type(item.get(key)) is not type(value) or item[key] != value:
                raise ProbeError('qps-payload', f'{key}: missing or conflicting exact payload identity')
        for key in ('errors', 'timeouts', 'mismatches') if item is verdict else ('errors', 'timeouts'):
            if integer(item.get(key), key) != 0:
                raise ProbeError('qps-correctness', f'{key}: nonzero failure count')
    total = 0
    for row, target in zip(rows, (warmup, duration)):
        if (row.get('side') != stack or type(row.get('round')) is not int or row['round'] != round_no):
            raise ProbeError('qps-identity', 'wrong stack or round')
        if (type(row.get('connections')) is not int or row['connections'] != connections or
                type(row.get('inflight')) is not int or row['inflight'] != inflight or
                type(row.get('request_timeout_ms')) is not int or row['request_timeout_ms'] != request_timeout_ms):
            raise ProbeError('qps-protocol', 'wrong connection protocol or request timeout')
        completed = integer(row.get('completed'), 'completed', 1 if target else 0)
        for key in ('requests_attempted', 'responses_checked'):
            if integer(row.get(key), key) != completed:
                raise ProbeError('qps-count', f'{key}: dispatched/checked count disagrees with completed requests')
        elapsed = number(row.get('duration_s'), 'duration_s', target)
        if elapsed <= 0 or target and elapsed > target * 1.1:
            raise ProbeError('qps-duration', 'measurement duration exceeded protocol tolerance')
        close(row.get('qps'), completed / elapsed, 'qps')
        for key in ('p50_ms', 'p95_ms', 'p99_ms'):
            number(row.get(key), key)
        if not row['p50_ms'] <= row['p95_ms'] <= row['p99_ms']:
            raise ProbeError('qps-percentile', 'invalid percentile order')
        total += completed
    if integer(verdict.get('responses_checked'), 'responses_checked', 1) != total:
        raise ProbeError('qps-count', 'correctness count disagrees with completed requests')
    if integer(verdict.get('requests_attempted'), 'requests_attempted', 1) != total:
        raise ProbeError('qps-count', 'correctness dispatched count disagrees with completed requests')
    return rows[1]


def identity(value):
    if not isinstance(value, dict): raise ProbeError('qps-process', 'missing process identity')
    return integer(value.get('pid'), 'PID', 1), integer(value.get('start'), 'start tick')


def checked_memory(value, stack, phase, *, trickle_seconds=15, trickle_rate=10, expected_clients=15):
    integer(trickle_seconds, 'trickle seconds', 1); integer(trickle_rate, 'trickle rate', 1)
    integer(expected_clients, 'expected load generators', 1)
    if (stack not in ROOT_ROLES or phase not in ('lowLoad', 'serving') or not isinstance(value, dict) or
            value.get('method') != 'recursive-process-tree-pss' or value.get('stack') != stack or value.get('phase') != phase):
        raise ProbeError('qps-memory', 'missing recursive PSS phase identity')
    begin = number(value.get('startedMonotonic'), 'phase start')
    if number(value.get('completedMonotonic'), 'phase end') <= begin:
        raise ProbeError('qps-timing', 'invalid memory phase interval')
    number(value.get('cleanupCompletedMonotonic'), 'cleanup completion', value['completedMonotonic'])
    probe = identity(value.get('probeProcess'))
    ready=value.get('readinessResponse',{})
    if (not isinstance(ready,dict) or ready.get('ok') is not True or type(ready.get('status')) is not int or ready['status']!=200 or
            ready.get('bodySHA256')!=hashlib.sha256(expected_body('matrix-json-1k')).hexdigest()):
        raise ProbeError('qps-readiness','missing exact application-readiness response')
    number(ready.get('latency_ms'),'readiness latency')
    roots, members = value.get('measurementRoots'), value.get('measurementProcesses')
    if not isinstance(roots, list) or len(roots) != len(ROOT_ROLES[stack]) or not isinstance(members, list) or not members:
        raise ProbeError('qps-process', 'missing measured forest roots or members')
    root_ids = {identity(p) for p in roots}
    by_id = {identity(p): p for p in members}
    if len(root_ids) != len(roots) or len(by_id) != len(members) or not root_ids <= set(by_id) or probe in by_id:
        raise ProbeError('qps-process', 'duplicate, missing or probe process in serving PSS')
    if {p.get('role'): p.get('childRole') for p in roots} != ROOT_ROLES[stack]:
        raise ProbeError('qps-process', 'invalid serving process role roots')
    by_pid = {p['pid']: p for p in members}
    if len(by_pid) != len(members) or probe[0] in by_pid: raise ProbeError('qps-process', 'reused serving or probe PID')
    for root in roots:
        if (integer(root.get('session'), 'root session', 1) != root['pid'] or
                integer(root.get('parent'), 'root parent', 1) != probe[0] or
                by_id[identity(root)].get('session') != root['session'] or by_id[identity(root)].get('parent') != root['parent']):
            raise ProbeError('qps-process', 'serving root lacks its owned session or parent')
    for member in members:
        integer(member.get('session'), 'member session', 1); integer(member.get('parent'), 'member parent', 1)
        seen, current = set(), member
        while identity(current) not in root_ids:
            if current['pid'] in seen or current.get('parent') not in by_pid:
                raise ProbeError('qps-process', 'serving member has no owned root ancestry')
            seen.add(current['pid']); parent = by_pid[current['parent']]
            if identity(parent)[1] > identity(current)[1]: raise ProbeError('qps-process', 'invalid ancestry start ticks')
            current = parent
        root = next(p for p in roots if identity(p) == identity(current))
        if member.get('role') != (root['role'] if identity(member) in root_ids else root['childRole']):
            raise ProbeError('qps-process', 'PSS member role differs from its measured root')
        if member.get('state') not in ('R', 'S', 'D', 'I'): raise ProbeError('qps-process', 'nonliving serving member')
    samples = value.get('pssSamples')
    counts = {role: sum(p['role'] == role for p in members) for role in {p['role'] for p in members}}
    required_counts = ({'host': 1, 'worker': 1} if stack == 'capsid' else {'master': 1, 'worker': 1} if stack == 'flask'
        else {'nginx': 1, 'nginx-worker': 1, 'fpm-master': 1, 'fpm-worker': 1} if stack == 'php' else {'process': 1})
    if counts != required_counts:
        raise ProbeError('qps-process', 'serving forest differs from the frozen single-worker topology')
    if not isinstance(samples, list) or not 2 <= len(samples) <= 8:
        raise ProbeError('qps-memory', 'missing bounded stable PSS readings')
    for sample in samples:
        parts = sample.get('processes') if isinstance(sample, dict) else None
        if not isinstance(parts, list) or {identity(p) for p in parts} != set(by_id) or len(parts) != len(by_id):
            raise ProbeError('qps-memory', 'PSS reading does not cover every pinned process')
        total = sum(integer(p.get('pss_kb'), 'process PSS', 1) for p in parts)
        if integer(sample.get('pss_kb'), 'total PSS', 1) != total:
            raise ProbeError('qps-memory', 'PSS process sum disagrees with total')
    final = samples[-1]
    readings = value.get('pss_readings_kb')
    if (samples[-2]['pss_kb'] != final['pss_kb'] or readings != [final['pss_kb']] * 2 or
            not isinstance(readings, list) or any(type(v) is not int for v in readings)):
        raise ProbeError('qps-memory', 'PSS did not settle at two equal positive readings')
    roles = {}
    for part in final['processes']:
        role = by_id[identity(part)]['role']; roles[role] = roles.get(role, 0) + part['pss_kb']
    if (value.get('roles_kb') != roles or any(type(v) is not int for v in value['roles_kb'].values()) or
            integer(value.get('pss_kb'), 'PSS', 1) != final['pss_kb']):
        raise ProbeError('qps-memory', 'PSS role sums or summary differ from raw processes')
    clients = value.get('loadgenProcesses')
    if not isinstance(clients, list) or len({identity(p) for p in clients}) != len(clients):
        raise ProbeError('qps-process', 'missing or duplicate load-generator identities')
    client_ids = {identity(p) for p in clients}
    if ({p[0] for p in client_ids} & set(by_pid) or any(p[0] == probe[0] for p in client_ids) or
            (phase == 'lowLoad' and clients)):
        raise ProbeError('qps-process', 'load generator was included in serving PSS or trickle')
    cleanup = value.get('cleanup', {})
    if not isinstance(cleanup, dict) or type(cleanup.get('ownedPidsRemaining')) is not int or cleanup['ownedPidsRemaining'] != 0:
        raise ProbeError('qps-cleanup', 'missing complete owned-forest cleanup')
    trees = cleanup.get('trees')
    if (not isinstance(trees, list) or any(not isinstance(t, dict) or not isinstance(t.get('processes'), list) for t in trees) or
            {identity(t.get('root')) for t in trees} != root_ids | client_ids or len(trees) != len(root_ids | client_ids)):
        raise ProbeError('qps-cleanup', 'cleanup omitted an owned serving/client root')
    cleaned = {identity(p) for t in trees for p in t.get('processes', [])}
    if not (set(by_id) | client_ids) <= cleaned:
        raise ProbeError('qps-cleanup', 'cleanup omitted a pinned serving/client process')
    if any(type(t.get('returncode')) is not int or identity(t['root']) in client_ids and t['returncode'] != 0 for t in trees):
        raise ProbeError('qps-cleanup', 'load generator did not exit successfully')
    if phase == 'lowLoad':
        if value.get('beforeMeasuredLoad') is not True:
            raise ProbeError('qps-trickle', 'missing fresh low-load phase')
        count = trickle_seconds * trickle_rate
        requests = value.get('requests')
        if (integer(value.get('validatedRequests'), 'validatedRequests', 1) != count or
                not isinstance(requests, list) or len(requests) != count or value.get('workload') != 'matrix-json-1k'):
            raise ProbeError('qps-trickle', 'missing complete exact trickle requests')
        elapsed = number(value.get('duration_s'), 'low-load duration', trickle_seconds)
        if elapsed > trickle_seconds * 1.1: raise ProbeError('qps-trickle', 'trickle duration exceeded protocol tolerance')
        digest, previous = hashlib.sha256(expected_body('matrix-json-1k')).hexdigest(), 0
        for index, row in enumerate(requests):
            planned = index / trickle_rate
            if (not isinstance(row, dict) or row.get('ok') is not True or type(row.get('status')) is not int or row['status'] != 200 or
                    type(row.get('sequence')) is not int or row['sequence'] != index + 1 or row.get('bodySHA256') != digest):
                raise ProbeError('qps-trickle', 'trickle response identity or exact body failed')
            close(row.get('scheduled_s'), planned, 'trickle schedule')
            dispatched = number(row.get('started_s'), 'trickle dispatch', max(previous, planned))
            finished = number(row.get('completed_s'), 'trickle completion', dispatched)
            if dispatched - planned > .1 or finished > elapsed:
                raise ProbeError('qps-trickle', 'trickle was delayed or out of its measured interval')
            number(row.get('latency_ms'), 'trickle latency'); previous = finished
    elif len(clients) != expected_clients:
        raise ProbeError('qps-process', 'serving phase omitted actual per-round load generators')
    return value


def checked_memory_pair(phases, stack, **kwargs):
    if not isinstance(phases, dict) or set(phases) != {'lowLoad', 'serving'}:
        raise ProbeError('qps-memory', 'missing memory phases')
    low = checked_memory(phases['lowLoad'], stack, 'lowLoad', **kwargs)
    serving = checked_memory(phases['serving'], stack, 'serving', **kwargs)
    if (low['cleanupCompletedMonotonic'] >= serving['startedMonotonic'] or
            identity(low['probeProcess']) != identity(serving['probeProcess']) or
            {identity(p) for p in low['measurementProcesses']} & {identity(p) for p in serving['measurementProcesses']}):
        raise ProbeError('qps-process', 'low-load must precede a fresh serving forest')
    return phases
