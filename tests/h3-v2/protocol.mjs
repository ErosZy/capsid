import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';

export const encoder = new TextEncoder();
export const decoder = new TextDecoder();

export const hex = bytes => {
    let output = '';
    for (const byte of bytes) {
        output += byte.toString(16).padStart(2, '0');
    }
    return output || '-';
};

export const unhex = value => {
    if (value === '-') {
        return new Uint8Array();
    }
    assert.match(value, /^(?:[0-9a-f]{2})+$/i, 'driver hex');
    const output = new Uint8Array(value.length / 2);
    for (let index = 0; index < output.length; ++index) {
        output[index] = Number.parseInt(
            value.slice(index * 2, index * 2 + 2),
            16,
        );
    }
    return output;
};

const encodeU32 = value => [
    value & 0xff,
    (value >>> 8) & 0xff,
    (value >>> 16) & 0xff,
    (value >>> 24) & 0xff,
];

const decodeU32 = (bytes, offset) => {
    if (offset + 4 > bytes.length) {
        throw new Error('truncated driver header blob');
    }
    return (
        bytes[offset] |
        (bytes[offset + 1] << 8) |
        (bytes[offset + 2] << 16) |
        (bytes[offset + 3] << 24)
    ) >>> 0;
};

export const encodeHeaders = headers => {
    const chunks = [ encodeU32(headers.length) ];
    for (const [ name, value ] of headers) {
        const nameBytes = encoder.encode(name);
        const valueBytes = encoder.encode(value);
        chunks.push(
            encodeU32(nameBytes.length),
            [ ...nameBytes ],
            encodeU32(valueBytes.length),
            [ ...valueBytes ],
        );
    }
    return hex(new Uint8Array(chunks.flat()));
};

export const decodeHeaders = encoded => {
    const bytes = unhex(encoded);
    let offset = 0;
    const count = decodeU32(bytes, offset);
    offset += 4;
    const output = [];
    for (let index = 0; index < count; ++index) {
        const nameSize = decodeU32(bytes, offset);
        offset += 4;
        const name = decoder.decode(
            bytes.subarray(offset, offset + nameSize),
        );
        offset += nameSize;
        const valueSize = decodeU32(bytes, offset);
        offset += 4;
        const value = decoder.decode(
            bytes.subarray(offset, offset + valueSize),
        );
        offset += valueSize;
        output.push([ name, value ]);
    }
    if (offset !== bytes.length) {
        throw new Error('trailing data in driver header blob');
    }
    return output;
};

export const parseResult = (line, expectedId) => {
    const fields = line.split(' ');
    assert.equal(fields[0], 'RESULT', 'driver result marker');
    assert.equal(Number(fields[1]), expectedId, 'driver request id');
    return {
        status: Number(fields[2]),
        headers: decodeHeaders(fields[3]),
        body: unhex(fields[4]),
        error: decoder.decode(unhex(fields[5])),
        statusText: decoder.decode(unhex(fields[6])),
    };
};

export class FrameworkWorker {
    #child;
    #lines;
    #exitPromise;
    #state = 'created';

    constructor({
        driverPath,
        workerPath,
        bundlePath,
        flags = [],
    }) {
        this.#child = spawn(
            driverPath,
            [ workerPath, bundlePath, ...flags ],
            { stdio: [ 'pipe', 'pipe', 'inherit' ] },
        );
        this.#lines = createInterface({
            input: this.#child.stdout,
            crlfDelay: Infinity,
        })[Symbol.asyncIterator]();
        this.#exitPromise = new Promise((resolve, reject) => {
            this.#child.once('error', reject);
            this.#child.once('exit', resolve);
        });
    }

    get lifecycleState() {
        return this.#state;
    }

    async #nextLine() {
        const result = await this.#lines.next();
        if (result.done) {
            this.#state = 'exited';
            throw new Error('H3 framework worker driver exited unexpectedly');
        }
        if (result.value.startsWith('FATAL ')) {
            this.#state = 'fatal';
            throw new Error(decoder.decode(unhex(result.value.slice(6))));
        }
        return result.value;
    }

    #send(command) {
        this.#child.stdin.write(`${command}\n`);
    }

    async start() {
        assert.equal(await this.#nextLine(), 'READY', 'worker readiness');
        this.#state = 'ready';
    }

    async request({
        id,
        method = 'GET',
        url,
        headers = [],
        body = new Uint8Array(),
        chunkSize = 257,
    }) {
        this.#state = `request:${id}`;
        this.#send([
            'REQUEST',
            id,
            hex(encoder.encode(method)),
            hex(encoder.encode(url)),
            encodeHeaders(headers),
            hex(body),
            chunkSize,
        ].join(' '));
        const result = parseResult(await this.#nextLine(), id);
        this.#state = 'ready';
        return result;
    }

    async concurrent({
        firstId,
        firstUrl,
        secondId,
        secondUrl,
    }) {
        this.#state = `concurrent:${firstId},${secondId}`;
        this.#send([
            'CONCURRENT',
            firstId,
            hex(encoder.encode(firstUrl)),
            secondId,
            hex(encoder.encode(secondUrl)),
        ].join(' '));
        const first = parseResult(await this.#nextLine(), firstId);
        const second = parseResult(await this.#nextLine(), secondId);
        this.#state = 'ready';
        return [ first, second ];
    }

    async cancel({ id, url, mode = 'started' }) {
        this.#state = `cancel:${id}:${mode}`;
        this.#send([
            'CANCEL',
            id,
            hex(encoder.encode(url)),
            mode,
        ].join(' '));
        assert.equal(await this.#nextLine(), `CANCELED ${id}`);
        this.#state = 'ready';
    }

    async cancelUpload({ id, url }) {
        this.#state = `cancel-upload:${id}`;
        this.#send([
            'CANCEL_UPLOAD',
            id,
            hex(encoder.encode(url)),
        ].join(' '));
        assert.equal(await this.#nextLine(), `CANCELED_UPLOAD ${id}`);
        this.#state = 'ready';
    }

    async stop() {
        if (this.#state === 'exited') {
            return;
        }
        this.#state = 'stopping';
        this.#child.stdin.end('STOP\n');
        assert.equal(await this.#exitPromise, 0, 'driver exit');
        this.#state = 'exited';
    }

    async kill() {
        this.#child.kill('SIGKILL');
        await this.#exitPromise.catch(() => {});
        this.#state = 'exited';
    }
}
