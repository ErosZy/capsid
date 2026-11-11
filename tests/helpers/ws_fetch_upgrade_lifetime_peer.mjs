import assert from 'node:assert/strict';
import net from 'node:net';
import {createHash} from 'node:crypto';

// A local raw peer keeps the handshake/body open until the driver acts.
// Every EOF and frame is captured before cleanup can close any connection.
export async function fetchUpgradeLifetimePeer(kind, challenge, role) {
    const records = [], sockets = new Set(); let acceptPending, sendPending, httpPending;
    const listener = net.createServer({allowHalfOpen: true}, socket => {
        sockets.add(socket);
        const item = {localAddress: socket.localAddress, localPort: socket.localPort, request: null, rawRequest: null,
            responseStatus: null, frames: [], events: [], peerEnded: false, peerReset: false,
            fixtureEnded: false, fixtureDestroyed: false, error: null};
        records.push(item); let bytes = Buffer.alloc(0), requested = false, upgraded = false, message;
        socket.on('close', () => sockets.delete(socket));
        socket.on('end', () => {item.peerEnded = true; item.events.push('peer-ended'); socket.end();});
        socket.on('error', error => {if (error.code === 'ECONNRESET') {item.peerReset = true; item.events.push('peer-reset');} else item.error = error.code;});
        function frame(opcode, payload) {
            assert.ok(payload.length < 126, 'bounded lifetime probe frame');
            socket.write(Buffer.concat([Buffer.from([0x80 | opcode, payload.length]), payload]));
        }
        function parseFrames() {
            while (bytes.length >= 2) {
                const opcode = bytes[0] & 15, fin = Boolean(bytes[0] & 128), masked = Boolean(bytes[1] & 128);
                assert.equal(bytes[0] & 0x70, 0); assert.equal(masked, true);
                const length = bytes[1] & 127; assert.ok(length < 126, 'bounded lifetime client frame');
                if (bytes.length < 6 + length) return;
                const mask = bytes.subarray(2, 6), payload = Buffer.from(bytes.subarray(6, 6 + length));
                for (let index = 0; index < payload.length; index++) payload[index] ^= mask[index % 4];
                bytes = bytes.subarray(6 + length);
                item.frames.push({opcode, fin, masked, mask: mask.toString('hex'), payload: payload.toString('base64')});
                if (opcode === 8) {
                    assert.ok(fin && payload.length <= 125); frame(8, payload);
                    item.fixtureEnded = true; item.events.push('close-reply'); socket.end();
                } else if (opcode === 9) {assert.equal(fin, true); frame(10, payload);}
                else {
                    assert.ok([0, 1, 2].includes(opcode));
                    if (opcode) {assert.equal(message, undefined); message = {opcode, chunks: []};}
                    assert.ok(message); message.chunks.push(payload);
                    if (fin) {frame(message.opcode, Buffer.concat(message.chunks)); message = undefined;}
                }
            }
        }
        socket.on('data', chunk => {
            try {
                bytes = Buffer.concat([bytes, chunk]);
                if (!requested) {
                    const end = bytes.indexOf('\r\n\r\n'); if (end < 0) return;
                    requested = true; item.request = bytes.subarray(0, end).toString('latin1');
                    item.rawRequest = bytes.subarray(0, end + 4).toString('base64'); bytes = bytes.subarray(end + 4); item.events.push('request');
                    const lines = item.request.split('\r\n');
                    const headers = lines.slice(1).map(line => {const colon = line.indexOf(':'); assert.ok(colon > 0); return [line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim()];});
                    const get = name => {const values = headers.filter(([key]) => key === name); assert.equal(values.length, 1); return values[0][1];};
                    assert.equal(lines[0], `GET /hold?challenge=${challenge}&role=${role} HTTP/1.1`);
                    assert.equal(get('upgrade').toLowerCase(), 'websocket'); assert.equal(get('connection').toLowerCase(), 'upgrade');
                    assert.equal(get('sec-websocket-version'), '13'); const key = get('sec-websocket-key'); assert.match(key, /^[A-Za-z0-9+/]{22}==$/);
                    assert.equal(Buffer.from(key, 'base64').toString('base64'), key);
                    const upgrade = () => {
                        assert.equal(upgraded, false); upgraded = true; item.responseStatus = 101; item.events.push('upgrade');
                        const accept = createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
                        socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\nX-Probe: ${challenge}\r\n\r\n`);
                        sendPending = () => {assert.equal(item.peerEnded, false); item.events.push('send-after-parent'); frame(1, Buffer.from('after/' + challenge + '/' + role));};
                        parseFrames();
                    };
                    if (kind === 'pending') {
                        item.events.push('response-held'); acceptPending = upgrade;
                        httpPending = () => {
                            assert.equal(item.peerEnded, false); assert.equal(item.peerReset, false); assert.equal(item.responseStatus, null);
                            const body = 'complete/' + challenge + '/' + role;
                            item.responseStatus = 409; item.fixtureEnded = true; item.events.push('response-complete');
                            socket.end(`HTTP/1.1 409 Conflict\r\nContent-Length: ${Buffer.byteLength(body)}\r\nConnection: close\r\nX-Probe: ${challenge}\r\n\r\n${body}`);
                        };
                    }
                    else if (kind === 'body') {
                        item.responseStatus = 409;
                        const first = Buffer.from('first/' + challenge + '/' + role);
                        socket.write(`HTTP/1.1 409 Conflict\r\nTransfer-Encoding: chunked\r\nConnection: close\r\nX-Probe: ${challenge}\r\n\r\n` +
                            first.length.toString(16) + '\r\n' + first.toString() + '\r\n'); item.events.push('first-body');
                    } else {assert.equal(kind, 'socket'); upgrade();}
                } else if (upgraded) parseFrames();
                else assert.equal(bytes.length, 0, 'no request body or unaccepted client data');
            } catch (error) {item.error = error.stack; item.fixtureDestroyed = true; socket.destroy();}
        });
    });
    await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
    return {port: listener.address().port, snapshot: () => structuredClone(records),
        accept: () => {assert.equal(typeof acceptPending, 'function'); const accept = acceptPending; acceptPending = null; accept();},
        respondHttp: () => {assert.equal(typeof httpPending, 'function'); const respond = httpPending; httpPending = acceptPending = null; respond();},
        sendAfterParent: () => {assert.equal(typeof sendPending, 'function'); sendPending();},
        stop: async () => {for (const socket of sockets) socket.destroy(); await new Promise(resolve => listener.close(resolve));}};
}
