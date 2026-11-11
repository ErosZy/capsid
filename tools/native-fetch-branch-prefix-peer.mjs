import net from 'node:net';
import {gzipSync} from 'node:zlib';
import {setTimeout as delay} from 'node:timers/promises';
export const plans = ['tee', 'clone'].flatMap(graph => ['wide', 'full'].flatMap(method =>
    ['continue', 'cancel0', 'cancel1', 'all', 'abort', 'cancel0abort', 'cancel1abort', 'eof'].map(action =>
        ({id: `raw-${graph}-${method}-${action}`, kind: 'raw', graph, method, action, api: `${graph}-${method}-${action}`}))));
const payload = Buffer.alloc(65573); let seed = 0x12345678;
for (let i = 0; i < payload.length; ++i) {seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; payload[i] = seed >>> 24;}
export async function openLargeSource(plan) {
    const body = payload;
    const head = Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n' +
        `X-Branch-Case: ${plan.id}\r\n` + (plan.action === 'eof' ? '' : `Content-Length: ${body.length}\r\n`) + '\r\n');
    const packet = Buffer.concat([head, body]);
    const data = {plan, records: [], phases: [], bodyReleased: false, prefixReleased: false, failure: null}, sockets = new Map(); let source;
    const server = net.createServer({allowHalfOpen: true}, socket => {
        socket.setNoDelay(true); let input = Buffer.alloc(0);
        const row = {role: null, request: null, rawRequest: '', sent: [], events: ['accepted'],
            peerEnded: false, peerReset: false, fixtureEnded: false, fixtureDestroyed: false};
        data.records.push(row); sockets.set(socket, row);
        socket.on('end', () => {row.peerEnded = true; row.events.push('peer-ended');});
        socket.on('error', error => {if (error.code === 'ECONNRESET') row.peerReset = true; else row.error = error.code;});
        socket.on('close', () => sockets.delete(socket));
        socket.on('data', bytes => {
            input = Buffer.concat([input, bytes]); row.rawRequest = input.toString('base64');
            if (row.request !== null) return;
            const at = input.indexOf('\r\n\r\n'); if (at < 0) return;
            row.request = input.subarray(0, at).toString(); row.events.push('request');
            const url = new URL(row.request.split(' ')[1], 'http://127.0.0.1');
            const send = (value, end = false) => {
                row.sent.push(value.toString('base64')); row.events.push(end ? 'sent-end' : 'sent-head');
                if (end) {row.fixtureEnded = true; socket.end(value);} else socket.write(value);
            };
            if (url.pathname === '/body') {
                row.role = 'source'; if (source) {data.failure = 'duplicate source'; socket.destroy(); return;}
                source = {row, send}; send(head);
            } else if (url.pathname === '/control') {
                row.role = 'control';
                const phase = url.searchParams.get('phase');
                let phaseData; try {phaseData = JSON.parse(url.searchParams.get('data'));} catch (error) {data.failure = String(error);}
                data.phases.push({phase, data: phaseData, sourcePresent: Boolean(source), bodyReleasedBefore: data.bodyReleased,
                    sourceEndedBefore: source?.row.peerEnded ?? false, sourceResetBefore: source?.row.peerReset ?? false});
                if (phase === 'prepared' && source) {data.prefixReleased = true; source.send(body.subarray(0, 137));}
                send(Buffer.from('HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok'), true);
                if (phase === 'after-action' && ['continue', 'cancel0', 'cancel1', 'eof'].includes(plan.action) && source) {
                    data.bodyReleased = true; source.send(plan.action === 'eof' ? Buffer.alloc(0) : body.subarray(137), true);
                }
            } else {data.failure = 'unknown path'; socket.destroy();}
        });
    });
    await new Promise((resolve, reject) => {server.once('error', reject); server.listen(0, '127.0.0.1', resolve);}); data.port = server.address().port;
    return {data, packet, target: `http://127.0.0.1:${data.port}/body`, base: `http://127.0.0.1:${data.port}`,
        async terminal() {const end = performance.now() + 3000;
            while (performance.now() < end) {if (data.records.length === 6 && data.records.every(row => row.peerEnded || row.peerReset)) return true; await delay(5);} return false;},
        async close() {for (const [socket, row] of sockets) {row.fixtureDestroyed = true; socket.destroy();}
            await new Promise(resolve => server.close(resolve));}};
}
