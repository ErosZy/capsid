// Independent raw-wire regression for synchronous handler throws. Goldens
// were observed on both official workerd v1.20261010.1 x64 assets using the
// same fixture, compatibility_date 2026-08-20 and no compatibility flags.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {startCapsid, stopCapsid, withCleanup} from './helpers/ws_host_backend.mjs';
import {responseSession} from './helpers/ws_response_client.mjs';
import {observeHandlerErrors} from './helpers/ws_handler_errors.mjs';
import {assertHandlerErrorDiagnostics} from '../tools/workerd-handler-error-contract.mjs';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    assert.ok(['--host', '--worker', '--mode'].includes(process.argv[index]), 'known argument');
    assert.ok(process.argv[index + 1] && !args.has(process.argv[index]), 'unique complete argument');
    args.set(process.argv[index], process.argv[index + 1]);
}
for (const name of ['--host', '--worker', '--mode']) assert.ok(args.get(name), name);
const host = path.resolve(args.get('--host')), worker = path.resolve(args.get('--worker'));
const mode = args.get('--mode');
assert.ok(['single-worker', 'static-pool', 'managed'].includes(mode));
assert.ok(mode !== 'managed' || process.platform === 'linux', 'managed backend requires Linux');
const fixture = fileURLToPath(new URL('./fixtures/workerd-ws/api-compat.mjs', import.meta.url));
const digest = value => crypto.createHash('sha256').update(value).digest('hex');
assert.equal(digest(fs.readFileSync(fixture)),
    'd60f9081508318ed30d0be2d332c912fbbb7422c8f8f5dec2145c1c94b76f1e0', 'reviewed same-input fixture');
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-throw-wire-'));
const before = {host: digest(fs.readFileSync(host)), worker: digest(fs.readFileSync(worker))};
const backend = await startCapsid(host, worker, fixture, work, mode);
let shutdown, handlerErrorCaseCount;
await withCleanup(async () => {
    const golden = {
        c17: {hex: '8106717565756564', frames: [{opcode: 1, payload: 'cXVldWVk'}],
            listeners: [{id: 'C17-listener', outcome: 'first'}]},
        c18: {hex: '8108612d6265666f7265880603e8646f6e65', frames: [
            {opcode: 1, payload: 'YS1iZWZvcmU='}, {opcode: 8, payload: 'A+hkb25l'},
        ], listeners: []},
    };
    for (const [scenario, autoCloseReply] of [['c17', true], ['c18', true], ['c18', false]])
    for (let repeat = 0; repeat < 4; ++repeat) {
        const peer = responseSession(backend.port, `/@capsid/orders/ws?case=${scenario}`,
            {autoCloseReply, observeCloseReply: true});
        const headers = await peer.headers;
        assert.equal(headers.status, 101, `${scenario}: actual upgrade`);
        const accept = crypto.createHash('sha1')
            .update(headers.requestKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
        assert.equal(/^sec-websocket-accept:\s*(\S+)\s*$/im.exec(headers.rawHeader)?.[1],
            accept, `${scenario}: actual handshake key`);
        peer.send('go');
        const observed = await peer.complete;
        const state = await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/state`,
            {signal: AbortSignal.timeout(4000)});
        assert.equal(state.status, 200, `${scenario}: healthy request after throw`);
        assert.deepEqual(await state.json(), golden[scenario].listeners, `${scenario}: fail-fast callback order`);
        assert.equal(observed.peerTerminated, true, `${scenario}: actual peer terminal`);
        assert.ok(['peer-ended', 'peer-reset'].includes(observed.completion), `${scenario}: no local disposal or deadline`);
        assert.equal(observed.transportError, observed.completion === 'peer-reset' ? 'ECONNRESET' : null);
        assert.equal(Buffer.from(observed.rawTail, 'base64').toString('hex'), golden[scenario].hex,
            `${scenario}: exact accepted sends and Close bytes before transport end`);
        assert.deepEqual(observed.frames, golden[scenario].frames, `${scenario}: exact complete frame list`);
        assert.equal(observed.closeReplySent, scenario === 'c18' && autoCloseReply,
            `${scenario}: actual peer Close reply policy`);
        console.log(JSON.stringify({scenario, repeat, autoCloseReply, mode, passed: true,
            wireHex: golden[scenario].hex, completion: observed.completion}));
    }
    const handlerErrors = await observeHandlerErrors(backend.port, '/@capsid/orders', assert, {includeReferenceBoundary: true});
    handlerErrorCaseCount = Object.keys(handlerErrors).length;
    assert.equal(Object.keys(handlerErrors).length, 162, 'all independently pinned handler-error cases executed');
    assertHandlerErrorDiagnostics(handlerErrors, backend.stderr());
}, async () => {
    shutdown = await stopCapsid(backend.child);
    const stderr = backend.stderr();
    if (stderr) process.stderr.write(stderr);
    assert.deepEqual(shutdown, {code: 0, signal: null, forced: false}, 'bounded cooperative Host stop');
    assert.doesNotMatch(stderr, /ERROR: AddressSanitizer|WARNING: ThreadSanitizer/);
});
assert.equal(digest(fs.readFileSync(host)), before.host);
assert.equal(digest(fs.readFileSync(worker)), before.worker);
console.log(JSON.stringify({complete: true, mode, originalWireCases: 12, handlerErrorCases: handlerErrorCaseCount,
    hostSHA256: before.host, workerSHA256: before.worker, shutdown}));
