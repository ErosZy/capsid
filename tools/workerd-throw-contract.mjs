import assert from 'node:assert/strict';

// Same-input raw TCP observations on both official x64 assets of workerd
// v1.20261010.1 (8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f),
// compatibility_date 2026-08-20, flags []. These bytes precede transport
// termination even when Node/undici discards its queued message events.
const wireContract = Object.freeze({
    c17: {hex: '8106717565756564', frames: [{opcode: 1, payload: 'cXVldWVk'}]},
    c18: {hex: '8108612d6265666f7265880603e8646f6e65', frames: [
        {opcode: 1, payload: 'YS1iZWZvcmU='}, {opcode: 8, payload: 'A+hkb25l'},
    ]},
});

export function assertHandlerThrowCase(scenario, value, check = assert) {
    const label = `${scenario}: throw wire contract`;
    check.ok(Object.hasOwn(wireContract, scenario), label + ' known scenario');
    check.ok(value && typeof value === 'object', label + ' observation required');
    check.deepEqual(Object.keys(value).sort(), ['client', 'wire'], label + ' complete fields');
    const {wire, client} = value, expected = wireContract[scenario];
    check.deepEqual(Object.keys(wire).sort(),
        ['completion', 'frames', 'hex', 'peerTerminated', 'transportError'], label + ' wire fields');
    check.equal(wire.hex, expected.hex, label + ' exact bytes');
    check.deepEqual(wire.frames, expected.frames, label + ' exact complete frames');
    check.equal(wire.peerTerminated, true, label + ' actual peer terminal');
    check.ok(['peer-ended', 'peer-reset'].includes(wire.completion), label + ' no deadline or local disposal');
    check.equal(wire.transportError, wire.completion === 'peer-reset' ? 'ECONNRESET' : null,
        label + ' actual transport error');
    check.deepEqual(Object.keys(client).sort(), ['close', 'seen'], label + ' client fields');
    const abrupt = {code: 1006, reason: '', wasClean: false};
    if (scenario === 'c17') {
        check.deepEqual(client.close, abrupt, label + ' abrupt client close');
        check.ok(JSON.stringify(client.seen) === '[]' || JSON.stringify(client.seen) === '["queued"]',
            label + ' exact allowed client messages');
    } else {
        if (Array.isArray(client.seen) && client.seen.length === 0) {
            check.deepEqual(client.seen, [], label + ' discarded client messages');
            check.deepEqual(client.close, abrupt, label + ' discarded client close');
        } else {
            check.deepEqual(client.seen, ['a-before'], label + ' exact delivered client message');
            check.deepEqual(client.close, {code: 1000, reason: 'done', wasClean: true},
                label + ' delivered client close');
        }
    }
    // Preserve actual traces in the report. Only comparison uses this checked
    // projection: EOF versus reset and queued client events vary by platform.
    return {hex: wire.hex, frames: structuredClone(wire.frames), peerTerminated: true};
}

export function assertHandlerThrowObservation(value, check = assert) {
    check.ok(value && typeof value === 'object', 'handler throw observations required');
    check.deepEqual(Object.keys(value).sort(), ['c17', 'c18'], 'both handler throw cases required');
    return Object.fromEntries(['c17', 'c18'].map(name => [name, assertHandlerThrowCase(name, value[name], check)]));
}
