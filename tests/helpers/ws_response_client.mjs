import net from 'node:net';
import crypto from 'node:crypto';

function frame(opcode, payload) {
    payload = Buffer.from(payload);
    if (payload.length >= 126) throw new Error('response fixture sends only short control/text messages');
    const mask = crypto.randomBytes(4), data = Buffer.from(payload);
    for (let i = 0; i < data.length; ++i) data[i] ^= mask[i % 4];
    return Buffer.concat([Buffer.from([0x80 | opcode, 0x80 | data.length]), mask, data]);
}

// Keep a snapshot of the peer observation before local teardown. A later close
// caused by our own socket.destroy() must never change a returned observation.
export function responseSession(port, target,
    {timeoutMs = 4000, protocols = 'chat, super', autoCloseReply = true, observeCloseReply = false} = {}) {
    const socket = net.connect(port, '127.0.0.1');
    const requestKey = crypto.randomBytes(16).toString('base64');
    const record = {status:null, rawHeader:null, requestKey, rawTail:'', frames:[],
        peerTerminated:false, transportError:null, completion:null};
    let input = Buffer.alloc(0), bodyStart = 0, parsed = 0, finished = false, closeSent = false, closeReplySent = false;
    let resolveHeaders, rejectHeaders, resolveComplete, rejectComplete;
    const headers = new Promise((resolve,reject) => { resolveHeaders=resolve; rejectHeaders=reject; });
    const complete = new Promise((resolve,reject) => { resolveComplete=resolve; rejectComplete=reject; });
    // Callers can await headers and completion in either order. Preserve the
    // original rejecting promises while observing early failures immediately.
    headers.catch(()=>{}); complete.catch(()=>{});
    const textQueue = [], textWaiters = [];
    // Keep the established Response report shape. Only the dedicated throw
    // regression requests the additional, immutable Close-reply observation.
    const snapshot = () => structuredClone({...record, rawTail:input.subarray(bodyStart).toString('base64'),
        ...(observeCloseReply ? {closeReplySent} : {})});
    const finish = (error, completion) => {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        record.completion = completion;
        const observed = snapshot();
        if (error) {
            error.observation = observed;
            rejectHeaders(error); rejectComplete(error);
        } else resolveComplete(observed);
        for (const waiter of textWaiters.splice(0)) waiter.reject(error ?? new Error('peer ended before a fixture message'));
        socket.destroy();
    };
    const timer = setTimeout(() => finish(new Error('Response peer terminal deadline'), 'deadline'), timeoutMs);
    socket.on('connect', () => socket.write(
        `GET ${target} HTTP/1.1\r\nHost: public.example\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n` +
        `Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${requestKey}\r\nSec-WebSocket-Protocol: ${protocols}\r\n\r\n`));
    socket.on('data', chunk => {
        if (finished) return;
        try {
            input = Buffer.concat([input,chunk]);
            if (record.rawHeader === null) {
                const end = input.indexOf('\r\n\r\n');
                if (end < 0) {
                    if (input.length > 32768) throw new Error('Response headers exceed fixture bound');
                    return;
                }
                record.rawHeader = input.subarray(0,end).toString();
                const status = /^HTTP\/1\.1 (\d{3}) /.exec(record.rawHeader);
                if (!status) throw new Error('Response has no complete HTTP/1.1 status line');
                record.status = Number(status[1]); bodyStart = parsed = end+4;
                resolveHeaders(snapshot());
                if (record.status !== 101) { finish(null, 'http-status'); return; }
            }
            while (input.length - parsed >= 2) {
                const first = input[parsed], second = input[parsed+1], length = second & 127;
                if (first & 0x70 || second & 0x80 || !(first & 0x80) || length >= 126)
                    throw new Error('Response fixture received an invalid or unexpected frame encoding');
                if (input.length - parsed < length+2) break;
                const opcode = first & 15;
                const payload = input.subarray(parsed+2,parsed+2+length); parsed += length+2;
                record.frames.push({opcode,payload:payload.toString('base64')});
                if (opcode === 1) {
                    const data = payload.toString();
                    if (textWaiters.length) textWaiters.shift().resolve(data); else textQueue.push(data);
                } else if (opcode === 8 && !closeSent && autoCloseReply) {
                    closeSent = true; closeReplySent = true; socket.write(frame(8,payload));
                }
            }
        } catch (error) { finish(error,'invalid-wire'); }
    });
    socket.on('end', () => {
        if (finished) return;
        record.peerTerminated = true;
        if (record.rawHeader === null) finish(new Error('peer ended before complete Response headers'),'peer-ended');
        else finish(null,'peer-ended');
    });
    socket.on('error', error => {
        if (finished) return;
        record.peerTerminated = true; record.transportError = error.code ?? error.name;
        if (record.status === 101 && error.code === 'ECONNRESET') finish(null,'peer-reset');
        else finish(error,'peer-error');
    });
    socket.on('close', () => {
        if (finished) return;
        record.peerTerminated = true;
        finish(new Error('peer closed without a complete terminal observation'),'peer-closed');
    });
    return {
        headers, complete,
        snapshot,
        send(text) { socket.write(frame(1,Buffer.from(text))); },
        close() {
            if (!closeSent) { closeSent = true; socket.write(frame(8,Buffer.from([3,232,...Buffer.from('done')]))); }
        },
        text() {
            if (textQueue.length) return Promise.resolve(textQueue.shift());
            if (finished) return Promise.reject(new Error('Response fixture has no unread text message'));
            return new Promise((resolve,reject) => textWaiters.push({resolve,reject}));
        },
        dispose() { finish(new Error('Response fixture disposed before peer termination'),'client-disposed'); },
    };
}
