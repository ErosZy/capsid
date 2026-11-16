// Public Headers operations only; no internal normalizer or expected-result imports.
const observe = action => {
    try { return {ok: true, value: action()}; }
    catch (error) { return {ok: false, error: error.name}; }
};

export default {
    fetch() {
        const names = [], values = [];
        for (let code = 0; code < 128; ++code) {
            names.push(observe(() => new Headers([['x' + String.fromCharCode(code) + 'y', 'value']]).get('x' + String.fromCharCode(code) + 'y')));
            const value = 'a' + String.fromCharCode(code) + 'b';
            values.push(['constructor', 'append', 'set'].map(method => observe(() => {
                const headers = method === 'constructor' ? new Headers([['x', value]]) : new Headers();
                if (method !== 'constructor') headers[method]('x', value);
                return headers.get('x');
            })));
        }
        const unicode = ['é', '中文', '💡', '\ufeff', '\ud800', '\udc00', '\ud800\udc00', '\ud800a', '\ud800\ud800']
            .map(value => observe(() => new Headers([['x', value]]).get('x')));
        const trace = [];
        const converted = new Headers([[
            {[Symbol.toPrimitive](hint) { trace.push('name:' + hint); return 'X-Test'; }},
            {[Symbol.toPrimitive](hint) { trace.push('value:' + hint); return ' ascii '; }},
        ]]);
        return Response.json({
            names, values, unicode, trace, converted: converted.get('x-test'),
            whitespace: new Headers([['x', '\t\r\n ascii \r\n']]).get('x'),
            emptyName: new Headers([['', 'value']]).get(''),
            longAscii: new Headers([['x', 'a'.repeat(16384)]]).get('x'),
        });
    },
};
