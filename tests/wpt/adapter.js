const results = [];
const pending = new Set();
let sealed = false;
let implicitTest = null;
let resolveCompletion;

const completion = new Promise(resolve => {
    resolveCompletion = resolve;
});

class AssertionError extends Error {
    constructor(message) {
        super(message);
        this.name = 'AssertionError';
    }
}

const format = value => {
    if (typeof value === 'string') {
        return JSON.stringify(value);
    }
    try {
        return JSON.stringify(value);
    } catch {
        return String(value);
    }
};

globalThis.format_value = format;

const assertionMessage = (description, detail) =>
    description ? `${description}: ${detail}` : detail;

const maybeComplete = () => {
    if (sealed && pending.size === 0) {
        resolveCompletion(Object.freeze(results.slice()));
    }
};

class HarnessTest {
    constructor(name, asynchronous) {
        this.name = name || 'Untitled test';
        this.asynchronous = asynchronous;
        this.completed = false;
        this.cleanups = [];
        this.timeout = asynchronous
            ? setTimeout(() => {
                this.fail(new Error('test timed out'));
            }, 10000)
            : undefined;
        if (asynchronous) {
            pending.add(this);
        }
    }

    step(callback, thisValue, args = []) {
        if (this.completed) {
            return undefined;
        }
        try {
            return callback.apply(thisValue ?? this, args);
        } catch (error) {
            this.fail(error);
            return undefined;
        }
    }

    step_func(callback, thisValue) {
        return (...args) => this.step(callback, thisValue, args);
    }

    step_func_done(callback = () => {}, thisValue) {
        return (...args) => {
            if (this.completed) {
                return;
            }
            this.step(callback, thisValue, args);
            if (!this.completed) {
                this.done();
            }
        };
    }

    step_timeout(callback, delay, ...args) {
        return setTimeout(this.step_func(callback), delay, ...args);
    }

    unreached_func(description = 'reached unreachable code') {
        return this.step_func(() => {
            throw new AssertionError(description);
        });
    }

    add_cleanup(callback) {
        this.cleanups.push(callback);
    }

    fail(error) {
        if (this.completed) {
            return;
        }
        const summary = error?.message ?? String(error);
        const detail = error?.stack
            ? `${summary}\n${error.stack}`
            : summary;
        this.finish('FAIL', detail);
    }

    done() {
        if (!this.completed) {
            this.finish('PASS', '');
        }
    }

    finish(status, message) {
        this.completed = true;
        if (this.timeout !== undefined) {
            clearTimeout(this.timeout);
        }
        for (const cleanup of this.cleanups.reverse()) {
            try {
                cleanup();
            } catch (error) {
                status = 'FAIL';
                message ||= `cleanup failed: ${error?.stack ?? error}`;
            }
        }
        results.push({ name: this.name, status, message });
        pending.delete(this);
        maybeComplete();
    }
}

globalThis.setup = options => {
    if (typeof options === 'function') {
        options();
        return;
    }
    if (options?.single_test && !implicitTest) {
        implicitTest = new HarnessTest('single test', true);
    }
};

globalThis.test = (callback, name) => {
    const testCase = new HarnessTest(name, false);
    testCase.step(callback, testCase, [ testCase ]);
    if (!testCase.completed) {
        testCase.done();
    }
    return testCase;
};

globalThis.async_test = (callback, name) => {
    if (typeof callback !== 'function') {
        return new HarnessTest(callback, true);
    }
    const testCase = new HarnessTest(name, true);
    testCase.step(callback, testCase, [ testCase ]);
    return testCase;
};

globalThis.promise_test = (callback, name) => {
    const testCase = new HarnessTest(name, true);
    let promise;
    try {
        promise = callback.call(testCase, testCase);
    } catch (error) {
        testCase.fail(error);
        return testCase;
    }
    Promise.resolve(promise).then(
        () => testCase.done(),
        error => testCase.fail(error),
    );
    return testCase;
};

globalThis.step_timeout = (callback, delay, ...args) =>
    setTimeout(callback, delay, ...args);

globalThis.done = () => {
    if (implicitTest) {
        implicitTest.done();
        return;
    }
    sealed = true;
    maybeComplete();
};

globalThis.assert_true = (actual, description) => {
    if (actual !== true) {
        throw new AssertionError(
            assertionMessage(description, `expected true, got ${format(actual)}`),
        );
    }
};

globalThis.assert_false = (actual, description) => {
    if (actual !== false) {
        throw new AssertionError(
            assertionMessage(description, `expected false, got ${format(actual)}`),
        );
    }
};

globalThis.assert_equals = (actual, expected, description) => {
    if (!Object.is(actual, expected)) {
        throw new AssertionError(assertionMessage(
            description,
            `expected ${format(expected)}, got ${format(actual)}`,
        ));
    }
};

globalThis.assert_not_equals = (actual, expected, description) => {
    if (Object.is(actual, expected)) {
        throw new AssertionError(assertionMessage(
            description,
            `expected values to differ, both were ${format(actual)}`,
        ));
    }
};

globalThis.assert_array_equals = (actual, expected, description) => {
    if (actual == null || expected == null ||
        actual.length !== expected.length) {
        throw new AssertionError(assertionMessage(
            description,
            `array lengths differ: ${format(actual)} vs ${format(expected)}`,
        ));
    }
    for (let index = 0; index < expected.length; ++index) {
        if (!Object.is(actual[index], expected[index])) {
            throw new AssertionError(assertionMessage(
                description,
                `arrays differ at ${index}: ` +
                    `${format(actual[index])} vs ${format(expected[index])}`,
            ));
        }
    }
};

globalThis.assert_own_property = (object, property, description) => {
    if (!Object.prototype.hasOwnProperty.call(object, property)) {
        throw new AssertionError(assertionMessage(
            description,
            `expected own property ${format(property)}`,
        ));
    }
};

globalThis.assert_inherits = (object, property, description) => {
    if (!(property in object) ||
        Object.prototype.hasOwnProperty.call(object, property)) {
        throw new AssertionError(assertionMessage(
            description,
            `expected inherited property ${format(property)}`,
        ));
    }
};

globalThis.assert_in_array = (actual, expected, description) => {
    if (!expected.includes(actual)) {
        throw new AssertionError(assertionMessage(
            description,
            `expected ${format(actual)} to be in ${format(expected)}`,
        ));
    }
};

globalThis.assert_regexp_match = (actual, expected, description) => {
    if (!expected.test(actual)) {
        throw new AssertionError(assertionMessage(
            description,
            `expected ${format(actual)} to match ${expected}`,
        ));
    }
};

globalThis.assert_class_string = (object, expected, description) => {
    const actual = Object.prototype.toString.call(object);
    const classString = `[object ${expected}]`;
    if (actual !== classString) {
        throw new AssertionError(assertionMessage(
            description,
            `expected ${classString}, got ${actual}`,
        ));
    }
};

globalThis.assert_approx_equals =
    (actual, expected, epsilon, description) => {
        if (typeof actual !== 'number' || typeof expected !== 'number' ||
            Math.abs(actual - expected) > epsilon) {
            throw new AssertionError(assertionMessage(
                description,
                `expected ${format(actual)} to be within ${epsilon} of ` +
                    format(expected),
            ));
        }
    };

globalThis.assert_greater_than = (actual, expected, description) => {
    if (!(actual > expected)) {
        throw new AssertionError(assertionMessage(
            description,
            `expected ${format(actual)} to be greater than ${format(expected)}`,
        ));
    }
};

globalThis.assert_throws_js = (constructor, callback, description) => {
    let thrown = false;
    try {
        callback();
    } catch (error) {
        thrown = true;
        if (!(error instanceof constructor)) {
            throw new AssertionError(assertionMessage(
                description,
                `expected ${constructor.name}, got ${error?.constructor?.name}`,
            ));
        }
    }
    if (!thrown) {
        throw new AssertionError(assertionMessage(
            description,
            `expected ${constructor.name} to be thrown`,
        ));
    }
};

globalThis.assert_throws_exactly = (expected, callback, description) => {
    let thrown = false;
    try {
        callback();
    } catch (error) {
        thrown = true;
        if (error !== expected) {
            throw new AssertionError(assertionMessage(
                description,
                `expected ${format(expected)}, got ${format(error)}`,
            ));
        }
    }
    if (!thrown) {
        throw new AssertionError(assertionMessage(
            description,
            `expected ${format(expected)} to be thrown`,
        ));
    }
};

globalThis.assert_throws_dom =
    (name, constructorOrCallback, callbackOrDescription, maybeDescription) => {
        const hasConstructor =
            constructorOrCallback?.name === 'DOMException';
        const constructor =
            hasConstructor ? constructorOrCallback : DOMException;
        const callback =
            hasConstructor ? callbackOrDescription : constructorOrCallback;
        const description =
            hasConstructor ? maybeDescription : callbackOrDescription;
        let error;
        try {
            callback();
        } catch (caught) {
            error = caught;
        }
        if (!(error instanceof constructor) || error.name !== name) {
            throw new AssertionError(assertionMessage(
                description,
                `expected ${name}, got ${error?.name ?? 'no exception'}`,
            ));
        }
    };

globalThis.assert_throws_quotaexceedederror =
    (callback, _requested, _quota, description) => {
        let error;
        try {
            callback();
        } catch (caught) {
            error = caught;
        }
        if (!(error instanceof DOMException) ||
            error.name !== 'QuotaExceededError' ||
            error.code !== 22) {
            throw new AssertionError(assertionMessage(
                description,
                `expected QuotaExceededError, got ` +
                    `${error?.name ?? 'no exception'}`,
            ));
        }
    };

globalThis.promise_rejects_js =
    async (_test, constructor, promise, description) => {
        try {
            await promise;
        } catch (error) {
            if (error instanceof constructor) {
                return error;
            }
            throw new AssertionError(assertionMessage(
                description,
                `expected rejection with ${constructor.name}, got ` +
                    `${error?.constructor?.name}`,
            ));
        }
        throw new AssertionError(assertionMessage(
            description,
            `expected rejection with ${constructor.name}`,
        ));
    };

globalThis.promise_rejects_exactly =
    async (_test, expected, promise, description) => {
        try {
            await promise;
        } catch (error) {
            if (error === expected) {
                return error;
            }
            throw new AssertionError(assertionMessage(
                description,
                `expected rejection with ${format(expected)}, got ` +
                    format(error),
            ));
        }
        throw new AssertionError(assertionMessage(
            description,
            `expected rejection with ${format(expected)}`,
        ));
    };

globalThis.assert_unreached = (description = 'reached unreachable code') => {
    throw new AssertionError(description);
};

globalThis.__wptSeal = () => {
    sealed = true;
    maybeComplete();
};

globalThis.__wptCompletion = completion;
