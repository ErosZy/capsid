const originalTest = RegExp.prototype.test;

RegExp.prototype.test = function(value) {
    if (this.source.includes('0-9A-Za-z-')) {
        throw new Error('incoming Request repeated JS header validation');
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
