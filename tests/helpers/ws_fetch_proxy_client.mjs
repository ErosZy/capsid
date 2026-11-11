import net from 'node:net';
import crypto from 'node:crypto';

function frame(opcode,payload) {
    payload=Buffer.from(payload);const mask=crypto.randomBytes(4),data=Buffer.from(payload);let head;
    if(payload.length<126)head=Buffer.from([0x80|opcode,0x80|payload.length]);
    else if(payload.length<=65535){head=Buffer.alloc(4);head[0]=0x80|opcode;head[1]=0xfe;head.writeUInt16BE(payload.length,2);}
    else{head=Buffer.alloc(10);head[0]=0x80|opcode;head[1]=0xff;head.writeBigUInt64BE(BigInt(payload.length),2);}
    for(let index=0;index<data.length;++index)data[index]^=mask[index%4];return Buffer.concat([head,mask,data]);
}

// Keep a snapshot of the peer observation before local teardown. A later close
// caused by our own socket.destroy() must never change a returned observation.
export function fetchProxySession(port, target,
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
    const textQueue = [], textWaiters = [], binaryQueue = [], binaryWaiters = [];
    // Preserve an immutable snapshot before any local disposal.
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
        for(const waiter of binaryWaiters.splice(0))waiter.reject(error??new Error('peer ended before binary data'));
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
            while(input.length-parsed>=2){
                const first=input[parsed],second=input[parsed+1],tag=second&127;let length=tag,header=2;
                if(first&0x70||second&0x80||!(first&0x80))throw new Error('invalid native proxy frame bits');
                if(tag===126){if(input.length-parsed<4)break;length=input.readUInt16BE(parsed+2);header=4;if(length<126)throw new Error('noncanonical 16-bit length');}
                if(tag===127){if(input.length-parsed<10)break;const wide=input.readBigUInt64BE(parsed+2);if(wide>1048576n||wide<65536n)throw new Error('invalid 64-bit frame length');length=Number(wide);header=10;}
                if(input.length-parsed<header+length)break;
                const opcode=first&15,payload=input.subarray(parsed+header,parsed+header+length);parsed+=header+length;
                record.frames.push({opcode,payload:payload.toString('base64')});
                if(opcode===1){const text=payload.toString();if(textWaiters.length)textWaiters.shift().resolve(text);else textQueue.push(text);}
                else if(opcode===2){const bytes=Buffer.from(payload);if(binaryWaiters.length)binaryWaiters.shift().resolve(bytes);else binaryQueue.push(bytes);}
                else if(opcode===8&&!closeSent&&autoCloseReply){closeSent=true;closeReplySent=true;socket.write(frame(8,payload));}
                else if(opcode===9)socket.write(frame(10,payload));
                else if(opcode!==8&&opcode!==10)throw new Error('unsupported native proxy opcode');
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
        sendBinary(bytes) {socket.write(frame(2,bytes));},
        binary(){if(binaryQueue.length)return Promise.resolve(binaryQueue.shift());if(finished)return Promise.reject(new Error('no binary message'));return new Promise((resolve,reject)=>binaryWaiters.push({resolve,reject}));},
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
