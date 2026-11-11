import net from 'node:net';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {nativeFetchIntrinsicPacket} from './native-fetch-intrinsic-input.mjs';

export async function openNativeFetchIntrinsicSource(plan) {
    const packet = nativeFetchIntrinsicPacket(plan), sockets = new Map(), data = {plan, records: []};
    const server = net.createServer({allowHalfOpen: true}, socket => {
        socket.setNoDelay(true); let input = Buffer.alloc(0);
        const row = {request: null, rawRequest: '', sentBytes: 0, sentSHA256: null,
            events: ['accepted'], peerEnded: false, peerReset: false, fixtureEnded: false, fixtureDestroyed: false};
        data.records.push(row); sockets.set(socket, row);
        socket.on('end', () => { row.peerEnded = true; row.events.push('peer-ended'); });
        socket.on('error', error => { if (error.code === 'ECONNRESET') row.peerReset = true; else row.error = error.code; });
        socket.on('close', () => sockets.delete(socket));
        socket.on('data', bytes => {
            input = Buffer.concat([input, bytes]); row.rawRequest = input.toString('base64');
            if (row.request !== null) return; const at = input.indexOf('\r\n\r\n'); if (at < 0) return;
            row.request = input.subarray(0, at).toString(); row.events.push('request');
            row.sentBytes = packet.length; row.sentSHA256 = createHash('sha256').update(packet).digest('hex');
            row.fixtureEnded = true; row.events.push('sent-end'); socket.end(packet);
        });
    });
    await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
    data.port = server.address().port;
    return {data, packet, target: `http://127.0.0.1:${data.port}/body`, base: `http://127.0.0.1:${data.port}`,
        async terminal() {
            const end = performance.now() + 3000;
            while (performance.now() < end) {
                if (data.records.length === 1 && data.records.every(row => row.peerEnded || row.peerReset)) return true;
                await delay(5);
            }
            return false;
        },
        async close() {
            for (const [socket, row] of sockets) { row.fixtureDestroyed = true; socket.destroy(); }
            await new Promise(resolve => server.close(resolve));
        }};
}
