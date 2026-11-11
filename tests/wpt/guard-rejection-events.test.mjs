import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { test } from 'node:test';
import { guardRejectionEvents, guardedRejectionTests } from './guard-rejection-events.mjs';

assert.ok(process.env.CAPSID_WPT_ROOT, 'controls require the locked WPT checkout');
const relative = 'html/webappapis/scripting/processing-model-2/unhandled-promise-rejections/support/promise-rejection-events.js';
const original = fs.readFileSync(path.join(process.env.CAPSID_WPT_ROOT, relative), 'utf8');
const adapter = fs.readFileSync(new URL('./adapter.js', import.meta.url), 'utf8');
const diagnostic = 'required rejectionhandled callback count';

async function execute(source, delivery = 'normal') {
    let nextTimer = 0;
    let now = 0;
    const timers = new Map();
    const listeners = new Map();
    const setTimer = (callback, delay) => {
        const id = ++nextTimer;
        timers.set(id, { callback, time: now + Number(delay) });
        return id;
    };
    const dispatch = (type, event) => {
        for (const listener of [...(listeners.get(type) ?? [])]) listener(event);
    };
    class ControlledPromise {
        constructor(executor) {
            this.reported = false;
            this.handled = false;
            const reject = reason => {
                this.reason = reason;
                setTimer(() => {
                    if (!this.handled) {
                        this.reported = true;
                        dispatch('unhandledrejection', { promise: this, reason });
                    }
                }, 0);
            };
            try { executor(() => {}, reject); } catch (error) { reject(error); }
        }
        then(_fulfilled, rejected) {
            this.handled = true;
            if (this.reported && delivery !== 'missing') {
                setTimer(() => {
                    const event = {
                        promise: delivery === 'unrelated' ? {} : this,
                        reason: delivery === 'workerd-reason' ? undefined : this.reason,
                    };
                    dispatch('rejectionhandled', event);
                    if (delivery === 'duplicate') dispatch('rejectionhandled', event);
                }, delivery === 'late' ? 30 : 0);
            }
            setTimer(() => rejected(this.reason), 0);
        }
        static reject(reason) { return new this((_resolve, reject) => reject(reason)); }
    }
    const context = vm.createContext({
        setTimeout: setTimer,
        clearTimeout: id => timers.delete(id),
        Blob: class {},
        DOMException,
        createImageBitmap: () => ControlledPromise.reject(new DOMException('', 'InvalidStateError')),
        addEventListener(type, listener) {
            if (!listeners.has(type)) listeners.set(type, new Set());
            listeners.get(type).add(listener);
        },
        removeEventListener(type, listener) { listeners.get(type)?.delete(listener); },
    });
    vm.runInContext('globalThis.self = globalThis;', context);
    vm.runInContext(adapter, context);
    context.Promise = ControlledPromise;
    // Select two unchanged locked upstream bodies. The controllable event
    // source proves the guard independently of Capsid's current runtime result.
    context.names = [...guardedRejectionTests];
    vm.runInContext(`
        const originalAsyncTest = async_test;
        globalThis.async_test = (callback, name) => names.includes(name)
            ? originalAsyncTest(callback, name) : undefined;
    `, context);
    vm.runInContext(source, context);
    while (timers.size) {
        const [id, timer] = [...timers].sort((a, b) => a[1].time - b[1].time || a[0] - b[0])[0];
        assert.ok(timer.time < 1000, 'the two cases must complete before their harness deadline');
        timers.delete(id);
        now = timer.time;
        timer.callback();
    }
    return JSON.parse(JSON.stringify(await context.__wptCompletion));
}

test('locked upstream cases demonstrate the original missing-event false pass', async () => {
    const results = await execute(original, 'missing');
    assert.deepEqual(results.map(x => x.name), guardedRejectionTests);
    assert.deepEqual(results.map(x => x.status), ['PASS', 'PASS']);
});

test('normal events preserve both original test bodies and their assertions', async () => {
    const results = await execute(guardRejectionEvents(original));
    assert.deepEqual(results.map(x => x.status), ['PASS', 'PASS']);
});

for (const delivery of ['missing', 'unrelated', 'late', 'duplicate']) {
    test(`${delivery} handled delivery cannot pass`, async () => {
        const results = await execute(guardRejectionEvents(original), delivery);
        assert.equal(results.length, 2);
        for (const result of results) {
            assert.equal(result.status, 'FAIL');
            assert.equal(result.errorName, 'AssertionError');
            assert.equal(result.phase, 'cleanup', 'presence failures cannot use a test-body waiver');
            assert.ok(result.message.includes(diagnostic), result.message);
        }
    });
}

test('default-workerd reason disagreement remains visible', async () => {
    const results = await execute(guardRejectionEvents(original), 'workerd-reason');
    assert.deepEqual(results.map(x => x.status), ['FAIL', 'FAIL']);
    assert.deepEqual(results.map(x => x.errorName), ['AssertionError', 'TypeError']);
    for (const result of results) {
        assert.equal(result.phase, 'test');
        assert.ok(!result.message.includes(diagnostic));
    }
});

test('source layout changes fail instead of silently omitting the guards', () => {
    assert.throws(() => guardRejectionEvents(original.replace(guardedRejectionTests[0], 'renamed upstream case')),
        /guard target/);
    assert.throws(() => guardRejectionEvents(original + original), /guard target/);
    assert.throws(() => guardRejectionEvents(guardRejectionEvents(original)), /already guarded/);
});

test('removing only the declared insertions restores every original byte', () => {
    const guarded = guardRejectionEvents(original);
    assert.notEqual(guarded, original);
    const restored = guarded
        .replaceAll('  var __capsidHandledCallbacks = 0;\n' +
            '  t.add_cleanup(function() {\n' +
            `    assert_equals(__capsidHandledCallbacks, 1, '${diagnostic}');\n` +
            '  });\n', '')
        .replaceAll('      __capsidHandledCallbacks++;\n', '');
    assert.equal(restored, original);
});
