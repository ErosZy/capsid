const matchRecord = match => match === null ? null : ({
    values: Array.from(match, value => value ?? null),
    index: match.index,
    groups: match.groups ? {...match.groups} : null,
    indices: match.indices ? Array.from(match.indices, pair => pair ?? null) : null,
});

const errorName = operation => {
    try { operation(); return 'no-error'; } catch (error) { return error.name; }
};

const cases = {
    captures() {
        const sizes = [0, 1, 6, 7, 8, 16, 40];
        let checksum = 0;
        const matches = sizes.map(groups => {
            const expression = new RegExp(`^${'(a)'.repeat(groups)}$`, 'd');
            const input = 'a'.repeat(groups);
            for (let repeat = 0; repeat < 64; ++repeat) {
                const match = expression.exec(input);
                if (match === null || match.length !== groups + 1) throw new Error('capture count');
                checksum += match.length;
            }
            return {groups, match: matchRecord(expression.exec(input))};
        });
        return {checksum, matches};
    },
    flags() {
        return [6, 7, 16].map(count => {
            const expression = new RegExp(`(?<lead>😀)${'(x)'.repeat(count)}`, 'dgu');
            const text = `!😀${'x'.repeat(count)}?`;
            const match = matchRecord(expression.exec(text));
            const lastIndex = expression.lastIndex;
            const missing = expression.exec(text);
            const sticky = new RegExp(expression.source, 'dyu');
            sticky.lastIndex = 1;
            const atOne = matchRecord(sticky.exec(text));
            sticky.lastIndex = 0;
            return {count, match, lastIndex, missing, reset: expression.lastIndex,
                atOne, atZero: sticky.exec(text), stickyReset: sticky.lastIndex};
        });
    },
    reentry() {
        const nested = new RegExp('(x)'.repeat(16), 'd');
        const expression = new RegExp(`(?<letter>a)${'(a)'.repeat(6)}`, 'dg');
        let calls = 0;
        let inner;
        expression.lastIndex = {valueOf() {
            ++calls;
            inner = matchRecord(nested.exec('x'.repeat(16)));
            return 1;
        }};
        return {outer: matchRecord(expression.exec('baaaaaaa')), inner,
            calls, lastIndex: expression.lastIndex};
    },
    overrides() {
        const expression = /a/;
        const trace = [];
        const marker = new Error('exec marker');
        expression.exec = function(input) { trace.push(input); return {0: 'custom', length: 1}; };
        const test = expression.test('first');
        const match = expression[Symbol.match]('second');
        Object.defineProperty(expression, 'exec', {get() {
            trace.push('get-exec');
            return () => { throw marker; };
        }});
        let same = false;
        try { expression.test('third'); } catch (error) { same = error === marker; }
        const receiver = {tag: 'receiver'};
        let proxyThis;
        const maximum = new Proxy(Math.max, {apply(target, self, args) {
            proxyThis = self.tag;
            return Reflect.apply(target, self, args);
        }});
        return {test, match: match[0], trace, same, proxy: Reflect.apply(maximum, receiver, [4, 12]), proxyThis};
    },
    calls() {
        class List extends Array {}
        const list = Reflect.construct(Array, [1, 2, 3], List);
        const failures = [
            () => Reflect.apply(null, null, []),
            () => Reflect.apply({}, null, []),
            () => new Math.max(),
            () => Array.prototype.values.call(null),
            () => Object.getOwnPropertyDescriptor(RegExp.prototype, 'global').get.call({}),
        ].map(errorName);
        return {bound: Math.max.bind(null, 3)(1, 9), list: Array.from(list), subclass: list instanceof List,
            boxed: Number.prototype.valueOf.call(Object(7)), failures};
    },
    iterators() {
        const array = [2, 4];
        const iterator = array.values();
        const first = iterator.next();
        array.push(6);
        const rest = Array.from(iterator);
        const invalid = errorName(() => Object.getPrototypeOf(iterator).next.call({}));
        const custom = [1, 2];
        custom[Symbol.iterator] = function* () { yield 7; yield 11; };
        const [head, ...tail] = custom;
        return {first, rest, done: iterator.next(), invalid, head, tail,
            string: Array.from('😀a'), typed: Array.from(new Uint16Array([0, 65535]).values()),
            entries: Array.from([4, 5].entries())};
    },
    async generators() {
        let closed = 0;
        async function* values() {
            try {
                yield await Promise.resolve(5);
                yield 8;
            } catch (error) {
                yield error.id;
            } finally {
                ++closed;
            }
        }
        const iterator = values();
        const first = await iterator.next();
        const caught = await iterator.throw({id: 13});
        const done = await iterator.next();
        let syncClosed = 0;
        function* sync() {
            try { yield Array.from([4, 5].entries()); } finally { ++syncClosed; }
        }
        const normal = sync();
        return {first, caught, done, closed, syncFirst: normal.next(),
            syncReturn: normal.return('finish'), syncClosed};
    },
    healthy() { return {okay: true, value: Math.round(2.4) + ' ok '.trim().length}; },
};

export default {
    async fetch(request) {
        const name = new URL(request.url).pathname.slice(1);
        if (!Object.hasOwn(cases, name)) return new Response('missing case', {status: 404});
        return Response.json(await cases[name]());
    },
};
