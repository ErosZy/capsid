import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import {
    FrameworkWorker,
    decoder,
} from './protocol.mjs';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    args.set(process.argv[index], process.argv[index + 1]);
}

const driverPath = args.get('--driver');
const workerPath = args.get('--worker');
const bundlePath = args.get('--bundle');
if (!driverPath || !workerPath || !bundlePath) {
    throw new Error('expected --driver, --worker and --bundle');
}

let upstreamRequests = 0;
const server = createServer((request, response) => {
    upstreamRequests += 1;
    response.writeHead(200, {
        'content-type': 'text/plain',
        'x-h3-upstream': 'permission-probe',
    });
    response.end('permission-probe-ok');
});
await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
});
const address = server.address();
assert.ok(address && typeof address === 'object');
const target = `http://127.0.0.1:${address.port}/probe`;
const runtimeUrl = path => `https://compat.example${path}`;
const json = result => JSON.parse(decoder.decode(result.body));

const request = (worker, id, path) => worker.request({
    id,
    url: runtimeUrl(path),
});

const deniedWorker = new FrameworkWorker({
    driverPath,
    workerPath,
    bundlePath,
});
const allowedWorker = new FrameworkWorker({
    driverPath,
    workerPath,
    bundlePath,
    flags: [ '--loopback-port', String(address.port) ],
});

let phase = 'deny-by-default';
try {
    await deniedWorker.start();
    const denied = await request(
        deniedWorker,
        1,
        `/runtime/outbound-denied?url=${encodeURIComponent(target)}`,
    );
    assert.equal(denied.error, '', 'denied request runtime error');
    assert.equal(denied.status, 200, 'denied request H3 response status');
    const deniedBody = json(denied);
    assert.equal(deniedBody.allowed, false, 'network must be denied');
    assert.equal(deniedBody.name, 'TypeError', 'fetch rejection type');
    assert.match(
        deniedBody.message,
        /network request denied by egress policy/i,
        'fetch rejection classification',
    );
    assert.equal(
        upstreamRequests,
        0,
        'denied fetch must not reach the upstream server',
    );
    await deniedWorker.stop();

    phase = 'explicit loopback authorization';
    await allowedWorker.start();
    const allowed = await request(
        allowedWorker,
        2,
        `/runtime/outbound?url=${encodeURIComponent(target)}`,
    );
    assert.equal(allowed.error, '', 'allowed request runtime error');
    assert.equal(allowed.status, 200, 'allowed request status');
    assert.deepEqual(json(allowed), {
        status: 200,
        statusText: 'OK',
        contentType: 'text/plain',
        upstream: 'permission-probe',
        body: 'permission-probe-ok',
    });
    assert.equal(
        upstreamRequests,
        1,
        'authorized fetch must reach the upstream exactly once',
    );
    await allowedWorker.stop();
} catch (cause) {
    await deniedWorker.kill().catch(() => {});
    await allowedWorker.kill().catch(() => {});
    throw new Error(JSON.stringify({
        failure: cause?.message ?? String(cause),
        phase,
        deniedWorkerState: deniedWorker.lifecycleState,
        allowedWorkerState: allowedWorker.lifecycleState,
        upstreamRequests,
    }, null, 2), { cause });
} finally {
    await new Promise(resolve => server.close(resolve));
}

console.log('PASS: H3 v2 deny-by-default and explicit outbound permission');
