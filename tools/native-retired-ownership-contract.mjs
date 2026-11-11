import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';

const packetHashes = {
    raw: {whole: 'a9ce213e0c75b432a9e5c166e4f38e661ec95a3370cd111c72688ecb4b503205', prefix: '039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81'},
    gzip: {whole: 'cd3f19dbdf51d99a27569b63199e9f1a2d9a0393b1688ab31d5c6d8f134a50a3', prefix: '8c1383ad4d7b91e01832241b9c4cedbdf1bacd586383eadc99446f77a77a6199'}
};

export const promiseModes = ['direct', 'await', 'await-then', 'await-prelude', 'await-nested',
    'new-promise-adopt', 'resolve-adopt', 'all-one', 'all-resolved', 'race-one', 'any-one',
    'all-settled', 'finally', 'catch', 'then-adopt', 'async-return'];
export const iteratorModes = ['next', 'prevent', 'loop', 'prelude'];
export const controlModes = ['timer', 'ordinary', 'mixed-all', 'override-then', 'timer-complete', 'ordinary-complete'];
export const branchGraphs = ['tee-default', 'tee-byob', 'clone-default', 'clone-byob',
    'nested-tee', 'clone-tee', 'tee-transform', 'clone-pipe'];
export const retiredGroups = [...promiseModes.map(mode => 'promise_' + mode), 'branches',
    ...iteratorModes.map(mode => 'iterator_' + mode), 'controls'];
export function plansForGroup(group) {
    assert.ok(retiredGroups.includes(group), 'known retired ownership group');
    let graphs;
    if (group === 'branches') graphs = branchGraphs;
    else if (group === 'controls') graphs = controlModes;
    else {
        const mode = group.slice(group.indexOf('_') + 1);
        graphs = ['plain', 'tee', 'clone'].map(topology => topology + '-' + mode);
    }
    return ['raw', 'gzip'].flatMap(bodyKind => graphs.map(graph => ({bodyKind, graph})));
}
function prefix(result, byob = false) {
    assert.equal(result.done, false, 'first read is data');
    assert.deepEqual(result.bytes, [1, 2, 3], 'exact held decoded prefix');
    if ('offset' in result) {assert.equal(result.offset, byob ? 8 : 0); assert.equal(result.capacity, byob ? 80 : 4096);}
}
function responseMetadata(value, row) {
    assert.equal(value.status, 201);
    assert.equal(value.statusText, 'Graph');
    assert.equal(value.ok, true);
    assert.equal(value.url, row.targetUrl);
    assert.equal(value.redirected, false);
    assert.equal(value.type, 'default');
    assert.equal(value.bodyUsed, true);
    assert.equal(value.locked, true);
    const headers = [['connection', 'close'], ...(row.bodyKind === 'gzip' ? [['content-encoding', 'gzip']] : []),
        ['content-length', String(row.sourcePrefixEncodedLength + row.sourceSuffixEncodedLength)], ['x-graph', 'held']];
    assert.deepEqual(value.headers, headers, 'original held HTTP metadata');
}
function publicState(value, row, group) {
    assert.equal(value.graph, row.graph);
    if (group === 'branches') {
        responseMetadata(value.response, row);
        for (const copy of value.copies) responseMetadata(copy, row);
        const count = ['nested-tee', 'clone-tee'].includes(row.graph) ? 3 : 2;
        assert.equal(value.copies.length, row.graph.startsWith('clone') ? 1 : 0);
        assert.deepEqual(value.leaves, Array.from({length: count}, () => ({locked: true})));
        assert.equal(value.jobs.length, count);
        value.jobs.forEach((job, index) => {
            assert.equal(job.index, index);
            assert.equal(job.phase, 'pending');
            assert.equal(job.closedPhase, row.graph === 'clone-pipe' && index === 0 ? 'not-acquired' : 'pending');
            assert.equal(job.chunks.length, 1);
            prefix(job.chunks[0], row.graph.endsWith('-byob'));
            assert.equal(job.chunks[0].type, 'Uint8Array');
            assert.deepEqual(job.transformLocks, row.graph === 'tee-transform' && index === 0 ? {readable: true, writable: true} : null);
            assert.equal(job.sinkLocked, row.graph === 'clone-pipe' && index === 0 ? true : null);
            assert.equal(job.readerReleased, undefined);
            assert.equal(job.error, undefined);
            assert.equal(job.sinkClosed, undefined);
            assert.equal(job.sinkAbort, undefined);
        });
    } else {
        assert.equal(value.bodyLocked, true);
        if (group === 'controls') {
            prefix(value.first);
            const complete = row.graph.endsWith('-complete');
            assert.equal(value.phase, complete ? 'fulfilled' : ['timer', 'ordinary'].includes(row.graph) ? 'followup' : row.graph);
            assert.equal(value.outerThenRan, complete);
            assert.equal(value.getterCalls, row.graph === 'override-then' ? 1 : 0);
            assert.equal(value.customThenCalls, row.graph === 'override-then' ? 1 : 0);
        } else {
            assert.equal(value.phase, 'pending');
            assert.equal(value.outerThenRan, false);
            assert.equal(value.error, null);
            const count = row.graph.startsWith('plain-') ? 1 : 2;
            assert.deepEqual(value.leaves, [{locked: true}, ...Array.from({length: count - 1}, () => ({locked: false}))]);
            if (group.startsWith('iterator_')) {
                assert.equal(value.chunks.length, 1);
                prefix(value.chunks[0]);
            } else {
                prefix(value.first);
                assert.equal(value.next, null);
                assert.equal(value.finallyRan, false);
            }
        }
    }
}
function retiredSource(runtime, row) {
    assert.equal(runtime.sources.length, 1, 'one actual upstream connection');
    assert.equal(runtime.ownerHttpEndedBeforeGuest, true);
    assert.equal(runtime.sourceTerminalBeforeGuestRequest, true);
    assert.equal(runtime.sourceBeforeGuestRequest.length, 1);
    const source = runtime.sourceBeforeGuestRequest[0];
    assert.ok(source.peerEnded || source.peerReset, 'upstream retired before guest');
    assert.equal(source.fixtureEnded, false, 'fixture did not fabricate EOF');
    assert.equal(source.fixtureDestroyed, false, 'fixture did not fabricate disconnect');
    assert.ok(source.events.includes('request'));
    assert.ok(source.events.includes('headers-and-prefix-sent') || source.events.includes('delayed-prefix-sent'));
    assert.equal(source.events.includes('suffix-released'), false);
    assert.equal(createHash('sha256').update(Buffer.from(source.prefix, 'base64')).digest('hex'), packetHashes[row.bodyKind].prefix);
    const headers = 'HTTP/1.1 201 Graph\r\nContent-Length: ' + (row.sourcePrefixEncodedLength + row.sourceSuffixEncodedLength) + '\r\n' +
        (row.bodyKind === 'gzip' ? 'Content-Encoding: gzip\r\n' : '') + 'X-Graph: held\r\nConnection: close\r\n\r\n';
    assert.equal(Buffer.from(source.headers, 'base64').toString(), headers);
    const url = new URL(row.targetUrl);
    assert.ok(Buffer.from(source.rawRequest, 'base64').toString().startsWith(`GET ${url.pathname}${url.search} HTTP/1.1\r\n`));
}
export function validateRetiredCase(row, plan, group) {
    assert.equal(row.bodyKind, plan.bodyKind);
    assert.equal(row.graph, plan.graph);
    assert.deepEqual(row.sourceDecodedBytes, Array.from({length: 17}, (_, i) => i + 1));
    assert.equal(row.sourcePrefixEncodedLength, row.bodyKind === 'raw' ? 3 : 19);
    assert.equal(row.sourceSuffixEncodedLength, row.bodyKind === 'raw' ? 14 : 24);
    assert.equal(row.sourcePacketSHA256, packetHashes[row.bodyKind].whole);
    assert.equal(row.sourcePrefixSHA256, packetHashes[row.bodyKind].prefix);
    const target = new URL(row.targetUrl);
    assert.equal(target.hostname, 'localhost');
    assert.equal(target.searchParams.get('readerRoute'), row.graph);
    assert.equal(target.pathname, '/' + row.bodyKind + '-body');
    const reference = row.reference, native = row.native;
    assert.equal(reference.collectionFailure, undefined, 'reference completed');
    retiredSource(reference, row);
    assert.equal(reference.suffixReleased, false);
    assert.equal(reference.sourceTerminalBeforeRuntimeStop, true);
    assert.equal(reference.healthyReuse, true);
    publicState(reference.owner, row, group);
    publicState(reference.guest, row, group);
    assert.deepEqual(reference.owner, reference.guest, 'retirement does not settle or unlock held objects');
    assert.equal(native.helperForced, undefined, 'parent never forces a successful native outcome');
    retiredSource(native, row);
    assert.ok(native.stdout.includes('CONTEXT_OWNER_RETIRED true'));
    publicState(native.owner, row, group);
    const printed = prefix => {
        const values = native.stdout.split(/\r?\n/).filter(line => line.startsWith(prefix));
        assert.equal(values.length, 1, 'one complete original helper record');
        return JSON.parse(values[0].slice(prefix.length));
    };
    assert.deepEqual(printed('CONTEXT_OWNER '), native.owner);

    assert.deepEqual(native.owner, reference.owner, 'exact native owner public state');
    assert.ok(!/ERROR: AddressSanitizer|LeakSanitizer:|runtime error:|AddressSanitizer:DEADLYSIGNAL/.test(native.stderr));
    if (group === 'controls' && !row.graph.endsWith('-complete')) {
        assert.deepEqual(native.exit, {code: 1, signal: null}, 'real native guard failure');
        assert.match(native.stderr, /TOKEN LEAK on clean exit|WORKER POISONED: terminal continuation leak/);
        assert.ok(native.collectionFailure, 'success path must reject this actual guard failure');
        return;
    }
    assert.equal(native.collectionFailure, undefined);
    assert.deepEqual(native.exit, {code: 0, signal: null});
    assert.deepEqual(native.workerExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
    assert.equal(native.suffixReleased, false);
    assert.equal(native.sourceTerminalBeforeRuntimeStop, true);
    assert.equal(native.healthyReuse, true);
    publicState(native.guest, row, group);
    assert.deepEqual(native.guest, reference.guest, 'exact native guest public state');
    assert.deepEqual(printed('CONTEXT_GUEST '), native.guest);
    assert.deepEqual(printed('CONTEXT_PROTOCOL '), native.protocol);
    assert.deepEqual(printed('CONTEXT_EXIT '), native.workerExit);
    assert.equal(native.protocol.requests, 3);
    assert.deepEqual(native.protocol.replies.map(value => value.id), [1, 2, 3]);
    for (const reply of native.protocol.replies) {
        assert.equal(reply.heads, 1);
        assert.equal(reply.ends, 1);
        assert.ok(Number.isSafeInteger(reply.bodyBytes) && reply.bodyBytes > 0);
        assert.ok(Number.isSafeInteger(reply.bodyFrames) && reply.bodyFrames > 0);
        assert.equal(reply.returnedCredit, reply.bodyBytes);
    }
}
export function validateRetiredReport(report, group) {
    assert.equal(report.complete, true);
    assert.equal(report.group, group);
    assert.equal(report.eventDeadlineMilliseconds, 5000);
    assert.equal(report.naturalNativeExitDeadlineMilliseconds, 2500);
    const plans = plansForGroup(group);
    assert.equal(report.cases.length, plans.length, 'every original group case completed');
    report.cases.forEach((row, index) => validateRetiredCase(row, plans[index], group));
    assert.equal(report.measuredInputsUnchanged, true);
    return {cases: plans.length, expectedGuardRejections: group === 'controls' ? 8 : 0};
}
