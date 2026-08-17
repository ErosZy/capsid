const textDecoder = new TextDecoder();
let matrixStage = 'not-started';

function setMatrixStage(stage) {
    matrixStage = stage;
    console.log(`direct-fetch-matrix:${stage}`);
}

function assert(condition, message) {
    if (!condition) {
        throw new Error(message);
    }
}

async function expectNetworkError(promise, label) {
    try {
        await promise;
    } catch (error) {
        assert(error instanceof TypeError, `${label}: expected TypeError, got ${error?.name}`);
        return;
    }
    throw new Error(`${label}: fetch unexpectedly succeeded`);
}

function byteStream(size) {
    let remaining = size;

    return new ReadableStream({
        pull(controller) {
            if (remaining === 0) {
                controller.close();
                return;
            }
            const chunk = new Uint8Array(Math.min(8192, remaining));
            chunk.fill(0x6c);
            remaining -= chunk.byteLength;
            controller.enqueue(chunk);
        },
    });
}

async function expectBodyLimitError(promise, label) {
    try {
        await promise;
    } catch (error) {
        assert(error instanceof TypeError,
            `${label}: expected TypeError, got ${error?.name}`);
        assert(error.message.includes('configured limit'),
            `${label}: missing limit error detail: ${error.message}`);
        return;
    }
    throw new Error(`${label}: configured body limit was not enforced`);
}

async function runBodyLimits(primary, limit) {
    setMatrixStage('request-body-limit-boundary');
    const exactUpload = await fetch(`${primary}/upload`, {
        method: 'POST',
        body: byteStream(limit),
        duplex: 'half',
    });
    assert((await exactUpload.text()).startsWith(`${limit}|`),
        'request exactly at configured limit failed');

    setMatrixStage('request-body-limit-redirect');
    const replayed = await fetch(
        `${primary}/redirect?code=307&to=/inspect`,
        {
            method: 'POST',
            headers: { 'content-type': 'text/plain' },
            body: 'limited-replayable-body',
        });
    assert(await replayed.text() ===
        'POST|limited-replayable-body||text/plain|',
    'configured request limit broke replayable redirect body');

    setMatrixStage('request-body-limit-known');
    await expectBodyLimitError(
        fetch(`${primary}/upload`, {
            method: 'POST',
            body: new Uint8Array(limit + 1),
        }),
        'known-length request body');

    setMatrixStage('response-body-limit-boundary');
    const exactResponse = await fetch(
        `${primary}/sized-response?size=${limit}`);
    assert((await exactResponse.arrayBuffer()).byteLength === limit,
        'response exactly at configured limit failed');

    setMatrixStage('response-body-limit-known');
    const oversizedResponse = await fetch(
        `${primary}/sized-response?size=${limit + 1}`);
    await expectBodyLimitError(
        oversizedResponse.arrayBuffer(),
        'known-length response body');
    await new Promise(resolve => setTimeout(resolve, 50));

    setMatrixStage('response-body-limit-stream');
    const oversizedStream = await fetch(
        `${primary}/sized-response?size=${limit + 1}&chunked=1`);
    await expectBodyLimitError(
        oversizedStream.arrayBuffer(),
        'streaming response body');
    await new Promise(resolve => setTimeout(resolve, 50));

    setMatrixStage('request-body-limit-stream');
    await expectBodyLimitError(
        fetch(`${primary}/upload`, {
            method: 'POST',
            body: byteStream(limit + 1),
            duplex: 'half',
        }),
        'streaming request body');

    return [ 'request-body-limit', 'response-body-limit' ];
}

async function runMatrix(primary, secondary, closedPort) {
    const completed = [];

    setMatrixStage('headers');
    const headersResponse = await fetch(`${primary}/headers`, {
        headers: [
            [ 'x-request-duplicate', 'one' ],
            [ 'x-request-duplicate', 'two' ],
        ],
    });
    assert(headersResponse.status === 200, 'headers: status');
    assert(headersResponse.statusText === 'Matrix Phrase',
        `headers: statusText ${JSON.stringify(headersResponse.statusText)}`);
    assert(headersResponse.headers.get('x-duplicate') === 'one, two',
        `headers: duplicate response value ${headersResponse.headers.get('x-duplicate')}`);
    assert(JSON.stringify(headersResponse.headers.getSetCookie()) ===
        JSON.stringify([ 'a=1; Path=/', 'b=2; Path=/' ]),
    `headers: Set-Cookie ${JSON.stringify(headersResponse.headers.getSetCookie())}`);
    assert(await headersResponse.text() === 'one, two', 'headers: duplicate request value');
    completed.push('headers');

    setMatrixStage('connection-reuse');
    const acceptsBefore = Number(
        await (await fetch(`${primary}/accept-count`)).text());
    for (let reuse = 0; reuse < 2; ++reuse) {
        const pooled = await fetch(`${primary}/headers`, {
            headers: [
                [ 'x-request-duplicate', 'one' ],
                [ 'x-request-duplicate', 'two' ],
            ],
        });
        assert(await pooled.text() === 'one, two',
            'pooled connection headers body');
    }
    const acceptsAfter = Number(
        await (await fetch(`${primary}/accept-count`)).text());
    assert(acceptsAfter === acceptsBefore,
        `sequential fetch must reuse the pooled connection: ${acceptsBefore} -> ${acceptsAfter}`);
    const evicted = await fetch(`${primary}/conn-close`);
    assert(evicted.status === 200, 'conn-close response status');
    const acceptsEvicted = Number(
        await (await fetch(`${primary}/accept-count`)).text());
    assert(acceptsEvicted === acceptsAfter + 1,
        `Connection: close response must evict the pooled connection: ${acceptsAfter} -> ${acceptsEvicted}`);
    completed.push('connection-reuse');

    setMatrixStage('pool-isolation');
    const primaryAcceptsBefore = Number(
        await (await fetch(`${primary}/accept-count`)).text());
    const secondaryAcceptsBefore = Number(
        await (await fetch(`${secondary}/accept-count`)).text());
    await (await fetch(`${primary}/headers`)).text();
    await (await fetch(`${secondary}/headers`)).text();
    await (await fetch(`${primary}/headers`)).text();
    const primaryAcceptsAfter = Number(
        await (await fetch(`${primary}/accept-count`)).text());
    const secondaryAcceptsAfter = Number(
        await (await fetch(`${secondary}/accept-count`)).text());
    assert(primaryAcceptsAfter === primaryAcceptsBefore + 1,
        `primary endpoint opened ${primaryAcceptsAfter - primaryAcceptsBefore} connections instead of 1`);
    assert(secondaryAcceptsAfter === secondaryAcceptsBefore + 1,
        `secondary endpoint opened ${secondaryAcceptsAfter - secondaryAcceptsBefore} connections instead of 1`);
    completed.push('pool-isolation');

    setMatrixStage('redirect-302-post');
    const post302 = await fetch(`${primary}/redirect?code=302&to=/inspect`, {
        method: 'POST',
        headers: {
            'content-type': 'text/plain',
            'x-preserved': 'yes',
        },
        body: 'post-body',
    });
    const post302Inspection = await post302.text();
    assert(post302Inspection === 'GET||yes||',
        `302 POST rewrite/header cleanup: ${post302Inspection}`);

    setMatrixStage('redirect-302-put');
    const put302 = await fetch(`${primary}/redirect?code=302&to=/inspect`, {
        method: 'PUT',
        headers: { 'content-type': 'text/plain' },
        body: 'put-body',
    });
    const put302Inspection = await put302.text();
    assert(put302Inspection === 'PUT|put-body||text/plain|',
        `302 must preserve non-POST method and body: ${put302Inspection}`);

    setMatrixStage('redirect-303');
    const put303 = await fetch(`${primary}/redirect?code=303&to=/inspect`, {
        method: 'PUT',
        headers: { 'content-type': 'text/plain' },
        body: 'put-body',
    });
    const put303Inspection = await put303.text();
    assert(put303Inspection === 'GET||||',
        `303 must rewrite to GET: ${put303Inspection}`);

    setMatrixStage('redirect-307');
    const post307 = await fetch(`${primary}/redirect?code=307&to=/inspect`, {
        method: 'POST',
        headers: { 'content-type': 'text/plain' },
        body: 'preserved-body',
    });
    const post307Inspection = await post307.text();
    assert(post307Inspection === 'POST|preserved-body||text/plain|',
        `307 must preserve method and body: ${post307Inspection}`);

    setMatrixStage('redirect-modes');
    await expectNetworkError(
        fetch(`${primary}/redirect?code=302&to=/inspect`, { redirect: 'error' }),
        'redirect error mode');

    const manual = await fetch(
        `${primary}/redirect?code=302&to=/inspect`,
        { redirect: 'manual' });
    assert(manual.status === 302, `manual redirect status ${manual.status}`);
    assert(manual.headers.get('location') === '/inspect', 'manual redirect location');

    setMatrixStage('status-304');
    const notModified = await fetch(`${primary}/not-modified`);
    assert(notModified.status === 304, `304 response status ${notModified.status}`);

    setMatrixStage('redirect-loop');
    await expectNetworkError(fetch(`${primary}/redirect-loop`), 'redirect loop');

    setMatrixStage('redirect-cross-origin');
    const crossOrigin = await fetch(
        `${primary}/redirect?code=302&to=${encodeURIComponent(`${secondary}/inspect`)}`,
        {
            headers: {
                authorization: 'Bearer secret',
                'x-preserved': 'yes',
            },
        });
    const crossOriginInspection = await crossOrigin.text();
    assert(crossOriginInspection === 'GET||yes||',
        `cross-origin redirect leaked Authorization: ${crossOriginInspection}`);
    completed.push('redirect');

    setMatrixStage('request-streaming');
    let uploadIndex = 0;
    const upload = new ReadableStream({
        pull(controller) {
            if (uploadIndex === 64) {
                controller.close();
                return;
            }
            const chunk = new Uint8Array(8192);
            chunk.fill(uploadIndex & 0xff);
            uploadIndex++;
            controller.enqueue(chunk);
        },
    });
    const uploadResponse = await fetch(`${primary}/upload`, {
        method: 'POST',
        body: upload,
        duplex: 'half',
    });
    assert(await uploadResponse.text() === '524288|0|63',
        'streaming request body was truncated or reordered');

    setMatrixStage('response-streaming');
    const streamedResponse = await fetch(`${primary}/stream-response`);
    const streamedReader = streamedResponse.body.getReader();
    const first = await streamedReader.read();
    assert(!first.done && new TextDecoder().decode(first.value) === 'alpha',
        'response first stream chunk');
    const second = await streamedReader.read();
    assert(!second.done && new TextDecoder().decode(second.value) === 'beta',
        'response second stream chunk');
    assert((await streamedReader.read()).done, 'response stream end');

    setMatrixStage('body-boundary');
    const largeResponse = await fetch(`${primary}/large-response`);
    const largeReader = largeResponse.body.getReader();
    let largeSize = 0;
    let largeChunks = 0;
    for (;;) {
        const { value, done } = await largeReader.read();
        if (done) {
            break;
        }
        largeSize += value.byteLength;
        largeChunks++;
        for (const byte of value) {
            assert(byte === 0x7a, 'large response data corruption');
        }
    }
    assert(largeSize === 524288, `large response size ${largeSize}`);
    assert(largeChunks > 1, 'large response was not streamed');
    completed.push('streaming-body-boundary');

    setMatrixStage('abort');
    const preAborted = new AbortController();
    preAborted.abort();
    try {
        await fetch(`${primary}/headers`, { signal: preAborted.signal });
        throw new Error('pre-aborted fetch unexpectedly succeeded');
    } catch (error) {
        assert(error?.name === 'AbortError', `pre-abort name ${error?.name}`);
    }

    const duringAbort = new AbortController();
    const abortResponse = await fetch(`${primary}/abort-body`, {
        signal: duringAbort.signal,
    });
    const abortReader = abortResponse.body.getReader();
    const abortFirst = await abortReader.read();
    assert(!abortFirst.done && textDecoder.decode(abortFirst.value) === 'first',
        'abort response first chunk');
    duringAbort.abort();
    try {
        await abortReader.read();
        throw new Error('aborted response stream unexpectedly completed');
    } catch (error) {
        assert(error?.name === 'AbortError', `body abort name ${error?.name}`);
    }
    completed.push('abort');

    setMatrixStage('network-errors');
    await expectNetworkError(
        fetch(`http://127.0.0.1:${closedPort}/connection-error`),
        'connection error');
    await expectNetworkError(
        fetch(`https://127.0.0.1:${new URL(primary).port}/tls-error`),
        'TLS protocol error');
    await expectNetworkError(
        fetch('http://capsid-direct-fetch.invalid/dns-error'),
        'DNS error');
    completed.push('network-errors');

    return completed;
}

async function runEgressProbe(target) {
    try {
        const response = await fetch(target);
        return {
            allowed: true,
            status: response.status,
            body: await response.text(),
        };
    } catch (error) {
        return {
            allowed: false,
            name: error?.name,
            message: error?.message,
        };
    }
}

export default {
    async fetch(request) {
        const url = new URL(request.url);
        const primary = url.searchParams.get('primary');
        const secondary = url.searchParams.get('secondary');
        const closedPort = url.searchParams.get('closed');

        try {
            if (url.pathname.endsWith('/egress-probe')) {
                const probe = await runEgressProbe(
                    url.searchParams.get('target'));
                return Response.json({ passed: true, ...probe });
            }
            const completed = url.pathname.endsWith('/body-limits')
                ? await runBodyLimits(
                    primary,
                    Number(url.searchParams.get('limit')))
                : await runMatrix(primary, secondary, closedPort);
            return Response.json({ passed: true, completed });
        } catch (error) {
            return Response.json({
                passed: false,
                stage: matrixStage,
                name: error?.name,
                message: error?.message,
                stack: error?.stack,
            }, { status: 500 });
        }
    },
};
