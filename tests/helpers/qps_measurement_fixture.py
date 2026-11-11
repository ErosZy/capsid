"""Synthetic QPS memory shapes for contract tests, never actual measurements."""
import copy
import hashlib


def synthetic_memory(stack, phase, base_pid=None, *, seconds=15, rate=10, clients=15):
    root_roles = {'capsid': [('host', 'worker')], 'workerd': [('process', 'process')],
        'fastapi': [('process', 'process')], 'flask': [('master', 'worker')],
        'php': [('nginx', 'nginx-worker'), ('fpm-master', 'fpm-worker')]}[stack]
    index = ('capsid','workerd','fastapi','flask','php').index(stack)
    base = base_pid if base_pid is not None else index*1000 + (200 if phase == 'lowLoad' else 400)
    probe = {'pid': 41, 'start': 1, 'parent': 1, 'session': 41, 'state': 'S'}
    def process(pid, parent, session):
        return {'pid': pid, 'start': 1000 + pid, 'parent': parent, 'session': session, 'state': 'S'}
    roots, members, trees = [], [], []
    for root_number, (role, child_role) in enumerate(root_roles):
        pid = base + root_number * 10
        root = dict(process(pid, probe['pid'], pid), role=role, childRole=child_role)
        owned = [dict(process(pid, probe['pid'], pid), role=role)]
        if role != 'process': owned += [dict(process(pid + 1, pid, pid), role=child_role)]
        roots.append(root); members += owned
        trees.append({'root': process(pid, probe['pid'], pid), 'signals': [], 'processes': copy.deepcopy(owned), 'returncode': -9})
    loadgen = []
    if phase == 'serving':
        loadgen = [process(base + 100 + index, probe['pid'], base + 100 + index) for index in range(clients)]
        trees += [{'root': p, 'signals': [], 'processes': [p], 'returncode': 0} for p in loadgen]
    parts = [dict(pid=p['pid'], start=p['start'], pss_kb=1024 // len(members)) for p in members]
    roles = {}
    for member, part in zip(members, parts): roles[member['role']] = roles.get(member['role'], 0) + part['pss_kb']
    result = {'method': 'recursive-process-tree-pss', 'stack': stack, 'phase': phase, 'pss_kb': 1024,
        'readinessResponse': {'ok':True,'status':200,'latency_ms':1.,
            'bodySHA256':hashlib.sha256(b'{"data":"'+b'x'*1013+b'"}').hexdigest()},
        'roles_kb': roles, 'pss_readings_kb': [1024, 1024],
        'pssSamples': [copy.deepcopy({'pss_kb': 1024, 'processes': parts}) for _ in range(2)],
        'measurementProcesses': members, 'measurementRoots': roots, 'probeProcess': probe,
        'loadgenProcesses': loadgen, 'cleanup': {'ownedPidsRemaining': 0, 'trees': trees},
        'startedMonotonic': index*500 + (10 if phase == 'lowLoad' else 50),
        'completedMonotonic': index*500 + (30 if phase == 'lowLoad' else 400),
        'cleanupCompletedMonotonic': index*500 + (31 if phase == 'lowLoad' else 401)}
    if phase == 'lowLoad':
        body = b'{"data":"' + b'x' * 1013 + b'"}'
        digest = hashlib.sha256(body).hexdigest()
        result.update(beforeMeasuredLoad=True, validatedRequests=seconds*rate, duration_s=seconds,
            workload='matrix-json-1k', requests=[{'ok': True, 'status': 200, 'sequence': i+1,
                'bodySHA256': digest, 'latency_ms': 1., 'scheduled_s': i/rate,
                'started_s': i/rate, 'completed_s': i/rate+.001} for i in range(seconds*rate)])
    return result
