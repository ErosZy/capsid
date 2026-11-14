"""Synthetic density records for validator controls, never measured results.

The process IDs, readings and responses below are deliberately constructed.
Production acceptance comes from separate real-process executions.
"""
import copy
import hashlib
import io
import json
from pathlib import Path
import tarfile
import uuid

from density_probe import instrument_fixture


def synthetic_density(root, directory, stack, size, phase, count, *, probe_index=1,
                      pss_kb=1024, source=None, final=True, ports=None, write=None):
    root = Path(root)
    if write is None:
        def write(path, data):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    encoded = lambda value: (json.dumps(value, sort_keys=True, allow_nan=False)+'\n').encode()
    target = 100 if phase == 'warm' else 1
    nonce = f'{probe_index:032x}'
    worker_ids = [str(uuid.UUID(int=(probe_index << 16) + i + 1, version=4)) for i in range(count)]
    root_pid = 10000 + probe_index * (count + 2)
    ready = [dict(pid=root_pid, start=probe_index, parent=1, session=root_pid, state='S')]
    if stack == 'capsid':
        ready += [dict(pid=root_pid+i+1, start=probe_index+i+1, parent=root_pid,
                       session=root_pid, state='S') for i in range(count)]
    each, rest = divmod(pss_kb, len(ready))
    if each <= 0:
        raise ValueError('synthetic process PSS must stay positive')
    parts = [dict(pid=p['pid'], start=p['start'], pss_kb=each + (i < rest)) for i,p in enumerate(ready)]
    sample = dict(pss_kb=pss_kb, processes=parts)
    ports = list(ports) if ports is not None else ([32000] if stack == 'capsid' else list(range(32000, 32000+count)))
    concurrency = max(64, count * 128)
    launch = dict(command=['synthetic process fixture; never executed'], stack=stack, size=size,
                  phase=phase, count=count, ports=ports, nonce=nonce,
                  mode='managed' if stack=='capsid' else 'workerd',
                  configuredParallelRequests=64, warmupConcurrencyLimit=concurrency)
    if stack == 'workerd':
        launch['command'].append('--control-fd=1')
    write(root/directory/'launch.json', encoded(launch))
    if source is None:
        source = "export default { fetch() { return new Response('synthetic fixture only'); } };\n"
    wrapper = 'runtime/apps/orders/v1/bundle.mjs' if stack=='capsid' else 'runtime/bundle.mjs'
    write(root/directory/wrapper, instrument_fixture(source, nonce).encode())
    runtime_index = dict(schema=1, archive=None, members=[])
    if stack == 'capsid':
        name = 'state/apps/orders/generations/sha256:'+'a'*64+'/COMPLETE'
        data = b'explicitly synthetic generation marker'
        memory = io.BytesIO()
        with tarfile.open(fileobj=memory, mode='w:') as tar:
            member = tarfile.TarInfo(name); member.size = len(data)
            tar.addfile(member, io.BytesIO(data))
        write(root/directory/'runtime-nonportable.tar', memory.getvalue())
        runtime_index.update(archive='runtime-nonportable.tar',
                             members=[dict(name=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())])
    write(root/directory/'runtime-nonportable.json', encoded(runtime_index))
    logs = ([json.dumps(dict(event='worker_ready',pid=p['pid'])) for p in ready[1:]] if stack=='capsid'
            else [json.dumps(dict(event='listen',socket=f's{i}',port=port)) for i,port in enumerate(ports)])
    requests, barriers, signals = [], [], []
    body_sha = hashlib.sha256(b'{"status":"ok","total":2039,"count":16}').hexdigest()
    workers = [dict(pid=p['pid'],start=p['start']) for p in ready[1:]]
    for ordinal in range(1,target+1):
        for i,worker in enumerate(worker_ids):
            logs.append('CAPSID_BENCH_DENSITY_V1 '+json.dumps(dict(nonce=nonce,worker=worker,count=ordinal)))
            request = dict(port=ports[i%len(ports)],status=200,ok=True,bodySHA256=body_sha,
                           sequence=len(requests)+1,waveRequests=count,warmupConcurrencyLimit=concurrency)
            if stack=='capsid':
                request['externalWorkerExecutionBarrier']=True
            requests.append(request)
        if stack=='capsid':
            wave_start = 100 + (ordinal - 1) * .04
            idle = dict(startedMonotonic=wave_start, deadlineMonotonic=400, admittedMonotonic=wave_start+.001,
                        readings=[dict(startedMonotonic=wave_start, completedMonotonic=wave_start+.001,
                                       httpInflight=0, rawGauge='capsid_channel_http_inflight{scope="service",listener=""} 0')])
            barriers.append(dict(wave=ordinal,workers=workers,rootPid=root_pid,rootPaused=False,
                                 idleReadiness=idle,pauseStartedMonotonic=wave_start+.002,
                                 observedHttpInflightBeforeResume=count,pauseSeconds=.02,
                                 resumeSignalsAttempted=count,resumeSignalsIssued=count,resumeFailures=[]))
            for worker in workers:
                signals += [dict(**worker,signal='SIGSTOP'),dict(**worker,signal='SIGCONT')]
    signals += [dict(pid=p['pid'],start=p['start'],signal='SIGKILL') for p in reversed(ready)]
    cleanup = dict(ownedPidsRemaining=0,processes=copy.deepcopy(ready),signals=signals)
    write(root/directory/'requests.jsonl', b''.join(encoded(r) for r in requests))
    write(root/directory/'stderr.log', ('\n'.join(logs)+'\n').encode())
    write(root/directory/'cleanup.json', encoded(cleanup))
    if stack=='capsid':
        write(root/directory/'runtime/prewarm-barriers.jsonl', b''.join(encoded(r) for r in barriers))
    verdict = dict(ok=True,errors_as_success=False,responses_checked=count*target,
                   errors=0,mismatches=0,timeouts=0,warmupConcurrencyLimit=concurrency,
                   maximumWaveRequests=count,maximumRequestsAllowed=max(64,count*target*(32 if phase=='warm' else 8)),
                   coverageTimeoutSeconds=300,conditioningMethod='owned-worker-dispatch-v1' if stack=='capsid' else 'targeted-cell-socket-v1',
                   conditioningWaves=target,conditioningSeconds=target*.04+.01 if stack=='capsid' else .1,
                   externalWorkerExecutionBarrier=stack=='capsid')
    if stack=='capsid':
        verdict.update(barrierWaves=target,conditioningEvidence='runtime/prewarm-barriers.jsonl')
    row = dict(stack=stack,size=size,phase=phase,count=count,unit='workers' if stack=='capsid' else 'cells',
               status='ok',finalVerification=final,pss_readings_kb=[pss_kb,pss_kb],
               pssSamples=[copy.deepcopy(sample),copy.deepcopy(sample)],readyProcesses=ready,
               requestsPerWorker={worker:target for worker in worker_ids},probeDirectory=directory,
               cleanup=cleanup,correctness=verdict,ports=ports,limit='cap')
    if stack == 'workerd':
        row['readiness'] = dict(method='workerd-control-listen-v1',sockets=count,beforeTraffic=True,deadlineSeconds=120)
        network = dict(method='linux-conntrack-headroom-v1', requiredConnections=count*(target+1),
                       startupTimeoutSeconds=120, startMonotonic=10, deadlineMonotonic=130,
                       readings=1, admittedMonotonic=10.01, launchStartedMonotonic=10.02, readyMonotonic=10.03)
        reading = dict(sequence=1, sampledMonotonic=10.01, maximum=1048576, current=0,
                       requiredConnections=count*(target+1), eligible=True)
        row['startupNetworkReadiness'] = network
        write(root/directory/'network-startup.json', encoded(network))
        write(root/directory/'network-headroom.jsonl', encoded(reading))
    return row, verdict
