const NativeURLSearchParams = globalThis.URLSearchParams;
const nativePrototype = NativeURLSearchParams.prototype;

function toUSVString(value) {
    const string = String(value);

    return string.replace(/[\uD800-\uDFFF]/g, (codeUnit, index) => {
        const code = codeUnit.charCodeAt(0);

        if (code <= 0xDBFF) {
            const next = string.charCodeAt(index + 1);
            if (next >= 0xDC00 && next <= 0xDFFF) {
                return codeUnit;
            }
        } else {
            const previous = string.charCodeAt(index - 1);
            if (previous >= 0xD800 && previous <= 0xDBFF) {
                return codeUnit;
            }
        }

        return '\uFFFD';
    });
}

function normalizeSequence(init) {
    const pairs = [];

    for (const entry of init) {
        const tuple = Array.from(entry);
        if (tuple.length !== 2) {
            throw new TypeError(
                'URLSearchParams sequence entries must contain exactly two items');
        }
        pairs.push([ toUSVString(tuple[0]), toUSVString(tuple[1]) ]);
    }

    return pairs;
}

function normalizeRecord(init) {
    // Web IDL converts record keys to USVString before constructing the
    // sequence. A later key wins when two JS keys normalize to the same value.
    const normalized = new Map();

    for (const key of Object.keys(init)) {
        normalized.set(toUSVString(key), toUSVString(init[key]));
    }

    return Array.from(normalized);
}

function URLSearchParams(init = '') {
    if (!new.target) {
        throw new TypeError(
            "URLSearchParams constructor must be called with 'new'");
    }

    if (init !== null && (typeof init === 'object' ||
        typeof init === 'function')) {
        const iterator = init[Symbol.iterator];
        init = iterator === undefined
            ? normalizeRecord(init)
            : normalizeSequence(init);
    } else {
        init = toUSVString(init);
    }

    return new NativeURLSearchParams(init);
}

Object.defineProperty(URLSearchParams, 'prototype', {
    value: nativePrototype,
});
Object.defineProperty(nativePrototype, 'constructor', {
    configurable: true,
    writable: true,
    value: URLSearchParams,
});
Object.defineProperty(globalThis, 'URLSearchParams', {
    configurable: true,
    writable: true,
    value: URLSearchParams,
});
