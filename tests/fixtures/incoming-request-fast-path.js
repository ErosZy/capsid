const originalTest = RegExp.prototype.test;

// The two validation regexes normalizeName/normalizeValue historically
// dispatched on every inbound header pair. The inbound path must not pay
// that dispatch: the host has already validated the fields, and the light
// re-validation must use the byte-scanner implementation instead.
const headerValidationSources = [
    '[^!#$%&\'*+.^_`|~0-9A-Za-z-]',
    '[\\0\\r\\n]',
];

RegExp.prototype.test = function(value) {
    if (headerValidationSources.includes(this.source)) {
        throw new Error('incoming Request repeated JS header regex validation');
    }
    return Reflect.apply(originalTest, this, [ value ]);
};

export default {
    fetch(request) {
        // Restore before constructing the response: this probe is scoped to
        // bootstrap's inbound Request representation only.
        RegExp.prototype.test = originalTest;
        const clone = request.clone();
        return new Response(JSON.stringify({
            probe: request.headers.get('x-capsid-probe'),
            duplicate: request.headers.get('x-duplicate'),
            cloneProbe: clone.headers.get('x-capsid-probe'),
        }), { headers: { 'content-type': 'application/json' } });
    },
};
