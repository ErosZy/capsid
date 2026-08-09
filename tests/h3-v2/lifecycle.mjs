import assert from 'node:assert/strict';
import {
    FrameworkWorker,
    decoder,
    unhex,
} from './protocol.mjs';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    args.set(process.argv[index], process.argv[index + 1]);
}

const driverPath = args.get('--driver');
const workerPath = args.get('--worker');
const bundlePath = args.get('--bundle');
if (!driverPath || !workerPath || !bundlePath) {
    throw new Error('expected --driver, --worker and --bundle');
}

const worker = new FrameworkWorker({
    driverPath,
    workerPath,
    bundlePath,
    flags: [ '--timeout-ms', '500' ],
    collectEvents: true,
});

const url = path => `https://compat.example${path}`;
const json = result => JSON.parse(decoder.decode(result.body));
const text = result => decoder.decode(result.body);
const assertResponse = (result, status, label) => {
    assert.equal(result.error, '', `${label}: worker error`);
    assert.equal(result.status, status, `${label}: status`);
};

let nextId = 1000;
const request = (path, options = {}) => worker.request({
    id: ++nextId,
    method: options.method ?? 'GET',
    url: url(path),
    headers: options.headers ?? [],
    body: options.body ?? new Uint8Array(),
    chunkSize: options.chunkSize ?? 257,
});

const assertReusable = async phase => {
    const result = await request('/entry');
    assertResponse(result, 200, `${phase}: reusable entry`);
    assert.equal(text(result), 'h3-entry-ok', `${phase}: entry body`);
};

const assertCleanContext = async phase => {
    const result = await request(`/runtime/context-probe/${phase}`);
    assertResponse(result, 200, `${phase}: context probe`);
    assert.deepEqual(json(result), {
        phase,
        concurrent: null,
        middleware: null,
        activeContexts: 1,
    }, `${phase}: request-local context cleanup`);
};

let phase = 'startup';
try {
    await worker.start();

    phase = 'concurrent lazy initialization';
    const [ firstLazy, secondLazy ] = await worker.concurrent({
        firstId: ++nextId,
        firstUrl: url('/plugins/lazy?token=first'),
        secondId: ++nextId,
        secondUrl: url('/plugins/lazy?token=second'),
    });
    assertResponse(firstLazy, 200, 'first lazy request');
    assertResponse(secondLazy, 200, 'second lazy request');
    assert.deepEqual(json(firstLazy), {
        initializations: 1,
        token: 'first',
    });
    assert.deepEqual(json(secondLazy), {
        initializations: 1,
        token: 'second',
    });
    await assertCleanContext('after-lazy');

    phase = 'concurrent request context isolation';
    const [ slow, fast ] = await worker.concurrent({
        firstId: ++nextId,
        firstUrl: url(
            '/runtime/concurrent/slow?token=slow&delay=40',
        ),
        secondId: ++nextId,
        secondUrl: url(
            '/runtime/concurrent/fast?token=fast&delay=5',
        ),
    });
    assertResponse(slow, 200, 'slow concurrent request');
    assertResponse(fast, 200, 'fast concurrent request');
    assert.deepEqual(json(slow), {
        id: 'slow',
        token: 'slow',
        context: { id: 'slow', token: 'slow' },
        query: { token: 'slow', delay: '40' },
    });
    assert.deepEqual(json(fast), {
        id: 'fast',
        token: 'fast',
        context: { id: 'fast', token: 'fast' },
        query: { token: 'fast', delay: '5' },
    });
    await assertCleanContext('after-concurrent');

    phase = 'dispose success and error';
    const disposeSuccess = await request('/runtime/dispose/success');
    assertResponse(disposeSuccess, 200, 'dispose success');
    assert.equal(text(disposeSuccess), 'dispose-success');
    const disposeError = await request('/runtime/dispose/error');
    assertResponse(disposeError, 432, 'dispose error');
    const initialDispose = await request('/runtime/dispose/counts');
    assert.equal(json(initialDispose).success, 1);
    assert.equal(json(initialDispose).error, 1);
    assert.equal(json(initialDispose).successReason, null);
    assert.equal(json(initialDispose).errorReason, null);
    assert.equal(json(initialDispose).activeContexts, 1);

    phase = 'handler cancellation';
    await worker.cancel({
        id: ++nextId,
        url: url('/runtime/wait-for-abort'),
        mode: 'started',
    });
    let abortCounts = await request('/runtime/abort-counts');
    assertResponse(abortCounts, 200, 'handler cancellation counts');
    assert.equal(json(abortCounts).handlers, 1);
    assert.equal(json(abortCounts).handlerSignals, 1);
    assert.equal(json(abortCounts).dispose.cancel, 1);
    await assertReusable('handler cancellation');
    await assertCleanContext('after-handler-cancel');

    phase = 'request body parsing cancellation';
    await worker.cancelUpload({
        id: ++nextId,
        url: url('/runtime/cancel-parse'),
    });
    abortCounts = await request('/runtime/abort-counts');
    assertResponse(abortCounts, 200, 'body cancellation counts');
    assert.equal(json(abortCounts).parses, 1);
    assert.equal(json(abortCounts).dispose.cancelParse, 1);
    await assertReusable('body parsing cancellation');
    await assertCleanContext('after-body-cancel');

    phase = 'response stream cancellation';
    await worker.cancel({
        id: ++nextId,
        url: url('/runtime/stream-cancel'),
        mode: 'body',
    });
    abortCounts = await request('/runtime/abort-counts');
    assertResponse(abortCounts, 200, 'stream cancellation counts');
    assert.equal(json(abortCounts).streams, 1);
    assert.equal(json(abortCounts).dispose.stream, 1);
    assert.ok(
        json(abortCounts).dispose.abortReasons >= 1,
        'stream cancellation must dispose with an AbortError',
    );
    await assertReusable('stream cancellation');
    await assertCleanContext('after-stream-cancel');

    phase = 'dispose accounting after cancellation';
    const disposeCounts = await request('/runtime/dispose/counts');
    assertResponse(disposeCounts, 200, 'dispose counts');
    assert.deepEqual(
        {
            success: json(disposeCounts).success,
            error: json(disposeCounts).error,
            cancel: json(disposeCounts).cancel,
            cancelParse: json(disposeCounts).cancelParse,
            stream: json(disposeCounts).stream,
            activeContexts: json(disposeCounts).activeContexts,
        },
        {
            success: 1,
            error: 1,
            cancel: 1,
            cancelParse: 1,
            stream: 1,
            activeContexts: 1,
        },
    );

    phase = '404 and handled error reuse';
    const notFound = await request('/lifecycle-missing');
    assertResponse(notFound, 404, 'not found');
    await assertReusable('404');
    const handledError = await request('/errors/http');
    assertResponse(handledError, 451, 'handled HTTPError');
    await assertReusable('handled error');

    phase = 'stream failure reuse';
    const streamFailure = await request('/errors/stream');
    assert.match(
        streamFailure.error,
        /^RuntimeError: .*stream-failure/,
        'stream failure classification',
    );
    await assertReusable('stream failure');
    await assertCleanContext('after-stream-failure');

    phase = 'async timeout reuse and cleanup';
    const asyncTimeout = await request('/runtime/delay?ms=1000');
    assert.match(
        asyncTimeout.error,
        /^TimeoutError: /,
        'async timeout classification',
    );
    await assertReusable('async timeout');
    const settle = await request('/runtime/delay?ms=10');
    assertResponse(settle, 200, 'async timeout cleanup settle');
    assert.equal(text(settle), 'delay-complete');
    const timeoutCounts = json(await request('/runtime/abort-counts'));
    assert.equal(
        timeoutCounts.timeouts,
        1,
        'async timeout must abort the request signal',
    );
    assert.equal(
        timeoutCounts.dispose.timeout,
        2,
        'timed-out and settling H3 events must both dispose',
    );
    await assertCleanContext('after-async-timeout');

    phase = 'native event ownership';
    const ownership = await request('/runtime/ownership');
    assertResponse(ownership, 200, 'ownership route');
    assert.equal(text(ownership), 'ownership-ok');
    const ownershipLogs = ownership.events.filter(
        event => event.kind === 'LOG',
    );
    const beforeLog = ownershipLogs.find(event =>
        decoder.decode(unhex(event.text)) === 'capsid-owner:before');
    const afterLog = ownershipLogs.find(event =>
        decoder.decode(unhex(event.text)) === 'capsid-owner:after');
    assert.ok(beforeLog, 'before-await LOG must be emitted');
    assert.ok(afterLog, 'after-await LOG must be emitted');
    const ownershipId = nextId;
    assert.equal(
        Number(beforeLog.requestId),
        ownershipId,
        'before-await LOG must carry the request id',
    );
    assert.equal(
        Number(afterLog.requestId),
        ownershipId,
        'after-await LOG must carry the request id',
    );

    phase = 'cancel must end the realm';
    const cancelEvents = await worker.cancelContinuation({
        id: ++nextId,
        url: url('/runtime/ownership-cancel'),
        marker: 'capsid-owner:after-cancel',
    });
    assert.ok(
        !cancelEvents.some(event =>
            event.kind === 'LOG' &&
            decoder.decode(unhex(event.text)) === 'capsid-owner:after-cancel'),
        'after-cancel continuation must never run',
    );

    // NOTE: this phase runs last. On the fixed implementation the cancel
    // above poisons the worker, so any request after it (the old
    // cpu-timeout phase) must be reworked in WP-03; keeping it here
    // preserves the pre-fix ordering where the continuation survived the
    // cancel.
    phase = 'synchronous CPU timeout';
    const cpuTimeout = await request('/runtime/cpu-timeout');
    assert.match(
        cpuTimeout.error,
        /^(TimeoutError|RuntimeError): /,
        'synchronous CPU timeout classification',
    );

    await worker.stop();
} catch (cause) {
    const lifecycleState = worker.lifecycleState;
    await worker.kill().catch(() => {});
    throw new Error(JSON.stringify({
        failure: cause?.message ?? String(cause),
        phase,
        workerLifecycleState: lifecycleState,
    }, null, 2), { cause });
}

console.log(
    'PASS: H3 v2 lazy, concurrency, disposal, cancellation, timeout and reuse',
);
