import assert from 'node:assert/strict';
import test from 'node:test';
import {spawn} from 'node:child_process';
import {withCleanup, stopCapsid} from './helpers/ws_host_backend.mjs';

test('scenario result is returned after cleanup', async () => {
    const order = [];
    const result = await withCleanup(async () => { order.push('scenario'); return 42; },
        async () => { order.push('cleanup'); });
    assert.equal(result, 42); assert.deepEqual(order, ['scenario', 'cleanup']);
});
test('scenario failure survives successful cleanup', async () => {
    const primary = new Error('independent scenario failure');
    let cleaned = false;
    await assert.rejects(withCleanup(async () => { throw primary; }, async () => { cleaned = true; }),
        error => error === primary);
    assert.equal(cleaned, true);
});
test('cleanup failure fails a healthy scenario', async () => {
    const failure = new Error('independent teardown failure');
    await assert.rejects(withCleanup(async () => 42, async () => { throw failure; }), error => error === failure);
});
test('both scenario and shutdown failure remain observable', async () => {
    const primary = new Error('echo mismatch'), teardown = new Error('Host exited by signal');
    await assert.rejects(withCleanup(async () => { throw primary; }, async () => { throw teardown; }), error => {
        assert.ok(error instanceof AggregateError);
        assert.equal(error.cause, primary);
        assert.deepEqual(error.errors, [primary, teardown]);
        assert.match(error.message, /echo mismatch.*Host exited by signal/);
        return true;
    });
});
test('concurrent stop callers observe the same actual process terminal', async () => {
    // A real Node child is a fallback process, never a Capsid success fixture.
    const child = spawn(process.execPath, ['-e', 'process.stdout.write("ready\\n"); setInterval(()=>{},1000)'],
        {stdio: ['ignore', 'pipe', 'pipe']});
    let streamsClosed = false;
    child.once('close', () => { streamsClosed = true; });
    try {
        await new Promise((resolve, reject) => { child.stdout.once('data', resolve); child.once('error', reject); });
        const first = stopCapsid(child), second = stopCapsid(child);
        assert.equal(first, second);
        const terminal = await first;
        assert.equal(streamsClosed, true, 'stop waits for child stdio close, before the driver may exit');
        assert.deepEqual(terminal, {code: child.exitCode, signal: child.signalCode, forced: false});
        assert.ok(terminal.code !== null || terminal.signal !== null);
        assert.equal(await stopCapsid(child), terminal);
    } finally {
        if (child.exitCode === null && child.signalCode === null) {
            child.kill('SIGKILL'); await new Promise(resolve => child.once('exit', resolve));
        }
    }
});
