import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';

export const branchPrefixPlans = Object.freeze(['tee', 'clone'].flatMap(graph => ['wide', 'full'].flatMap(method =>
    ['continue', 'cancel0', 'cancel1', 'all', 'abort', 'cancel0abort', 'cancel1abort', 'eof'].map(action =>
        Object.freeze({id: `raw-${graph}-${method}-${action}`, kind: 'raw', graph, method, action, api: `${graph}-${method}-${action}`})))));
export function branchPrefixCases(method) {
    assert.ok(['wide', 'full'].includes(method), 'required typed minimum shard');
    return branchPrefixPlans.filter(plan => plan.method === method);
}
const payload = Buffer.alloc(65573);
let seed = 0x12345678n;
for (let index = 0; index < payload.length; ++index) {
    seed = (seed * 1664525n + 1013904223n) & 0xffffffffn;
    payload[index] = Number(seed >> 24n);
}
assert.equal(createHash('sha256').update(payload).digest('hex'), '4614eecedd09854b54f6f9b6dd21d93f29ebd9a9a39a565ddbfa103f05ed7a8a');
const encode = bytes => Buffer.from(bytes).toString('base64');
const canonicalPlan = plan => assert.deepEqual(plan, branchPrefixPlans.find(value => value.id === plan?.id));
export function branchPrefixPacket(plan) {
    canonicalPlan(plan);
    const fields = ['HTTP/1.1 201 Created', 'Connection: close', 'Content-Type: application/octet-stream', `X-Branch-Case: ${plan.id}`];
    if (plan.action !== 'eof') fields.push('Content-Length: 65573');
    return Buffer.concat([Buffer.from(fields.join('\r\n') + '\r\n\r\n'), payload]);
}
export function expectedBranchPrefixState(plan) {
    canonicalPlan(plan);
    const {graph, method, action} = plan, abort = action === 'abort' || action.endsWith('abort');
    const cancelled = new Set(action === 'all' ? [0, 1] : action.startsWith('cancel0') ? [0] : action.startsWith('cancel1') ? [1] : []);
    const headers = [['connection', 'close'], ...(action === 'eof' ? [] : [['content-length', '65573']]),
        ['content-type', 'application/octet-stream'], ['x-branch-case', plan.id]];
    const metadata = (bodyUsed, locked) => ({status: 201, bodyUsed, locked, headers: structuredClone(headers)});
    const tee = graph === 'tee';
    const state = {graph, method, action, failure: null,
        controls: ['prepared', 'prefix', 'observed-prefix', 'called', 'after-action'].map(phase => ({phase, status: 200, text: 'ok'})),
        before: metadata(false, false), afterGraph: metadata(tee, tee), after: metadata(true, tee),
        branches: [], prepared: [], afterPrefix: [], afterAction: [], final: []};
    if (!tee) state.copy = metadata(false, false);
    if (abort) state.aborted = true;
    for (const slot of [0, 1]) {
        const size = slot === 0 ? 65536 : 1024, cancel = cancelled.has(slot);
        const why = {name: 'Error', message: cancel ? `branch ${['zero', 'one'][slot]} stop` : 'branch transport stop',
            sameReason: false, nativePrototype: true};
        const base = label => ({label, inputBefore: size + 32, returnedPromise: true, inputAfterIssue: 0, inputAfter: 0});
        const eof = label => ({...base(label), done: true, value: {type: 'Uint8Array', offset: 0, length: 0,
            capacity: size + 32, bytes: '', backing: encode(Buffer.alloc(size + 32, 165))}});
        const rejected = label => ({...base(label), error: why});
        const branch = {slot, byob: true, firstState: cancel || abort ? 'rejected' : 'fulfilled',
            closedState: abort && !cancel ? 'rejected' : 'fulfilled', cancellationState: cancel ? 'fulfilled' : 'none',
            tails: [], closedStable: true, inputAfterIssue: 0,
            closed: abort && !cancel ? {error: why} : {fulfilled: true}, released: true, lockedAfterRelease: false};
        if (cancel) {branch.cancellation = {fulfilled: true}; branch.first = rejected('first'); branch.tails = [eof('tail0')];}
        else if (abort) {branch.first = rejected('first'); branch.tails = [rejected('tail0')];}
        else {
            const sizes = action === 'eof' ? [137] : slot === 1 ? [...Array(64).fill(1024), 37] :
                method === 'wide' ? [32768, 32768, 37] : [65536, 37];
            let cursor = 0;
            const reads = sizes.map((count, index) => {
                const bytes = payload.subarray(cursor, cursor + count); cursor += count;
                const backing = Buffer.alloc(size + 32, 165); bytes.copy(backing, 8);
                return {...base(index === 0 ? 'first' : `tail${index - 1}`), done: false,
                    value: {type: 'Uint8Array', offset: 8, length: count, capacity: size + 32, bytes: encode(bytes), backing: encode(backing)}};
            });
            assert.equal(cursor, action === 'eof' ? 137 : 65573);
            branch.first = reads[0]; branch.tails = [...reads.slice(1), eof(`tail${reads.length - 1}`)];
        }
        state.branches.push(branch);
        const pending = {slot, firstState: 'pending', closedState: 'pending', cancellationState: 'none', inputAfterIssue: 0, locked: true};
        state.prepared.push(structuredClone(pending)); state.afterPrefix.push(structuredClone(pending));
        if (cancel) Object.assign(pending, {firstState: 'rejected', closedState: 'fulfilled', cancellationState: 'fulfilled'});
        else if (abort) Object.assign(pending, {firstState: 'rejected', closedState: 'rejected'});
        state.afterAction.push(pending);
        state.final.push({slot, firstState: branch.firstState, closedState: branch.closedState,
            cancellationState: branch.cancellationState, inputAfterIssue: 0, locked: false});
    }
    return state;
}

export function expectedBranchPrefixSource(plan, port, earlyFin = false) {
    assert.ok(Number.isSafeInteger(port) && port > 0 && port < 65536, 'actual source port');
    assert.equal(typeof earlyFin, 'boolean', 'preserved cross-socket FIN observation');
    const state = expectedBranchPrefixState(plan), release = ['continue', 'cancel0', 'cancel1', 'eof'].includes(plan.action);
    const packet = branchPrefixPacket(plan), head = packet.subarray(0, packet.indexOf('\r\n\r\n') + 4);
    const request = `GET /body HTTP/1.1\r\nHost: 127.0.0.1:${port}`;
    const sent = [encode(head), encode(payload.subarray(0, 137))];
    if (release) sent.push(plan.action === 'eof' ? '' : encode(payload.subarray(137)));
    const records = [{role: 'source', request, rawRequest: encode(Buffer.from(request + '\r\n\r\n')), sent,
        events: ['accepted', 'request', 'sent-head', 'sent-head', ...(release ? ['sent-end'] : []), 'peer-ended'],
        peerEnded: true, peerReset: false, fixtureEnded: release, fixtureDestroyed: false}];
    const phases = [['prepared', state.prepared], ['prefix', {}], ['observed-prefix', state.afterPrefix],
        ['called', {action: plan.action}], ['after-action', state.afterAction]].map(([phase, data], index) => {
        const query = new URLSearchParams({phase, data: JSON.stringify(data)});
        const request = `GET /control?${query} HTTP/1.1\r\nHost: 127.0.0.1:${port}`;
        records.push({role: 'control', request, rawRequest: encode(Buffer.from(request + '\r\n\r\n')),
            sent: [encode(Buffer.from('HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok'))],
            events: ['accepted', 'request', 'sent-end', 'peer-ended'], peerEnded: true, peerReset: false,
            fixtureEnded: true, fixtureDestroyed: false});
        return {phase, data, sourcePresent: true, bodyReleasedBefore: false,
            sourceEndedBefore: !release && (index === 4 || (index === 3 && earlyFin)), sourceResetBefore: false};
    });
    return {plan, records, phases, bodyReleased: release, prefixReleased: true, failure: null, port};
}

export const branchPrefixInputPaths = Object.freeze({
    runner: 'tests/test_native_fetch_branch_prefix.mjs', binding: 'tests/fixtures/native-fetch-branch-prefix-binding.mjs',
    fixture: 'tests/fixtures/native-fetch-branch-prefix.mjs', common: 'tests/fixtures/native-fetch-branch-prefix-common.mjs',
    user: 'tests/fixtures/native-fetch-branch-prefix-user.mjs', peer: 'tools/native-fetch-branch-prefix-peer.mjs',
    sourceModel: 'tools/native-fetch-branch-prefix-input.mjs', contract: 'tools/native-fetch-branch-prefix-contract.mjs',
    controls: 'tests/test_native_fetch_branch_prefix_contract.mjs', referenceCapture: 'tools/capture-native-fetch-branch-prefix-reference.mjs',
    referenceArchive: 'tests/fixtures/workerd-native-fetch-branch-prefix-report.json.br',
    referenceMetadata: 'tests/fixtures/workerd-native-fetch-branch-prefix-report.json',
    helper: 'tests/test_binding_fetch_retained.cc', exitObserver: 'tests/graceful_worker_exit.h',
});
const root = new URL('../', import.meta.url);
const bytes = name => fs.readFileSync(new URL(branchPrefixInputPaths[name], root));
const digest = name => createHash('sha256').update(bytes(name)).digest('hex');
export function branchPrefixPrimaryIdentities() {
    const common = bytes('common');
    for (const name of ['binding', 'fixture']) assert.ok(bytes(name).subarray(0, common.length).equals(common),
        'verbatim application function in Binding and actual public reference');
    return {fixtureSHA256: digest('fixture'), bindingSHA256: digest('binding'), commonFunctionSHA256: digest('common'),
        sourceModelSHA256: digest('sourceModel'), peerSHA256: digest('peer'), contractSHA256: digest('contract'),
        captureSHA256: digest('referenceCapture')};
}
