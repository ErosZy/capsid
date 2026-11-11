import assert from 'node:assert/strict';
import net from 'node:net';
import {createHash} from 'node:crypto';

export async function fetchUpgradePeer(kind, challenge) {
    const records = [], sockets = new Set(); let releaseBody;
    const listener = net.createServer({allowHalfOpen: true}, socket => {
        sockets.add(socket);
        const item = {localAddress: socket.localAddress, localPort: socket.localPort, request: null, rawRequest: null,
            frames: [], peerEnded: false, peerReset: false, fixtureEnded: false, fixtureDestroyed: false,
            responseStatus: null, events: [], error: null};
        records.push(item); let bytes = Buffer.alloc(0), upgraded = false, message = null;
        socket.on('close', () => sockets.delete(socket));
        socket.on('end', () => {item.peerEnded = true; item.events.push('peer-ended'); socket.end();});
        socket.on('error', error => {if (error.code === 'ECONNRESET') item.peerReset = true; else item.error = error.code;});
        const endHttp = (status, reason, headers, body) => {
            item.responseStatus = status; item.fixtureEnded = true; item.events.push('response-complete');
            socket.end(Buffer.concat([Buffer.from(`HTTP/1.1 ${status} ${reason}\r\n${headers}Content-Length: ${body.length}\r\nConnection: close\r\n\r\n`, 'latin1'), body]));
        };
        const sendFrame = (opcode, payload) => {
            let head;
            if (payload.length < 126) head = Buffer.from([0x80 | opcode, payload.length]);
            else if (payload.length <= 65535) {head = Buffer.alloc(4); head[0] = 0x80 | opcode; head[1] = 126; head.writeUInt16BE(payload.length, 2);}
            else {head = Buffer.alloc(10); head[0] = 0x80 | opcode; head[1] = 127; head.writeBigUInt64BE(BigInt(payload.length), 2);}
            socket.write(Buffer.concat([head, payload]));
        };
        const parseFrames = () => {
            while (bytes.length >= 2) {
                const opcode = bytes[0] & 15, fin = !!(bytes[0] & 128), masked = !!(bytes[1] & 128);
                assert.equal(bytes[0] & 0x70, 0, 'no unnegotiated RSV bits'); assert.equal(masked, true, 'actual client masking');
                let length = bytes[1] & 127, offset = 2;
                if (length === 126) {if (bytes.length < 4) return; length = bytes.readUInt16BE(2); offset = 4; assert.ok(length >= 126);}
                else if (length === 127) {if (bytes.length < 10) return; const wide = bytes.readBigUInt64BE(2); assert.ok(wide <= 1024n * 1024n); length = Number(wide); offset = 10; assert.ok(length > 65535);}
                assert.ok(length <= 1024 * 1024); if (bytes.length < offset + 4 + length) return;
                const mask = bytes.subarray(offset, offset + 4), payload = Buffer.from(bytes.subarray(offset + 4, offset + 4 + length));
                for (let index = 0; index < payload.length; ++index) payload[index] ^= mask[index % 4];
                bytes = bytes.subarray(offset + 4 + length);
                item.frames.push({opcode, fin, masked, mask: mask.toString('hex'), payload: payload.toString('base64')});
                if (opcode === 8) {
                    assert.ok(fin && payload.length <= 125); sendFrame(8, payload);
                    item.fixtureEnded = true; item.events.push('close-reply'); socket.end();
                } else if (opcode === 9) {assert.ok(fin && payload.length <= 125); sendFrame(10, payload);}
                else {
                    assert.ok(opcode === 0 || opcode === 1 || opcode === 2, 'supported actual data opcode');
                    if (opcode !== 0) {assert.equal(message, null); message = {opcode, chunks: [], bytes: 0};}
                    assert.ok(message); message.chunks.push(payload); message.bytes += payload.length; assert.ok(message.bytes <= 1024 * 1024);
                    if (fin) {sendFrame(message.opcode, Buffer.concat(message.chunks)); message = null;}
                }
            }
        };
        socket.on('data', chunk => {
            try {
                bytes = Buffer.concat([bytes, chunk]);
                if (!upgraded) {
                    const end = bytes.indexOf('\r\n\r\n'); if (end < 0) return;
                    assert.equal(item.request, null, 'one request per observed connection');
                    item.request = bytes.subarray(0, end).toString('latin1'); item.rawRequest = bytes.subarray(0, end + 4).toString('base64');
                    item.events.push('request'); const lines = item.request.split('\r\n');
                    const headers = new Map(lines.slice(1).map(line => {const at = line.indexOf(':'); assert.ok(at > 0); return [line.slice(0, at).toLowerCase(), line.slice(at + 1).trim()];}));
                    bytes = bytes.subarray(end + 4);
                    const valid = lines[0].startsWith('GET ') && headers.get('upgrade')?.toLowerCase() === 'websocket' &&
                        headers.get('connection')?.toLowerCase().split(/\s*,\s*/).includes('upgrade') &&
                        headers.get('sec-websocket-version') === '13' && /^[A-Za-z0-9+/]{22}==$/.test(headers.get('sec-websocket-key') ?? '');
                    if (!valid) {endHttp(400, 'Bad Request', '', Buffer.from('Invalid WebSocket handshake')); return;}
                    if (kind === 'upgrade-abort-pending') {item.events.push('response-held'); return;}
                    const extra = `X-Probe: ${challenge}\r\nSet-Cookie: first=1\r\nSet-Cookie: second=2\r\n`;
                    const redirected = lines[0].startsWith('GET /redirected?');
                    if (kind === 'upgrade-non101' || kind === 'upgrade-non101-stream' || kind.startsWith('upgrade-redirect') && !redirected) {
                        if (kind === 'upgrade-non101-stream') {
                            const body = Buffer.from(Array.from({length: 180000}, (_, index) => index % 251));
                            item.responseStatus = 409; socket.write(`HTTP/1.1 409 Conflict\r\n${extra}Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n`);
                            const sendPart = part => {socket.write(part.length.toString(16) + '\r\n'); socket.write(part); socket.write('\r\n');};
                            sendPart(body.subarray(0, 8192)); item.events.push('first-body');
                            releaseBody = () => {assert.equal(item.fixtureEnded, false); for (let at = 8192; at < body.length; at += 8192) sendPart(body.subarray(at, at + 8192));
                                item.fixtureEnded = true; item.events.push('response-complete'); socket.end('0\r\n\r\n');};
                        } else {
                            const redirect = kind.startsWith('upgrade-redirect');
                            endHttp(redirect ? 302 : 409, redirect ? 'Found' : 'Conflict', extra + (redirect ? `Location: /redirected?challenge=${challenge}\r\n` : ''), Buffer.from('body/' + challenge));
                        }
                        return;
                    }
                    const accept = kind === 'upgrade-invalid-accept' ? 'invalid'
                        : createHash('sha1').update(headers.get('sec-websocket-key') + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
                    item.responseStatus = 101;
                    socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n${extra}` +
                        (kind === 'upgrade-custom-headers' || kind === 'upgrade-server-protocol' ? 'Sec-WebSocket-Protocol: chat\r\n' : '') +
                        (kind === 'upgrade-server-extension' ? 'Sec-WebSocket-Extensions: permessage-deflate\r\n' : '') + '\r\n');
                    item.events.push('upgrade'); upgraded = true;
                }
                if (upgraded) parseFrames();
            } catch (error) {item.error = error.stack; item.fixtureDestroyed = true; socket.destroy();}
        });
    });
    await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
    return {port: listener.address().port, snapshot: () => structuredClone(records),
        release: () => {assert.equal(typeof releaseBody, 'function', 'body must reach release checkpoint'); releaseBody();},
        stop: async () => {for (const socket of sockets) socket.destroy(); await new Promise(resolve => listener.close(resolve));}};
}
