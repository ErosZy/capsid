import assert from 'node:assert/strict';
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
const defaultBundle = args.get('--default-bundle');
const malformedBundle = args.get('--malformed-bundle');
const debugBundle = args.get('--debug-bundle');
if (
    !driverPath ||
    !workerPath ||
    !defaultBundle ||
    !malformedBundle ||
    !debugBundle
) {
    throw new Error(
        'expected --driver, --worker, --default-bundle, ' +
        '--malformed-bundle and --debug-bundle',
    );
}

const text = result => decoder.decode(result.body);
const json = result => JSON.parse(text(result));
const workers = [];
let phase = 'default malformed URL rejection';

const run = async (bundlePath, path) => {
    const worker = new FrameworkWorker({
        driverPath,
        workerPath,
        bundlePath,
    });
    workers.push(worker);
    await worker.start();
    const result = await worker.request({
        id: workers.length,
        url: `https://compat.example${path}`,
    });
    await worker.stop();
    return result;
};

try {
    const rejected = await run(defaultBundle, '/foo%');
    assert.equal(rejected.error, '', 'default malformed runtime error');
    assert.equal(rejected.status, 400, 'default malformed status');
    assert.equal(rejected.statusText, '');
    assert.deepEqual(json(rejected), {
        status: 400,
        message: 'Bad Request',
    });

    phase = 'allowMalformedURL opt-in';
    const allowed = await run(malformedBundle, '/foo%');
    assert.equal(allowed.error, '', 'allowed malformed runtime error');
    assert.equal(allowed.status, 200, 'allowed malformed status');
    assert.deepEqual(json(allowed), {
        rawUrl: 'https://compat.example/foo%',
        pathname: '/foo%',
        path: 'foo%',
    });

    phase = 'production error redaction';
    const production = await run(defaultBundle, '/errors/unhandled');
    assert.equal(production.error, '', 'production error runtime error');
    assert.equal(production.status, 500, 'production error status');
    assert.equal(production.statusText, '');
    assert.deepEqual(json(production), {
        status: 500,
        unhandled: true,
        message: 'HTTPError',
    });
    assert.doesNotMatch(
        text(production),
        /sensitive-unhandled-message|stack/i,
    );

    phase = 'explicit debug diagnostics';
    const debug = await run(debugBundle, '/error');
    assert.equal(debug.error, '', 'debug error runtime error');
    assert.equal(debug.status, 500, 'debug error status');
    assert.equal(debug.statusText, '');
    const debugBody = json(debug);
    assert.equal(debugBody.status, 500);
    assert.equal(debugBody.unhandled, true);
    assert.equal(debugBody.message, 'HTTPError');
    assert.ok(Array.isArray(debugBody.stack));
    assert.ok(
        debugBody.stack.some(frame =>
            frame.includes('framework-reference.js')),
        'debug mode must expose an application stack',
    );
} catch (cause) {
    await Promise.all(workers.map(worker => worker.kill().catch(() => {})));
    throw new Error(JSON.stringify({
        failure: cause?.message ?? String(cause),
        phase,
        workerStates: workers.map(worker => worker.lifecycleState),
    }, null, 2), { cause });
}

console.log(
    'PASS: H3 v2 malformed URL and production/debug mode semantics',
);
