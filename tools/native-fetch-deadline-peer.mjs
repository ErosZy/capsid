import assert from 'node:assert/strict';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
import {nativeFetchDeadlinePlans, nativeFetchDeadlineReply} from './native-fetch-deadline-input.mjs';

export async function openNativeFetchDeadlineSource(plan) {
    assert.deepEqual(plan, nativeFetchDeadlinePlans.find(value => value.id === plan.id));
    const sockets = new Map(), data = {plan, records: [], failure: null, controlBeforeBody: false};
    const {head} = nativeFetchDeadlineReply(plan);
    const accept = socket => {
        socket.setNoDelay(true);
        const row = {role: null, rawRequest: '', request: null, sent: [], events: ['accepted'],
            peerEnded: false, peerReset: false, fixtureEnded: false, fixtureDestroyed: false};
        data.records.push(row);
        sockets.set(socket, row);
        let bytes = Buffer.alloc(0);
        socket.on('end', () => { row.peerEnded = true; row.events.push('peer-ended'); });
        socket.on('error', error => {
            if (error.code === 'ECONNRESET') row.peerReset = true;
            else row.error = error.code;
        });
        socket.on('close', () => sockets.delete(socket));
        socket.on('data', chunk => {
            bytes = Buffer.concat([bytes, chunk]);
            row.rawRequest = bytes.toString('base64');
            if (row.request !== null) return;
            const at = bytes.indexOf('\r\n\r\n');
            if (at < 0) return;
            row.request = bytes.subarray(0, at).toString();
            row.events.push('request');
            const target = new URL(row.request.split(' ')[1], 'http://localhost');
            if (target.pathname === '/hold') {
                row.role = 'source';
                if (plan.stage !== 'headless') {
                    row.sent.push(head.toString('base64'));
                    row.events.push('sent-head');
                    socket.write(head);
                }
            } else if (target.pathname === '/control') {
                row.role = 'control';
                const sources = data.records.filter(record => record.role === 'source');
                data.controlBeforeBody = sources.length === 1 && sources[0].sent.length === 1;
                try { data.phase = JSON.parse(target.searchParams.get('phase')); }
                catch (error) { data.failure = String(error); }
                const ack = Buffer.from('HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok');
                row.sent.push(ack.toString('base64'));
                row.fixtureEnded = true;
                row.events.push('sent-end');
                socket.end(ack);
            } else {
                data.failure = 'unknown path';
                socket.destroy();
            }
        });
    };
    const listeners = [net.createServer({allowHalfOpen: true}, accept), net.createServer({allowHalfOpen: true}, accept)];
    await new Promise((resolve, reject) => {
        listeners[0].once('error', reject);
        listeners[0].listen(0, '127.0.0.1', resolve);
    });
    data.port = listeners[0].address().port;
    try {
        await new Promise((resolve, reject) => {
            listeners[1].once('error', reject);
            listeners[1].listen({host: '::1', port: data.port, ipv6Only: true}, resolve);
        });
    } catch (error) {
        await new Promise(resolve => listeners[0].close(resolve));
        throw error;
    }
    return {data, target: `http://localhost:${data.port}/hold`, async terminal() {
        const end = performance.now() + 3000, count = plan.stage === 'headless' ? 1 : 2;
        while (performance.now() < end) {
            if (data.records.length === count && data.records.every(row => row.peerEnded || row.peerReset)) return true;
            await delay(5);
        }
        return false;
    }, async close() {
        for (const [socket, row] of sockets) { row.fixtureDestroyed = true; socket.destroy(); }
        await Promise.all(listeners.map(server => new Promise(resolve => server.close(resolve))));
    }};
}
