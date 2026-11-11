import crypto from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {responseSession} from './ws_response_client.mjs';
import {handlerErrorCaseIds, handlerErrorReferenceBoundary, assertHandlerErrorCase, assertHandlerErrorInventory} from '../../tools/workerd-handler-error-contract.mjs';

export async function observeHandlerErrors(port, prefix, check, {includeReferenceBoundary = false} = {}) {
    const observations = {};
    for (const id of [...handlerErrorCaseIds, ...(includeReferenceBoundary ? [handlerErrorReferenceBoundary] : [])])
        observations[id] = await observeHandlerErrorCase(port, prefix, check, id);
    assertHandlerErrorInventory(observations, check, includeReferenceBoundary);
    return observations;
}

// Used by isolated fault drivers. The ordinary driver always calls the full
// inventory above; this entry validates the same wire/state/error contract.
export async function observeHandlerErrorCase(port, prefix, check, id) {
    if (!handlerErrorCaseIds.includes(id) && id !== handlerErrorReferenceBoundary)
        throw new TypeError('Unknown handler error case');
    const [scenario, variant] = id.split('/');
    const retainedTransport = scenario === 'c17' && variant === 'error-listener-throw';
    const query = new URLSearchParams({case: scenario,
        kind: variant === 'error-listener-throw' ? 'TypeError' : variant,
        rethrow: variant === 'error-listener-throw' ? '1' : '0'});
    const peer = responseSession(port, prefix + '/handler-error?' + query,
        {autoCloseReply: false, observeCloseReply: true});
    const headers = await peer.headers;
    check.equal(headers.status, 101, id + ': actual upgrade');
    const accept = crypto.createHash('sha1').update(headers.requestKey +
        '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    check.equal(/^sec-websocket-accept:\s*(\S+)\s*$/im.exec(headers.rawHeader)?.[1], accept, id + ': actual handshake');
    peer.send('go');
    const started = performance.now();
    let raw;
    if (!retainedTransport)
        raw = await peer.complete.catch(error => { error.message = id + ': ' + error.message; throw error; });
    const readState = async () => {
        const response = await fetch(`http://127.0.0.1:${port}${prefix}/handler-error-state`,
            {signal: AbortSignal.timeout(4000)});
        if (response.status !== 200) throw new Error(id + ': HTTP recovery ' + response.status);
        return response.json();
    };
    const deadline = performance.now() + 4000;
    let events;
    for (;;) {
        events = await readState();
        if (events.some(event => event.type === 'error')) break;
        if (performance.now() >= deadline) throw new Error(id + ': handler error event absent: ' + JSON.stringify(events));
        await delay(5);
    }
    await delay(20);
    check.deepEqual(await readState(), events, id + ': exactly one stable error event');
    let observedOpenMs;
    if (retainedTransport) {
        await delay(100);
        observedOpenMs = performance.now() - started;
        peer.dispose();
        try { raw = await peer.complete; }
        catch (error) { if (!error.observation) throw error; raw = error.observation; }
    }
    const observation = {events, wire: {hex: Buffer.from(raw.rawTail, 'base64').toString('hex'),
        frames: raw.frames, peerTerminated: raw.peerTerminated, completion: raw.completion,
        transportError: raw.transportError, closeReplySent: raw.closeReplySent,
        ...(retainedTransport ? {observedOpenMs} : {})}};
    assertHandlerErrorCase(id, observation, check);
    return observation;
}
