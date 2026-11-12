import assert from 'node:assert/strict';

export const bindingQuotaVariants = Object.freeze(['plain', 'symbol', 'cycle', 'getter']);
export const bindingQuotaInputPaths = Object.freeze({
    runner: 'tests/test_binding_quota.mjs', controls: 'tests/test_binding_quota_contract.mjs',
    model: 'tools/binding-quota-input.mjs', contract: 'tools/binding-quota-contract.mjs',
    requestBinding: 'tests/fixtures/binding-quota-request-package.mjs', requestUser: 'tests/fixtures/binding-quota-request-user.mjs',
    workerBinding: 'tests/fixtures/binding-quota-worker-package.mjs', workerUser: 'tests/fixtures/binding-quota-worker-user.mjs',
    workerHelper: 'tests/test_binding_quota_worker.cc', requestHelper: 'tests/test_binding_fetch_retained.cc',
    exitObserver: 'tests/graceful_worker_exit.h', rpcPolicy: 'src/binding_rpc.h', rpcBackend: 'src/binding_rpc.cc',
    workerRuntime: 'src/worker_runtime.cc', readyProof: 'src/host/binding_compile.cc',
});
const freeze = value => {
    if (value && typeof value === 'object') { for (const item of Object.values(value)) freeze(item); Object.freeze(value); }
    return value;
};
const context = (parent, worker) => ({
    requestIdType: 'bigint', requestId: String(parent), deadlineType: 'bigint', nullPrototype: true, frozen: true,
    ...(worker ? {signalNew: true, signalAborted: false} : {signalType: 'object', aborted: false, signalNew: true}),
});
export function bindingQuotaWireTypes(data) {
    const types = {objects: 0, arrays: 0, objectNullPrototypes: true, arrayPrototypes: true};
    const visit = value => {
        if (!value || typeof value !== 'object') return;
        if (Array.isArray(value)) types.arrays++; else types.objects++;
        for (const item of Object.values(value)) visit(item);
    };
    visit(data); return types;
}
const wrap = data => ({data, types: bindingQuotaWireTypes(data)});
const abortReason = () => ({name: 'AbortError', message: 'The operation was aborted', code: 20, isDOMException: true});
const requestModels = new Map(bindingQuotaVariants.map(variant => {
    const held = Array.from({length: 63}, (_, id) => ({id, call: context(1, false)}));
    const before = {held, heldSignalsAborted: Array(63).fill(false), abortedCount: 0, call: context(1, false)};
    const recovered = {input: {value: 'after64th-completed'}, heldCount: 63, abortedCount: 0, call: context(1, false)};
    const request1 = {variant, overflowPhase: 'promise', overflow: {accepted: false, name: 'Error', message: 'binding request quota exceeded'}, observed: 0, before: wrap(before), recovered: wrap(recovered), lateBeforeEnd: []};
    const aborted = Array.from({length: 63}, (_, id) => ({id, aborted: true, reason: abortReason(), event: {type: 'abort', targetSame: true, currentTargetSame: true}}));
    const stats = {held, heldSignalsAborted: Array(63).fill(true), aborted, completedSignalsAborted: [false, false], call: context(2, false)};
    const request2 = {stats: wrap(stats), late: Array.from({length: 63}, (_, id) => ({id, status: 'rejected', name: 'Error', message: 'binding call was canceled'}))};
    return [variant, freeze({request1, request2})];
}));
export function requestQuotaExpected(variant) {
    assert.ok(bindingQuotaVariants.includes(variant)); return structuredClone(requestModels.get(variant));
}
export function requestQuotaTemplate(variant) {
    assert.ok(bindingQuotaVariants.includes(variant)); return requestModels.get(variant);
}

const parents = [...Array.from({length: 16}, (_, index) => index+1), 21];
const late = parent => Array.from({length: 64}, (_, slot) => ({parent: String(parent), slot, status: 'rejected', name: 'Error', message: 'binding call was canceled'}));
const summary = recovery => wrap({
    active: recovery ? 960 : 0, peak: 1024, total: recovery ? 1024 : 1088,
    groups: (recovery ? parents.slice(0, 16) : parents).map(parent => ({request: String(parent), started: 64, aborted: !recovery || parent===1 ? 64 : 0})),
    completedSignalsAborted: Array(recovery ? 1 : 2).fill(false), call: context(recovery ? 21 : 23, true),
});
const slices = parents.map((parent, index) => wrap({
    request: String(parent), items: Array.from({length: 64}, (_, slot) => ({
        slot, call: context(parent, true), aborted: true, reason: abortReason(),
        abortEvents: [{type: 'abort', targetSame: true, currentTargetSame: true}],
    })), completedSignalsAborted: Array(index+3).fill(false), call: context(23, true),
}));
const final = {summary: summary(false), slices, late: parents.flatMap(late), recoveryBefore: {summary: summary(true), late: late(1)}};
const responses = [[17, 'plain'], [18, 'symbol'], [19, 'cycle'], [20, 'getter'], [22, 'plain']].map(([request, variant]) => ({request, value: {
    variant, phase: 'promise', result: {accepted: false, name: 'Error', message: 'binding call quota exceeded'}, observed: 0,
}}));
responses.push({request: 23, value: final});
const logs = [], events = [];
const request = (id, step, variant) => events.push({type: 'request', request: id, step, variant});
const log = (id, kind) => {
    events.push({type: 'log', index: logs.length}); logs.push({request: 0, raw: '\x03\x00logquota-'+kind+':'+id+':64'});
};
const cancel = id => {events.push({type: 'cancel', request: id}); log(id, 'aborted');};
for (const id of parents.slice(0, 16)) {request(id, 'hold', 'none'); log(id, 'ready');}
for (const [id, variant] of [[17, 'plain'], [18, 'symbol'], [19, 'cycle'], [20, 'getter']]) {request(id, 'overflow', variant); events.push({type: 'response', request: id});}
cancel(1); request(21, 'hold', 'recovery'); log(21, 'ready');
request(22, 'overflow', 'plain'); events.push({type: 'response', request: 22});
for (const id of parents.slice(1)) cancel(id);
request(23, 'final', 'none'); events.push({type: 'response', request: 23});
const workerModel = freeze({responses, logs, events});
export const workerQuotaTemplate = () => workerModel;
export const workerQuotaExpected = () => structuredClone(workerModel);
export const bindingQuotaExit = () => ({reaped: true, channelClosed: true, code: 0});
export function quotaReadyLine(platform) {
    assert.ok(['win32', 'linux', 'darwin'].includes(platform));
    // POSIX installs CORE/NOFILE RLIMITS; Windows memory limit0 does not.
    const features = platform==='win32' ? 0 : 1;
    return 'memoryLimit=0 actualFeatures='+features+' expectedFeatures='+features;
}
export function requestQuotaTranscript(row, platform) {
    return [quotaReadyLine(platform), 'BINDING_REQUEST 1', 'BINDING_PHASE 1 '+JSON.stringify(row.request1),
        'BINDING_REQUEST 2', 'BINDING_PHASE 2 '+JSON.stringify(row.request2), 'BINDING_EXIT '+JSON.stringify(row.workerExit)].join('\n')+'\n';
}
export function workerQuotaTranscript(row, platform) {
    const lines = [quotaReadyLine(platform)], values = new Map(row.responses.map(item => [item.request, item.value]));
    for (const event of row.events) {
        if (event.type==='request') lines.push('QUOTA_REQUEST '+event.request+' '+event.step+' '+event.variant);
        else if (event.type==='cancel') lines.push('QUOTA_CANCEL '+event.request);
        else if (event.type==='response') lines.push('QUOTA_RESPONSE '+event.request+' '+JSON.stringify(values.get(event.request)));
        else if (event.type==='log') {const item=row.logs[event.index]; lines.push('QUOTA_LOG '+item.request+' '+Buffer.from(item.raw).toString('hex'));}
        else throw new Error('unexpected quota transport event');
    }
    lines.push('QUOTA_EXIT '+JSON.stringify(row.workerExit)); return lines.join('\n')+'\n';
}
