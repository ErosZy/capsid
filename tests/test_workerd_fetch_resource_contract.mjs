import {referenceBackendModes} from '../tools/workerd-platform.mjs';
import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import net from 'node:net';
import {listenLoopbackTcp} from './helpers/ws_host_backend.mjs';
import {fetchResourceCases, assertFetchResourceCase, validateFetchResourceReport, compareFetchResourceReports} from '../tools/workerd-fetch-resource-contract.mjs';

// Actual pinned Linux workerd output, replayed only to test this validator.
// Qualification always launches fresh native assets with current input hashes.
const stored = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-fetch-resource-report.json', import.meta.url), 'utf8'));
function matrix() {
    const reference = structuredClone(stored), expected = {runId: reference.runId, lock: reference.referenceLock,
        inputs: reference.inputs, host: 'a'.repeat(64), worker: 'b'.repeat(64)};
    const candidates = referenceBackendModes(expected.lock).map(mode => ({...structuredClone(reference), mode,
        binaries: {host: expected.host, worker: expected.worker}, cases: reference.cases.map(value => ({...structuredClone(value),
            shutdown: {code: 0, signal: null, forced: false}}))}));
    return {reference, candidates, expected};
}
function changeState(value, mutate, key = 'observed') {
    const document = JSON.parse(value[key].text); mutate(document); value[key].text = JSON.stringify(document);
}
function changeRequest(entry, mutate) {
    entry.request = mutate(entry.request);
    const raw = Buffer.from(entry.request + '\r\n\r\n');
    entry.rawRequest = raw.toString('base64'); entry.bytes = raw.length;
}
test('literal fetch resource inventory covers cancellation stages, resource lifetime and reason snapshots', () => {
    assert.deepEqual(fetchResourceCases, [
        'fetch-complete', 'fetch-close-pending', 'fetch-close-body', 'fetch-cancel-reader',
        'fetch-abort-pre-default', 'fetch-abort-pre-string', 'fetch-abort-pre-error', 'fetch-abort-pre-dom',
        'fetch-abort-pre-null', 'fetch-abort-pre-number', 'fetch-abort-pre-object',
        'fetch-abort-live-default', 'fetch-abort-live-string', 'fetch-abort-live-error', 'fetch-abort-live-dom',
        'fetch-abort-live-null', 'fetch-abort-live-number', 'fetch-abort-live-object',
        'fetch-abort-body-default', 'fetch-abort-body-string', 'fetch-abort-body-error', 'fetch-abort-body-dom',
        'fetch-abort-body-null', 'fetch-abort-body-number', 'fetch-abort-body-object',
        'fetch-two-owners', 'fetch-abort-live-mutation', 'fetch-abort-live-shared', 'fetch-disabled-cookie-cache',
        'fetch-sync-context', 'fetch-sync-context-numeric',
        'fetch-custom-headers',
    ]);
    assert.ok(Object.isFrozen(fetchResourceCases));
});
test('actual reference observations satisfy independent absolute resource contracts', () => {
    const {reference, candidates, expected} = matrix();
    assert.deepEqual(compareFetchResourceReports(reference, candidates, expected), {complete: true, casesPerBackend: 32, backends: 1 + candidates.length});
    assert.equal(validateFetchResourceReport(reference, expected).length, 32);
});
for (const id of fetchResourceCases) for (const [name, mutate] of [
    ['lost final callback', value => changeState(value, document => document.state.trace.pop())],
    ['extra callback after terminal', value => changeState(value, document => document.state.trace.push('late'), 'finalState')],
    ['wrong realm', value => changeState(value, document => document.realm = '00000000-0000-4000-8000-000000000001')],
    ['wrong challenge', value => {value.challenge = '00000000-0000-4000-8000-000000000002';}],
    ['truncated bytes', value => {value.wirea.rawTail = '';}],
    ['missing frame', value => {value.wirea.frames.splice(0, 1);}],
    ['local disposal', value => {value.wirea.completion = 'client-disposed';}],
    ['no real Close ACK', value => {value.wirea.closeReplySent = false;}],
    ['wrong Upgrade', value => {value.upgradea.requestKey = 'A'.repeat(22) + '==';}],
    ['missing upstream proof', value => {value.upstream.length ? value.upstream.pop() : value.upstream.push({});}],
    ['forced shutdown', value => {value.shutdown.forced = true;}],
    ['sanitizer failure', value => {value.stderr += '\nWARNING: ThreadSanitizer: data race';}],
]) test(id + ' rejects ' + name, () => {
    const value = structuredClone(stored.cases.find(entry => entry.id === id)); mutate(value);
    assert.throws(() => assertFetchResourceCase(value, 'reference'));
});
for (const id of fetchResourceCases) test(id + ' rejects equally wrong observations on every backend', () => {
    const {reference, candidates, expected} = matrix();
    for (const report of [reference, ...candidates]) changeState(report.cases.find(value => value.id === id), document => document.state.trace.push('shared-wrong-result'));
    assert.throws(() => compareFetchResourceReports(reference, candidates, expected), /complete same-realm state/);
});
for (const id of fetchResourceCases.filter(value => !value.startsWith('fetch-abort-pre-'))) for (const [name, mutate] of [
    ['socket left open', entry => {entry.peerEnded = false;}],
    ['fixture disposal', entry => {entry.fixtureDestroyed = true;}],
    ['invented EOF without event', entry => {entry.events.pop();}],
    ['another raw request', entry => {entry.rawRequest = Buffer.from('GET /other HTTP/1.1\r\n\r\n').toString('base64');}],
    ['incorrect request byte count', entry => {++entry.bytes;}],
]) test(id + ' rejects upstream ' + name, () => {
    const value = structuredClone(stored.cases.find(entry => entry.id === id)); mutate(value.upstream[0]);
    assert.throws(() => assertFetchResourceCase(value, 'reference'));
});
for (const id of fetchResourceCases.filter(value => value.startsWith('fetch-abort-'))) for (const field of ['sameReason', 'constructor', 'message', 'type'])
    test(id + ' rejects incorrect failure ' + field, () => {
        const value = structuredClone(stored.cases.find(entry => entry.id === id));
        changeState(value, document => {document.state.failure[field] = field === 'sameReason' ? !document.state.failure[field] : 'wrong';});
        assert.throws(() => assertFetchResourceCase(value, 'reference'), /complete same-realm state/);
    });
// Keep the raw bytes and byte count consistent so these controls exercise the
// independent HTTP semantics, rather than merely corrupting the archive.
for (const id of ['fetch-complete', 'fetch-sync-context-numeric', 'fetch-custom-headers']) {
    for (const [name, mutate] of [
        ['missing authority port', entry => changeRequest(entry, request => request.replace(/(\r\nhost: [^\r\n]+):\d+/i, '$1'))],
        ['duplicate Host', entry => changeRequest(entry, request => request + '\r\n' + request.split('\r\n').find(line => /^host:/i.test(line)))],
        ['another accepted port', entry => {entry.localPort = entry.localPort === 65535 ? 65534 : entry.localPort + 1;}],
        ['another listening address', entry => {entry.localAddress = '127.0.0.2';}],
    ]) test(id + ' rejects consistent request with ' + name, () => {
        const value = structuredClone(stored.cases.find(entry => entry.id === id)); mutate(value.upstream[0]);
        assert.throws(() => assertFetchResourceCase(value, 'reference'), /request headers|accepted socket port|listening address/);
    });
    test(id + ' rejects a different declared target port', () => {
        const value = structuredClone(stored.cases.find(entry => entry.id === id));
        value.targets[0].port = value.targets[0].port === 65535 ? 65534 : value.targets[0].port + 1;
        assert.throws(() => assertFetchResourceCase(value, 'reference'), /accepted socket port/);
    });
}
for (const header of ['User-Agent: txiki.js', 'Accept-Encoding: gzip, deflate', 'Origin: http://localhost',
    'Cache-Control: no-cache', 'Pragma: no-cache']) {
    test('default fetch rejects unsolicited ' + header.split(':')[0], () => {
        const value = structuredClone(stored.cases.find(entry => entry.id === 'fetch-complete'));
        changeRequest(value.upstream[0], request => request + '\r\n' + header);
        assert.throws(() => assertFetchResourceCase(value, 'reference'), /exact independent request headers/);
    });
    test('reports reject equally unsolicited ' + header.split(':')[0] + ' on every backend', () => {
        const {reference, candidates, expected} = matrix();
        for (const report of [reference, ...candidates]) {
            changeRequest(report.cases.find(entry => entry.id === 'fetch-complete').upstream[0], request => request + '\r\n' + header);
        }
        assert.throws(() => compareFetchResourceReports(reference, candidates, expected), /exact independent request headers/);
    });
}
for (const header of ['user-agent', 'accept-encoding', 'origin', 'cache-control', 'pragma', 'x-capsid-header'])
    test('custom fetch rejects a lost caller ' + header, () => {
        const value = structuredClone(stored.cases.find(entry => entry.id === 'fetch-custom-headers'));
        changeRequest(value.upstream[0], request => request.split('\r\n').filter(line => !line.toLowerCase().startsWith(header + ':')).join('\r\n'));
        assert.throws(() => assertFetchResourceCase(value, 'reference'), /exact independent request headers/);
    });
for (const id of ['fetch-sync-context', 'fetch-sync-context-numeric']) {
    for (const stage of ['before', 'after', 'callback']) for (const role of ['a', 'b'])
        test(id + ' rejects wrong ' + stage + ' ownership for ' + role, () => {
            const value = structuredClone(stored.cases.find(entry => entry.id === id));
            changeState(value, document => {
                const index = document.state.trace.findIndex(entry => entry.startsWith(stage + '/' + role + '/'));
                document.state.trace[index] = stage + '/' + role + (role === 'a' ? '/lost-owner' : '/ok');
            });
            assert.throws(() => assertFetchResourceCase(value, 'reference'), /complete same-realm state/);
        });
    test(id + ' rejects passive owner completion before its own action', () => {
        const value = structuredClone(stored.cases.find(entry => entry.id === id));
        changeState(value, document => document.state.other.trace.push('close'), 'checkpoint');
        assert.throws(() => assertFetchResourceCase(value, 'reference'), /complete same-realm state/);
    });
}
for (const [name, mutate] of [
    ['missing guard', value => {delete value.filesystemGuard;}],
    ['changed guard identity', value => {value.filesystemGuard.after.ino += '1';}],
]) test('disabled cookie cache rejects ' + name, () => {
    const value = structuredClone(stored.cases.find(entry => entry.id === 'fetch-disabled-cookie-cache')); mutate(value);
    assert.throws(() => assertFetchResourceCase(value, 'reference'));
});
for (const [name, mutate] of [
    ['foreign request cancelled', value => {value.checkpointUpstream[1].peerEnded = true;}],
    ['owner request retained', value => {value.checkpointUpstream[0].peerEnded = false;}],
    ['foreign callback ran after cancellation', value => changeState(value, document => document.state.other.trace.push('close'), 'checkpoint')],
]) test('two-owner control rejects ' + name, () => {
    const value = structuredClone(stored.cases.find(entry => entry.id === 'fetch-two-owners')); mutate(value);
    assert.throws(() => assertFetchResourceCase(value, 'reference'));
});
for (const field of ['sameErrors', 'sameReason0', 'sameReason1', 'sameNested', 'sharedAlias0', 'sharedAlias1', 'firstNested', 'secondNested'])
    test('shared cancellation rejects altered clone field ' + field, () => {
        const value = structuredClone(stored.cases.find(entry => entry.id === 'fetch-abort-live-shared'));
        changeState(value, document => {const prior = document.state.clones[field]; document.state.clones[field] = typeof prior === 'boolean' ? !prior : 'changed';});
        assert.throws(() => assertFetchResourceCase(value, 'reference'));
    });
for (const [name, mutate] of [
    ['missing case', reports => reports.forEach(report => report.cases.pop())],
    ['duplicate case', reports => reports.forEach(report => report.cases[1] = structuredClone(report.cases[0]))],
    ['stale run', reports => {reports[1].runId = 'old';}],
    ['different fixture', reports => {reports[1].inputs.fixture = 'c'.repeat(64);}],
    ['different contract', reports => {reports[1].inputs.contract = 'c'.repeat(64);}],
    ['different profile', reports => {reports[1].referenceLock.compatibility_date = '2026-01-01';}],
    ['different client', reports => {reports[1].client.node = 'v0.0.0';}],
    ['different worker', reports => {reports[1].binaries.worker = 'c'.repeat(64);}],
    ['missing backend', reports => {reports.pop();}],
    ['replayed realm', reports => {changeState(reports[1].cases[1], document => {document.realm = JSON.parse(reports[1].cases[0].initial.text).realm;}, 'initial');}],
    ['replayed challenge', reports => {reports[1].cases[1].challenge = reports[1].cases[0].challenge;}],
    ['partial collection', reports => {reports[1].complete = false;}],
]) test('resource reports reject ' + name, () => {
    const {reference, candidates, expected} = matrix(), reports = [reference, ...candidates]; mutate(reports);
    assert.throws(() => compareFetchResourceReports(reports[0], reports.slice(1), expected));
});
for (const args of [['--kinds', 'fetch-complete'], ['--mode'], ['--mode', 'single-worker', '--mode', 'static-pool'],
    ['--reference-from-env', '0'], ['--reference-from-env', '1', '--host', 'product']])
    test('driver rejects inventory filtering or malformed options ' + args.join(' '), () => {
        const result = spawnSync(process.execPath, [fileURLToPath(new URL('./test_websocket_fetch_resources.mjs', import.meta.url)), ...args],
            {encoding: 'utf8', timeout: 5000, env: {...process.env, CAPSID_WORKERD_REF: 'unopened-reference-asset'}});
        assert.notEqual(result.status, 0); assert.notEqual(result.error?.code, 'ETIMEDOUT');
        assert.doesNotMatch(result.stderr, /MODULE_NOT_FOUND|Cannot find module|ENOENT|CAPSID_WORKERD_REF required/);
        assert.match(result.stderr, /AssertionError/);
    });

test('Linux validation rejects omission of managed from a verified healthy backend matrix', () => {
    const m=matrix();assert.ok(referenceBackendModes(m.expected.lock).includes('managed'));
    assert.doesNotThrow(()=>compareFetchResourceReports(m.reference,m.candidates,m.expected));
    assert.throws(()=>compareFetchResourceReports(m.reference,m.candidates.filter(r=>r.mode!=='managed'),m.expected));
});

for (const id of fetchResourceCases.filter(id=>!id.startsWith('fetch-abort-pre-')&&id!=='fetch-sync-context-numeric'))
    test(id+' accepts actual owned IPv6 loopback observations without changing HTTP semantics',()=>{
        const value=structuredClone(stored.cases.find(entry=>entry.id===id));
        for(const key of ['upstream','checkpointUpstream','beforeAction'])
            for(const entry of value[key]??[])entry.localAddress='::1';
        assert.doesNotThrow(()=>assertFetchResourceCase(value,'reference'));
    });
for(const address of ['::2','::ffff:127.0.0.1','fe80::1'])
    test('resource model rejects unowned IPv6 address '+address,()=>{
        const value=structuredClone(stored.cases.find(entry=>entry.id==='fetch-complete'));
        value.upstream[0].localAddress=address;
        assert.throws(()=>assertFetchResourceCase(value,'reference'),/actual listening address/);
    });
test('numeric IPv4 target cannot claim an IPv6 accepted socket',()=>{
    const value=structuredClone(stored.cases.find(entry=>entry.id==='fetch-sync-context-numeric'));
    value.upstream[0].localAddress='::1';
    assert.throws(()=>assertFetchResourceCase(value,'reference'),/actual listening address/);
});

function listenForTest(server,options) {
    return new Promise((resolve,reject)=>{
        const failed=error=>{server.removeListener('listening',ready);reject(error);};
        const ready=()=>{server.removeListener('error',failed);resolve();};
        server.once('error',failed);server.once('listening',ready);server.listen(options);
    });
}
const closeForTest=server=>server.listening?new Promise((resolve,reject)=>
    server.close(error=>error?reject(error):resolve())):Promise.resolve();

test('loopback fixture owns both exact addresses on one port and preserves actual socket families',
    {timeout:3000},async()=>{
        const accepted=[];
        const listener=await listenLoopbackTcp(socket=>{
            accepted.push(socket.localAddress);
            socket.on('data',data=>{assert.equal(data.toString(),'probe');socket.end('owned/'+socket.localAddress);});
        });
        try {
            assert.deepEqual(listener.addresses.map(v=>[v.address,v.port]),
                [['127.0.0.1',listener.port],['::1',listener.port]]);
            for(const host of ['127.0.0.1','::1']) {
                const text=await new Promise((resolve,reject)=>{
                    const chunks=[],socket=net.createConnection({host,port:listener.port},()=>socket.end('probe'));
                    socket.on('data',data=>chunks.push(data));socket.once('error',reject);
                    socket.once('close',()=>resolve(Buffer.concat(chunks).toString()));
                });
                assert.equal(text,'owned/'+host);
            }
            assert.deepEqual(accepted,['127.0.0.1','::1']);
        } finally {await listener.close();}
        assert.equal(listener.close(),listener.close(),'listener teardown is idempotent');
    });
test('loopback fixture prevents another application owning its IPv6 authority',
    {timeout:3000},async()=>{
        const listener=await listenLoopbackTcp(socket=>socket.destroy());
        const foreign=net.createServer(socket=>socket.destroy());
        try {
            await assert.rejects(listenForTest(foreign,
                {host:'::1',port:listener.port,ipv6Only:true,exclusive:true}),{code:'EADDRINUSE'});
        } finally {await closeForTest(foreign);await listener.close();}
    });
test('IPv4-only control leaves the same authority available to another IPv6 application',
    {timeout:3000},async()=>{
        const ipv4=net.createServer(socket=>socket.destroy()),foreign=net.createServer(socket=>socket.destroy());
        try {
            await listenForTest(ipv4,{host:'127.0.0.1',port:0,exclusive:true});
            await listenForTest(foreign,{host:'::1',port:ipv4.address().port,ipv6Only:true,exclusive:true});
            assert.equal(ipv4.address().port,foreign.address().port);
            assert.equal(foreign.listening,true,'the old IPv4 reservation cannot exclude a foreign IPv6 listener');
        } finally {await closeForTest(foreign);await closeForTest(ipv4);}
    });
test('a partial dual-loopback bind failure releases only its own IPv4 listener',
    {timeout:3000},async()=>{
        const foreign=net.createServer(socket=>socket.destroy()),rebound=net.createServer(socket=>socket.destroy());
        try {
            await listenForTest(foreign,{host:'::1',port:0,ipv6Only:true,exclusive:true});
            const port=foreign.address().port;
            await assert.rejects(listenLoopbackTcp(socket=>socket.destroy(),{port}),{code:'EADDRINUSE'});
            assert.equal(foreign.listening,true,'cleanup cannot close another owner');
            await listenForTest(rebound,{host:'127.0.0.1',port,exclusive:true});
            assert.equal(rebound.address().port,port,'the failed attempt left no IPv4 reservation');
        } finally {await closeForTest(rebound);await closeForTest(foreign);}
    });
