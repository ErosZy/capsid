import assert from 'node:assert/strict';

// Independent source protocol: two workers, one channel slot each, and real
// DATA messages containing either one body or the 2 / 3 / remainder fragments.
// A zero configured limit is the unlimited control, not an empty-body limit.
export function responseLimitCases(encoding = null) {
    assert.ok(encoding === null || encoding === 'gzip', 'supported encoded-header regression');
    const cases = [];
    for (const limit of [8, 0]) {
        let channel = 0;
        for (const method of ['default', 'byob', 'least', 'high']) {
            for (const route of ['getReader', 'direct']) {
                for (const known of [true, false]) {
                    for (const length of [0, 7, 8, 9]) {
                        for (const fragmented of [false, true]) {
                            cases.push({limit, method, route, known, length, fragmented,
                                action: 'full', channel: ++channel});
                        }
                    }
                    for (const action of ['cancel', 'pending']) {
                        cases.push({limit, method, route, known, length: 7,
                            fragmented: true, action, channel: ++channel});
                    }
                }
            }
        }
    }
    if (encoding) for (const scenario of cases) if (scenario.known) scenario.encoding = encoding;
    return cases;
}

// Independent stored-block wire geometry: 10-byte header, 5 per data
// block, final empty block and 8-byte trailer. Cancellation leaves it open.
export function gzipWireLength(length, fragmented) {
    return 23 + length + (length ? (fragmented ? 15 : 5) : 0);
}

export function expectedResponseLimitState(scenario) {
    const {limit, method, route, known, length, fragmented, action} = scenario;
    const byob = method !== 'default';
    const over = limit !== 0 && length > limit && action === 'full';
    const early = over && known && !scenario.encoding;
    const error = over ? {name: 'TypeError',
        message: `fetch response body exceeded configured limit of ${limit} bytes`,
        isTypeError: true} : null;
    const inputBytes = byob ? 32 : null;
    const detached = byob ? 0 : null;
    const read = (bytes, done = false) => {
        const hasView = byob || !done;
        return {done, type: hasView ? 'Uint8Array' : null,
            bytes: hasView ? bytes : null,
            offset: hasView ? (done || !byob ? 0 : 8) : null,
            backingBytes: hasView ? (byob ? 32 : 4096) : null,
            outside: hasView ? true : null,
            inputBytes, inputAfterIssue: detached, inputAfter: detached};
    };
    let chunks = [];
    if (action !== 'full') {
        chunks = action === 'cancel' ? [[1, 2]] : [[1, 2], [3, 4, 5]];
    } else if (!early) {
        const source = Array.from({length}, (_, index) => index + 1);
        const fragments = !length ? [] : fragmented ?
            [source.slice(0, 2), source.slice(2, 5), source.slice(5)] : [source];
        let total = 0;
        for (const fragment of fragments) {
            if (!fragment.length) continue;
            total += fragment.length;
            // An overflowing source chunk is never partially delivered.
            if (limit && total > limit) break;
            chunks.push(fragment);
        }
        if (method === 'high') {
            // min=12 exceeds every fixture body: commit a partial view only at
            // EOF. A limit error must discard the uncommitted partial buffer.
            chunks = over ? [] : length ? [source] : [];
        }
    }
    const reads = chunks.map(bytes => read(bytes));
    if (action === 'full' && !over) reads.push(read([], true));
    const state = {method, route, known, length, action, phase: 'complete', reads,
        error, closed: over ? {error} : {fulfilled: true},
        cancel: over ? {error, sameRead: true} : {returnedUndefined: true},
        pending: action === 'pending' ? {
            beforeCancel: {readSettled: false, closedSettled: false},
            inputBytes, inputAfterIssue: detached,
            result: {error: {name: null, message: null, isTypeError: false},
                sameReason: false, type: 'object', tag: 'consumer-stop', keys: ['tag'], plain: true},
            inputAfter: detached,
        } : null,
        response: {status: 200, contentLength: known ? String(scenario.encoding ? gzipWireLength(length, fragmented) : length) : null},
        errorIdentity: true, bodyUsed: true, locked: false};
    if (scenario.encoding) state.response.contentEncoding = scenario.encoding;
    if (over) {
        // A previously errored native body rejects before transferring the
        // caller's buffer; a pending overflow happens after its transfer.
        state.rejectedInput = {inputBytes,
            inputAfterIssue: byob && early ? 32 : detached,
            inputAfter: byob && early ? 32 : detached};
    }
    return state;
}

export function assertResponseLimitRow(row, scenario) {
    assert.deepEqual(Object.keys(row).sort(),
        ['limit', 'fragmented', 'channel', 'sent', 'credit', 'cancels', 'live', 'observation'].sort());
    const {limit, fragmented, channel, length, action, known} = scenario;
    const sent = scenario.encoding ? (action === 'full' ? gzipWireLength(length, fragmented) : 25) : action === 'full' ? length : 5;
    assert.deepEqual({limit: row.limit, fragmented: row.fragmented, channel: row.channel,
        sent: row.sent, live: row.live}, {limit, fragmented, channel, sent, live: false});
    assert.equal(Number.isSafeInteger(row.credit), true);
    assert.ok(row.credit >= 0 && row.credit <= sent, 'no credit for unsent source bytes');
    const early = action === 'full' && limit !== 0 && known && !scenario.encoding && length > limit;
    assert.equal(row.cancels, action !== 'full' || early ? 1 : 0);
    if (action !== 'full') assert.equal(row.credit, scenario.encoding ? (action === 'cancel' ? 17 : 25) : action === 'cancel' ? 2 : 5);
    if (early) assert.equal(row.credit, 0);
    if (scenario.encoding && action === 'full' && fragmented && length) assert.ok(row.credit >= 25, 'both earlier encoded fragments actually consumed before final source fragment');
    assert.deepEqual(row.observation, expectedResponseLimitState(scenario),
        `real response body ${JSON.stringify(scenario)}`);
}

export function assertResponseLimitExit(row, limit) {
    assert.deepEqual(row, {limit, shutdownResult: 0, flushResult: 0, eventResult: 0,
        channelClosed: true, errorEvents: 0, processObserved: true, exitStatus: 0,
        waitError: 0, destroyForced: false}, 'actual graceful process exit and drained IPC');
}

export function validateResponseLimitTranscript(stdout, {encoding = null} = {}) {
    assert.equal(typeof stdout, 'string');
    const lines = stdout.trim().split(/\r?\n/);
    const inventory = responseLimitCases(encoding);
    let index = 0;
    for (const limit of [8, 0]) {
        for (let count = 0; count < 160; count++, index++) {
            const line = lines.shift();
            assert.ok(line?.startsWith('LIMIT_ROW '), `complete source row ${index}`);
            assertResponseLimitRow(JSON.parse(line.slice(10)), inventory[index]);
        }
        const exitLine = lines.shift();
        assert.ok(exitLine?.startsWith('LIMIT_WORKER_EXIT '), 'actual process exit evidence');
        const exit = JSON.parse(exitLine.slice('LIMIT_WORKER_EXIT '.length));
        assert.equal(exitLine, 'LIMIT_WORKER_EXIT ' + JSON.stringify(exit), 'canonical exit observation');
        assertResponseLimitExit(exit, limit);
        assert.equal(lines.shift(),
            `LIMIT_WORKER_COMPLETE limit=${limit} rows=160 slots=1 naturalExit=true`);
    }
    assert.deepEqual(lines, [], 'no skipped, duplicate or unrecognised observations');
    return {complete: true, workers: 2, rows: index, configuredLimits: [8, 0], slotsPerWorker: 1,
        ...(encoding ? {knownLengthContentEncoding: encoding, encodedHeaderCases: 160} : {})};
}
