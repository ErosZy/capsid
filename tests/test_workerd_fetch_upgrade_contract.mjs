import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {fetchUpgradeCases, assertFetchUpgradeCase, validateFetchUpgradeReport, compareFetchUpgradeReports} from '../tools/workerd-fetch-upgrade-contract.mjs';

// Exact fresh pinned Windows workerd output. Replay and synthetic candidates
// below exercise validators only; product qualification runs fresh binaries.
const stored = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-fetch-upgrade-report.json', import.meta.url), 'utf8'));
const knownReferenceError = {type: 'error', state: 3, message: 'Uncaught Error: Network connection lost.',
    error: 'Error: Network connection lost.', errorType: 'object', errorIsError: true, errorName: 'Error', errorMessage: 'Network connection lost.'};
const successfulSockets = ['upgrade-accept', 'upgrade-custom-headers', 'upgrade-large-message', 'upgrade-server-protocol', 'upgrade-post', 'upgrade-redirect-follow'];
function changeState(value, mutate) {
    const document = JSON.parse(value.observed.text); mutate(document.state, document);
    value.observed.text = value.atClose.text = JSON.stringify(document);
}
function changeRequest(entry, mutate) {
    entry.request = mutate(entry.request); entry.rawRequest = Buffer.from(entry.request + '\r\n\r\n', 'latin1').toString('base64');
}
function sample(kind) {return structuredClone(stored.cases.find(value => value.kind === kind));}
function matrix(platform = 'win32') {
    const reference = structuredClone(stored);
    const lock = JSON.parse(fs.readFileSync(new URL(platform === 'linux' ? './fixtures/workerd-ws/reference-lock.json'
        : './fixtures/workerd-ws/reference-lock-windows.json', import.meta.url), 'utf8'));
    reference.referenceLock = lock; reference.binaries.host = lock.binary_sha256;
    const expected = {runId: reference.runId, lock, inputs: reference.inputs, host: 'a'.repeat(64), worker: 'b'.repeat(64)};
    const candidates = ['single-worker', 'static-pool', ...(platform === 'linux' ? ['managed'] : [])].map(mode => {
        const report = structuredClone(reference); report.mode = mode; report.binaries = {host: expected.host, worker: expected.worker};
        for (const value of report.cases) {
            value.shutdown = {code: 0, signal: null, forced: false};
            // Synthetic healthy product traces never inherit the reference's
            // precisely documented extra close-race notification.
            changeState(value, state => {if (state.socket) state.socket.events = state.socket.events.filter(event => event.type !== 'error');});
        }
        return report;
    });
    return {reference, candidates, expected};
}

test('literal upgrade inventory includes handshake validation, body streaming, cancellation and redirects', () => {
    assert.deepEqual(fetchUpgradeCases, ['upgrade-accept', 'upgrade-custom-headers', 'upgrade-large-message',
        'upgrade-non101', 'upgrade-non101-stream', 'upgrade-invalid-accept', 'upgrade-server-protocol', 'upgrade-server-extension',
        'upgrade-post', 'upgrade-abort-pre', 'upgrade-abort-pending', 'upgrade-redirect-manual', 'upgrade-redirect-follow']);
    assert.ok(Object.isFrozen(fetchUpgradeCases));
});
for (const platform of ['win32', 'linux']) test(platform + ' actual reference data satisfy absolute upgrade contracts', () => {
    const {reference, candidates, expected} = matrix(platform);
    assert.equal(validateFetchUpgradeReport(reference, expected).length, 13);
    assert.deepEqual(compareFetchUpgradeReports(reference, candidates, expected), {complete: true, casesPerBackend: 13, backends: platform === 'linux' ? 3 : 2});
});
for (const kind of fetchUpgradeCases) for (const [name, mutate] of [
    ['lost terminal callback', value => changeState(value, state => state.trace.pop())],
    ['late callback', value => {const document = JSON.parse(value.atClose.text); document.state.trace.push('late'); value.atClose.text = JSON.stringify(document);}],
    ['wrong owner realm', value => changeState(value, (_state, document) => {document.realm = '00000000-0000-4000-8000-000000000001';})],
    ['wrong challenge', value => {value.challenge = '00000000-0000-4000-8000-000000000001';}],
    ['missing client close reply', value => {value.wire.closeReplySent = false;}],
    ['missing client EOF', value => {value.wire.peerTerminated = false;}],
    ['missing raw close', value => {value.wire.frames.pop();}],
    ['forced shutdown', value => {value.shutdown.forced = true;}],
    ['unexpected backend', value => assertFetchUpgradeCase(value, 'alias')],
]) test(kind + ' rejects ' + name, () => {
    const value = sample(kind); assert.throws(() => {mutate(value); assertFetchUpgradeCase(value, 'reference');});
});
for (const kind of fetchUpgradeCases.filter(value => value !== 'upgrade-abort-pre')) for (const [name, mutate] of [
    ['lost Connection header', entry => changeRequest(entry, value => value.split('\r\n').filter(line => !/^connection:/i.test(line)).join('\r\n'))],
    ['wrong version', entry => changeRequest(entry, value => value.replace('Sec-WebSocket-Version: 13', 'Sec-WebSocket-Version: 12'))],
    ['missing Host port', entry => changeRequest(entry, value => value.replace(/(Host: localhost):\d+/, '$1'))],
    ['duplicate Host', entry => changeRequest(entry, value => value + '\r\n' + value.split('\r\n').find(line => /^host:/i.test(line)))],
    ['different accepted port', entry => {entry.localPort = entry.localPort === 65535 ? 65534 : entry.localPort + 1;}],
    ['different accepted address', entry => {entry.localAddress = '127.0.0.2';}],
    ['fixture disposed before real EOF', entry => {entry.fixtureDestroyed = true;}],
    ['missing peer EOF', entry => {entry.peerEnded = false;}],
    ['missing upstream event', entry => {entry.events.pop();}],
    ['wrong actual status', entry => {entry.responseStatus = 400;}],
    ['unsent HTTP response', entry => {entry.fixtureEnded = !entry.fixtureEnded;}],
]) test(kind + ' rejects consistent upstream ' + name, () => {
    const value = sample(kind); mutate(value.upstream.at(-1)); assert.throws(() => assertFetchUpgradeCase(value, 'reference'));
});
for (const kind of successfulSockets) for (const [name, mutate] of [
    ['wrong data digest', value => changeState(value, state => {state.socket.messages[1].sha256 = '0'.repeat(64);})],
    ['early send accepted', value => changeState(value, state => {state.socket.beforeSend = {returned: 'undefined'};})],
    ['constructor protocol leaked', value => changeState(value, state => {state.socket.before.protocol = 'chat';})],
    ['wrong close status', value => changeState(value, state => {state.socket.closed.code = 1006;})],
    ['unmasked upstream data', value => {value.upstream.at(-1).frames[0].masked = false;}],
    ['missing actual upstream Close', value => {value.upstream.at(-1).frames.pop();}],
    ['wrong upstream Close body', value => {value.upstream.at(-1).frames.at(-1).payload = 'A+h3cm9uZw==';}],
    ['extra upstream frame after Close', value => {value.upstream.at(-1).frames.push(structuredClone(value.upstream.at(-1).frames.at(-1)));}],
    ['altered actual upstream payload', value => {value.upstream.at(-1).frames[0].payload = Buffer.from('wrong').toString('base64');}],
]) test(kind + ' rejects ' + name, () => {
    const value = sample(kind); mutate(value); assert.throws(() => assertFetchUpgradeCase(value, 'reference'));
});
for (const kind of successfulSockets) {
    test(kind + ' accepts only the exact documented reference close race', () => {
        const value = sample(kind); changeState(value, state => {state.socket.events = [knownReferenceError, state.socket.events.at(-1)];});
        assert.doesNotThrow(() => assertFetchUpgradeCase(value, 'reference'));
        value.shutdown = {code: 0, signal: null, forced: false};
        assert.throws(() => assertFetchUpgradeCase(value, 'single-worker'), /actual accepted socket data and close/);
    });
    for (const field of ['state', 'message', 'error', 'errorType', 'errorIsError', 'errorName', 'errorMessage'])
        test(kind + ' rejects changed reference race field ' + field, () => {
            const value = sample(kind); const error = {...knownReferenceError, [field]: 'wrong'};
            changeState(value, state => {state.socket.events = [error, state.socket.events.at(-1)];});
            assert.throws(() => assertFetchUpgradeCase(value, 'reference'), /actual accepted socket data and close/);
        });
}
for (const kind of ['upgrade-non101', 'upgrade-non101-stream', 'upgrade-redirect-manual']) for (const field of ['status', 'url', 'redirected', 'bodyNull', 'bodyUsed', 'cookies'])
    test(kind + ' rejects HTTP result ' + field, () => {
        const value = sample(kind); changeState(value, state => {state.result[field] = 'wrong';});
        assert.throws(() => assertFetchUpgradeCase(value, 'reference'), /complete independent HTTP response/);
    });
for (const kind of ['upgrade-invalid-accept', 'upgrade-server-extension', 'upgrade-abort-pre', 'upgrade-abort-pending']) for (const field of ['name', 'message', 'sameReason'])
    test(kind + ' rejects failure ' + field, () => {
        const value = sample(kind); changeState(value, state => {state.failure[field] = field === 'sameReason' ? !state.failure[field] : 'wrong';});
        assert.throws(() => assertFetchUpgradeCase(value, 'reference'));
    });
test('stream body must be readable while the real peer still holds the remainder', () => {
    for (const field of ['peerEnded', 'fixtureEnded']) {
        const value = sample('upgrade-non101-stream'); value.checkpointUpstream[0][field] = true;
        assert.throws(() => assertFetchUpgradeCase(value, 'reference'));
    }
    const value = sample('upgrade-non101-stream');
    const document = JSON.parse(value.checkpoint.text); document.state.firstBody.bytes = 180000; value.checkpoint.text = JSON.stringify(document);
    assert.throws(() => assertFetchUpgradeCase(value, 'reference'));
});
test('redirect cannot reuse its prior handshake nonce, even with consistent raw request bytes', () => {
    const value = sample('upgrade-redirect-follow');
    const key = value.upstream[0].request.match(/Sec-WebSocket-Key: ([^\r]+)/)[1];
    changeRequest(value.upstream[1], request => request.replace(/Sec-WebSocket-Key: [^\r]+/, 'Sec-WebSocket-Key: ' + key));
    assert.throws(() => assertFetchUpgradeCase(value, 'reference'), /fresh nonce/);
});
for (const [name, mutate] of [
    ['missing case everywhere', reports => reports.forEach(report => report.cases.pop())],
    ['wrong response everywhere', reports => reports.forEach(report => changeState(report.cases[0], state => {state.result.status = 400;}))],
    ['wrong upstream headers everywhere', reports => reports.forEach(report => changeRequest(report.cases[0].upstream[0], request => request + '\r\nCache-Control: no-cache'))],
    ['missing candidate', reports => reports.pop()],
    ['duplicate backend', reports => {reports[2].mode = reports[1].mode;}],
    ['stale run', reports => {reports[1].runId = '00000000-0000-4000-8000-000000000001';}],
    ['missing run identity', reports => {reports[1].runId = '';}],
    ['different worker', reports => {reports[1].binaries.worker = 'c'.repeat(64);}],
    ['different reference asset', reports => {reports[0].binaries.host = 'c'.repeat(64);}],
    ['different input', reports => {reports[1].inputs.contract = 'c'.repeat(64);}],
    ['different profile', reports => {reports[1].referenceLock.compatibility_date = '2026-01-01';}],
    ['wrong client', reports => {reports[1].client.node = 'v0.0.0';}],
    ['partial collection', reports => {reports[1].complete = false;}],
    ['replayed challenge', reports => {reports[1].cases[1].challenge = reports[1].cases[0].challenge;}],
]) test('upgrade reports reject ' + name, () => {
    const {reference, candidates, expected} = matrix(), reports = [reference, ...candidates]; mutate(reports);
    assert.throws(() => compareFetchUpgradeReports(reports[0], reports.slice(1), expected));
});
for (const args of [['--kinds', 'upgrade-accept'], ['--mode'], ['--mode', 'single-worker', '--mode', 'static-pool'],
    ['--reference-from-env', '0'], ['--reference-from-env', '1', '--host', 'product']])
    test('driver refuses filtering or malformed selectors ' + args.join(' '), () => {
        const result = spawnSync(process.execPath, [fileURLToPath(new URL('./test_websocket_fetch_upgrade.mjs', import.meta.url)), ...args],
            {encoding: 'utf8', timeout: 5000, env: {...process.env, CAPSID_WORKERD_REF: 'unopened-reference-asset'}});
        assert.notEqual(result.status, 0); assert.notEqual(result.error?.code, 'ETIMEDOUT');
        assert.match(result.stderr, /AssertionError/); assert.doesNotMatch(result.stderr, /MODULE_NOT_FOUND|Cannot find module|ENOENT/);
    });
