"""Independent checks of sealed density conditioning and process evidence."""
from collections import Counter
import hashlib
import json
import math
from pathlib import PurePosixPath
import re
import tarfile
import uuid


class DensityEvidenceError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise DensityEvidenceError('density conditioning: ' + message)


def number(value, label, minimum=0):
    require(type(value) in (int, float) and math.isfinite(value) and value >= minimum,
            label + ' must be a finite number')
    return value


def integer(value, label, minimum=1):
    require(type(value) is int and value >= minimum, label + ' must be an integer')
    return value


def identity(item):
    require(isinstance(item, dict), 'process identity must be an object')
    return integer(item.get('pid'), 'PID'), integer(item.get('start'), 'process start', 0)


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate JSON key: ' + key)
        result[key] = value
    return result


def validate_runtime_snapshot(archive, directory):
    prefix = directory + '/' if directory else ''
    index = archive.json(prefix + 'runtime-nonportable.json')
    require(isinstance(index, dict) and set(index) == {'schema', 'archive', 'members'} and
            type(index['schema']) is int and index['schema'] == 1 and isinstance(index['members'], list),
            'invalid runtime snapshot index')
    records = index['members']
    if not records:
        require(index['archive'] is None, 'empty runtime snapshot must not invent an archive')
        return
    require(index['archive'] == 'runtime-nonportable.tar' and len(records) <= 4096,
            'invalid runtime snapshot container')
    expected = {}
    for record in records:
        require(isinstance(record, dict) and set(record) == {'name', 'bytes', 'sha256'},
                'malformed runtime snapshot member')
        name = record['name']
        require(isinstance(name, str) and name and '\\' not in name and
                not any(ord(c) < 32 for c in name), 'invalid runtime snapshot name')
        path = PurePosixPath(name)
        require(not path.is_absolute() and path.as_posix() == name and
                all(p not in ('', '.', '..') for p in path.parts), 'runtime snapshot path traversal')
        special = [part for part in path.parts if ':' in part]
        require(special and all(re.fullmatch(r'sha256:[0-9a-f]{64}', part) for part in special),
                'runtime snapshot must preserve a known generation name, not a drive or ADS')
        require(name not in expected and integer(record['bytes'], 'runtime member size', 0) <= 256 * 1024**2 and
                isinstance(record['sha256'], str) and re.fullmatch(r'[0-9a-f]{64}', record['sha256']),
                'duplicate or malformed runtime snapshot identity')
        expected[name] = record
    seen = set()
    try:
        with tarfile.open(archive.sealed(prefix + index['archive']), 'r:') as tar:
            for member in tar:
                require(member.isfile() and not member.issym() and not member.islnk() and
                        member.name in expected and member.name not in seen,
                        'runtime snapshot has an unexpected, repeated or non-file member')
                record = expected[member.name]
                require(member.size == record['bytes'], 'runtime snapshot size mismatch')
                digest = hashlib.sha256()
                with tar.extractfile(member) as stream:
                    for block in iter(lambda: stream.read(65536), b''):
                        digest.update(block)
                require(digest.hexdigest() == record['sha256'], 'runtime snapshot byte hash mismatch')
                seen.add(member.name)
    except (tarfile.TarError, OSError) as exc:
        raise DensityEvidenceError('density conditioning: invalid runtime snapshot TAR') from exc
    require(seen == set(expected), 'runtime snapshot omitted an indexed member')


def validate_network_startup(archive, directory, row, count, target):
    network = archive.json(directory + '/network-startup.json')
    require(isinstance(network, dict) and row.get('startupNetworkReadiness') == network,
            'network startup summary disagrees with raw evidence')
    require(network.get('method') == 'linux-conntrack-headroom-v1' and
            integer(network.get('requiredConnections'), 'network required connections') == count * (target + 1),
            'network headroom must cover every original request plus one wave')
    require(number(network.get('startupTimeoutSeconds'), 'network startup timeout') == 120,
            'network readiness cannot increase the original startup budget')
    start = number(network.get('startMonotonic'), 'network startup start')
    deadline = number(network.get('deadlineMonotonic'), 'network startup deadline')
    admitted = number(network.get('admittedMonotonic'), 'network admission')
    launched = number(network.get('launchStartedMonotonic'), 'network process launch')
    ready = number(network.get('readyMonotonic'), 'network service ready')
    require(deadline == start + 120 and start <= admitted <= launched <= ready < deadline,
            'network wait and service startup must share the original absolute deadline')
    readings = archive.jsonl(directory + '/network-headroom.jsonl')
    require(len(readings) == integer(network.get('readings'), 'network reading count'),
            'network readings missing or summary forged')
    previous = start
    for ordinal, reading in enumerate(readings, 1):
        require(isinstance(reading, dict) and set(reading) == {'sequence', 'sampledMonotonic', 'maximum',
                'current', 'requiredConnections', 'eligible'}, 'malformed network reading fields')
        require(integer(reading.get('sequence'), 'network reading sequence') == ordinal,
                'network reading sequence gap')
        sampled = number(reading.get('sampledMonotonic'), 'network reading time')
        require(previous <= sampled <= admitted, 'network reading outside its prelaunch interval')
        previous = sampled
        maximum = integer(reading.get('maximum'), 'network native capacity')
        current = integer(reading.get('current'), 'network current connections', 0)
        required = integer(reading.get('requiredConnections'), 'network planned connections')
        require(required == count * (target + 1) and required <= maximum,
                'network capacity cannot support the unchanged full coverage')
        eligible = maximum - current >= required
        require(reading.get('eligible') is eligible and eligible is (ordinal == len(readings)),
                'network admission must follow actual available capacity, not a fixed hold')
    require(previous == admitted, 'network admission time disagrees with its final raw reading')


def validate_http_idle(barriers, count, verdict):
    """Reparse original service gauges; every pause needs actual prior retirement."""
    first = barriers[0].get('idleReadiness')
    require(isinstance(first, dict), 'HTTP idle evidence is missing')
    deadline = number(first.get('deadlineMonotonic'), 'HTTP idle deadline')
    coverage_start = deadline - verdict['coverageTimeoutSeconds']
    coverage_end = coverage_start + verdict['conditioningSeconds']
    require(0 <= coverage_start < coverage_end <= deadline, 'HTTP idle shared coverage deadline changed')
    previous_resume = coverage_start
    for barrier in barriers:
        idle = barrier.get('idleReadiness')
        require(isinstance(idle, dict) and set(idle) == {'startedMonotonic', 'deadlineMonotonic', 'readings', 'admittedMonotonic'},
                'HTTP idle evidence is incomplete')
        require(number(idle['deadlineMonotonic'], 'HTTP idle deadline') == deadline, 'HTTP idle coverage budget restarted')
        started = number(idle['startedMonotonic'], 'HTTP idle wait start')
        admitted = number(idle['admittedMonotonic'], 'HTTP idle admission')
        pause = number(barrier.get('pauseStartedMonotonic'), 'HTTP idle pause start')
        require(previous_resume <= started <= admitted <= pause < deadline, 'HTTP idle pause preceded actual retirement')
        readings = idle['readings']
        require(isinstance(readings, list) and 0 < len(readings) <= 60001, 'HTTP idle readings are missing or unbounded')
        previous_time, previous_count = started, count
        for index, reading in enumerate(readings):
            require(isinstance(reading, dict), 'HTTP idle reading must be an object')
            read_start = number(reading.get('startedMonotonic'), 'HTTP idle read start')
            read_end = number(reading.get('completedMonotonic'), 'HTTP idle read completion')
            require(previous_time <= read_start <= read_end <= admitted < deadline, 'HTTP idle reading order or deadline changed')
            raw = reading.get('rawGauge')
            match = re.fullmatch(r'capsid_channel_http_inflight\{scope="service",listener=""\} ([0-9]+)', raw) if isinstance(raw, str) else None
            require(match is not None, 'HTTP idle raw service gauge is missing')
            observed = integer(reading.get('httpInflight'), 'HTTP idle inflight', 0)
            require(observed == int(match[1]) and observed <= previous_count, 'HTTP idle raw gauge or previous-wave identity disagrees')
            require(observed == 0 if index == len(readings) - 1 else observed > 0, 'HTTP idle must admit only the first actual zero')
            previous_time, previous_count = read_end, observed
        require(admitted == previous_time, 'HTTP idle admission differs from the final raw reading')
        previous_resume = pause + number(barrier.get('pauseSeconds'), 'HTTP idle pause duration')
        require(previous_resume <= coverage_end, 'HTTP idle wait escaped the unchanged conditioning interval')


def validate_density_conditioning(archive, key, row, verdict, protocol):
    stack, size, phase = key
    count = row['count']
    target = protocol['warmRequests'] if phase == 'warm' else 1
    methods = {'capsid': 'owned-worker-dispatch-v1', 'workerd': 'targeted-cell-socket-v1'}
    require(verdict.get('conditioningMethod') == methods[stack], 'wrong conditioning method')
    require(integer(verdict.get('conditioningWaves'), 'wave count') == target, 'wrong number of waves')
    elapsed = number(verdict.get('conditioningSeconds'), 'conditioning elapsed time')
    require(0 < elapsed <= protocol['coverageTimeoutSeconds'], 'conditioning finished after its deadline')
    require(verdict.get('externalWorkerExecutionBarrier') is (stack == 'capsid'), 'wrong execution barrier kind')
    require(verdict.get('maximumWaveRequests') == count, 'warmup wave must contain one request per instance')
    require(verdict['responses_checked'] == count * target, 'one response per instance and wave is required')
    require(set(row['requestsPerWorker'].values()) == {target}, 'per-instance counters disagree with the waves')
    directory = row.get('probeDirectory')
    require(isinstance(directory, str) and directory not in ('', '.', '..') and
            not any(c in directory for c in '/\\:'), 'invalid probe directory')
    validate_runtime_snapshot(archive, directory)
    launch = archive.json(directory + '/launch.json')
    for name, expected in (('stack', stack), ('size', size), ('phase', phase), ('count', count)):
        require(type(launch.get(name)) is type(expected) and launch[name] == expected, 'launch identity mismatch: ' + name)
    require(launch.get('mode') == ('managed' if stack == 'capsid' else 'workerd'), 'wrong launch mode')
    require(type(launch.get('configuredParallelRequests')) is int and launch['configuredParallelRequests'] == 64,
            'the original 64-client connection limit is required')
    require(launch.get('warmupConcurrencyLimit') == verdict['warmupConcurrencyLimit'], 'launch concurrency mismatch')
    nonce = launch.get('nonce')
    require(isinstance(nonce, str) and re.fullmatch(r'[0-9a-f]{32}', nonce) is not None, 'missing fresh probe nonce')
    ports = launch.get('ports')
    require(isinstance(ports, list) and len(ports) == (1 if stack == 'capsid' else count), 'missing per-cell sockets')
    require(all(type(port) is int and 0 < port < 65536 for port in ports) and len(set(ports)) == len(ports), 'invalid serving ports')
    require(row.get('ports') == ports, 'final probe ports disagree with its launch')
    requests = archive.jsonl(directory + '/requests.jsonl')
    require(len(requests) == count * target, 'raw response count disagrees with conditioning')
    body_sha = hashlib.sha256(b'{"status":"ok","total":2039,"count":16}').hexdigest()
    for position, request in enumerate(requests, 1):
        require(type(request.get('sequence')) is int and request['sequence'] == position, 'response sequence gap')
        require(type(request.get('status')) is int and request['status'] == 200 and request.get('ok') is True,
                'an HTTP error cannot be counted as a response')
        require(request.get('bodySHA256') == body_sha, 'response bytes differ from the frozen fixture')
        require(type(request.get('waveRequests')) is int and request['waveRequests'] == count, 'incorrect raw request wave size')
        require(request.get('warmupConcurrencyLimit') == verdict['warmupConcurrencyLimit'], 'raw concurrency mismatch')
        require(type(request.get('port')) is int and request['port'] == ports[(position - 1) % len(ports)], 'wrong cell socket')
        require(request.get('externalWorkerExecutionBarrier', False) is (stack == 'capsid'), 'raw barrier kind mismatch')
    counts, records, ready, first_ready = {}, 0, 0, None
    control_ready = {}
    expected_control_ports = {f's{i}': port for i, port in enumerate(ports)}
    if stack == 'workerd':
        validate_network_startup(archive, directory, row, count, target)
        require(row.get('readiness') == {'method': 'workerd-control-listen-v1', 'sockets': count,
                                       'beforeTraffic': True, 'deadlineSeconds': 120},
                'missing original-deadline workerd startup readiness')
        command = launch.get('command')
        require(isinstance(command, list) and command.count('--control-fd=1') == 1,
                'workerd must expose its actual startup control events')
    log = archive.sealed(directory + '/stderr.log')
    decoder = json.JSONDecoder(object_pairs_hook=unique_pairs)
    for line in log.read_text(encoding='utf-8').splitlines():
        try:
            outer = json.loads(line, object_pairs_hook=unique_pairs)
        except json.JSONDecodeError:
            outer = None
        if isinstance(outer, dict) and outer.get('event') == 'worker_ready':
            ready += 1
        if stack == 'workerd' and isinstance(outer, dict) and 'event' in outer:
            require(set(outer) == {'event', 'socket', 'port'} and outer['event'] == 'listen',
                    'unexpected workerd startup event')
            name = outer['socket']
            require(isinstance(name, str) and name in expected_control_ports and name not in control_ready and
                    type(outer['port']) is int and outer['port'] == expected_control_ports[name],
                    'repeated or wrong-identity workerd startup event')
            require(records == 0, 'workerd startup event follows business traffic')
            control_ready[name] = outer['port']
        message = outer.get('message', '') if isinstance(outer, dict) else line
        if not isinstance(message, str) or 'CAPSID_BENCH_DENSITY_V1 ' not in message:
            continue
        if first_ready is None:
            first_ready = ready
        if stack == 'workerd':
            require(len(control_ready) == count, 'business traffic preceded complete workerd startup')
        payload = message.split('CAPSID_BENCH_DENSITY_V1 ', 1)[1]
        try:
            value, end = decoder.raw_decode(payload)
        except json.JSONDecodeError as exc:
            raise DensityEvidenceError('density conditioning: malformed business counter') from exc
        require(not payload[end:].strip() and isinstance(value, dict) and
                set(value) == {'nonce', 'worker', 'count'}, 'malformed business counter fields')
        require(value['nonce'] == nonce, 'counter belongs to another probe')
        worker = value['worker']
        try:
            parsed = uuid.UUID(worker) if isinstance(worker, str) else None
        except ValueError:
            parsed = None
        require(parsed is not None and parsed.version == 4 and str(parsed) == worker, 'invalid actual worker UUID')
        require(type(value['count']) is int and value['count'] == counts.get(worker, 0) + 1,
                'missing or repeated business counter')
        counts[worker] = value['count']
        records += 1
        require(len(counts) <= count and records <= count * target, 'extra worker or business execution')
        if records % count == 0:
            require(len(counts) == count and set(counts.values()) == {records // count}, 'wave did not touch every instance once')
    require(records == len(requests) and counts == row['requestsPerWorker'], 'business logs disagree with responses and coverage')
    if stack == 'capsid':
        require(ready == first_ready == count, 'all READY events must precede the first business request')
    else:
        require(len(control_ready) == count, 'missing complete workerd control-FD readiness events')
    ready_processes = row.get('readyProcesses')
    require(isinstance(ready_processes, list) and ready_processes, 'missing pinned readiness membership')
    members = {identity(item) for item in ready_processes}
    require(len(members) == len(ready_processes), 'duplicate readiness process identity')
    require(all(isinstance(item.get('state'), str) and len(item['state']) == 1 and
                item['state'] not in ('Z', 'X') for item in ready_processes), 'missing or dead readiness member')
    if stack == 'capsid':
        require(len(members) == count + 1, 'Host and every worker must be included in PSS')
    samples = row.get('pssSamples')
    require(isinstance(samples, list) and 2 <= len(samples) <= 8, 'missing whole-tree PSS attempts')
    for sample in samples:
        require(isinstance(sample, dict) and isinstance(sample.get('processes'), list), 'missing individual PSS evidence')
        parts = sample['processes']
        require(len(parts) == len(members) and {identity(p) for p in parts} == members, 'PSS omitted or replaced a pinned process')
        total = sum(integer(part.get('pss_kb'), 'process PSS') for part in parts)
        require(integer(sample.get('pss_kb'), 'whole-tree PSS') == total, 'individual PSS sum disagrees with total')
    require([s['pss_kb'] for s in samples[-2:]] == row['pss_readings_kb'], 'final PSS pair disagrees with actual samples')
    cleanup = archive.json(directory + '/cleanup.json')
    require(cleanup == row.get('cleanup') and type(cleanup.get('ownedPidsRemaining')) is int and
            cleanup['ownedPidsRemaining'] == 0, 'complete owned process cleanup is required')
    require(isinstance(cleanup.get('processes'), list) and members <= {identity(p) for p in cleanup['processes']},
            'cleanup evidence lost a measured process identity')
    if stack == 'capsid':
        require(verdict.get('conditioningEvidence') == 'runtime/prewarm-barriers.jsonl', 'missing barrier evidence path')
        require(type(verdict.get('barrierWaves')) is int and verdict['barrierWaves'] == target, 'wrong barrier wave total')
        barriers = archive.jsonl(directory + '/' + verdict['conditioningEvidence'])
        require(len(barriers) == target, 'missing complete barrier wave evidence')
        validate_http_idle(barriers, count, verdict)
        root_pid = integer(barriers[0].get('rootPid'), 'Host PID')
        roots = {p for p in members if p[0] == root_pid}
        require(len(roots) == 1, 'Host is absent from measured readiness membership')
        workers = members - roots
        for ordinal, barrier in enumerate(barriers, 1):
            require(type(barrier.get('wave')) is int and barrier['wave'] == ordinal, 'barrier wave sequence gap')
            require(type(barrier.get('rootPid')) is int and barrier['rootPid'] == root_pid and
                    barrier.get('rootPaused') is False, 'Host must stay running during prewarm')
            require(isinstance(barrier.get('workers'), list) and len(barrier['workers']) == count and
                    {identity(p) for p in barrier['workers']} == workers, 'barrier worker identity changed or omitted')
            for field in ('observedHttpInflightBeforeResume', 'resumeSignalsAttempted', 'resumeSignalsIssued'):
                require(type(barrier.get(field)) is int and barrier[field] == count, 'incomplete barrier dispatch or resume: ' + field)
            require(barrier.get('resumeFailures') == [], 'a failed resume cannot qualify conditioning')
            require(0 < number(barrier.get('pauseSeconds'), 'worker pause') < 5, 'pause exceeded the original socket deadline')
        signals = cleanup.get('signals')
        require(isinstance(signals, list), 'missing actual resume signal records')
        resumes = Counter(identity(s) for s in signals if isinstance(s, dict) and s.get('signal') == 'SIGCONT')
        require(all(resumes[worker] >= target for worker in workers), 'actual signal log does not prove every resume')
