import assert from 'node:assert/strict';
import {test} from 'node:test';
import {assertHandlerThrowObservation} from '../tools/workerd-throw-contract.mjs';

// These are validation controls, not reference execution evidence. Keep the
// wire goldens independent of the producer and of its comparison projection.
const sample = () => ({
    c17: {wire: {hex: '8106717565756564', frames: [{opcode: 1, payload: 'cXVldWVk'}],
        peerTerminated: true, completion: 'peer-ended', transportError: null},
        client: {seen: [], close: {code: 1006, reason: '', wasClean: false}}},
    c18: {wire: {hex: '8108612d6265666f7265880603e8646f6e65', frames: [
        {opcode: 1, payload: 'YS1iZWZvcmU='}, {opcode: 8, payload: 'A+hkb25l'},
    ], peerTerminated: true, completion: 'peer-reset', transportError: 'ECONNRESET'},
        client: {seen: ['a-before'], close: {code: 1000, reason: 'done', wasClean: true}}},
});

test('comparison retains exact frames while validating both observed client schedules', () => {
    const first = sample(), before = structuredClone(first);
    const compared = assertHandlerThrowObservation(first);
    assert.deepEqual(first, before, 'original observations remain intact');
    const second = sample();
    second.c17.client.seen = ['queued'];
    second.c18.client = {close: {wasClean: false, reason: '', code: 1006}, seen: []};
    second.c18.wire.completion = 'peer-ended';
    second.c18.wire.transportError = null;
    assert.deepEqual(assertHandlerThrowObservation(second), compared);
    assert.deepEqual(compared.c17, {hex: '8106717565756564',
        frames: [{opcode: 1, payload: 'cXVldWVk'}], peerTerminated: true});
});

for (const [name, corrupt, diagnostic] of [
    ['missing case', value => delete value.c18, /both handler throw cases required/],
    ['dropped send', value => {value.c17.wire.hex = ''; value.c17.wire.frames = [];}, /c17: throw wire contract exact bytes/],
    ['corrupted send', value => value.c17.wire.frames[0].payload = 'YmFk', /c17: throw wire contract exact complete frames/],
    ['dropped Close', value => value.c18.wire.hex = '8108612d6265666f7265', /c18: throw wire contract exact bytes/],
    ['reordered frames', value => value.c18.wire.frames.reverse(), /c18: throw wire contract exact complete frames/],
    ['extra frame', value => value.c17.wire.frames.push({opcode: 1, payload: 'cXVldWVk'}), /c17: throw wire contract exact complete frames/],
    ['local disposal', value => value.c17.wire.completion = 'client-disposed', /c17: throw wire contract no deadline or local disposal/],
    ['deadline', value => value.c18.wire.completion = 'deadline', /c18: throw wire contract no deadline or local disposal/],
    ['invented terminal', value => value.c17.wire.peerTerminated = false, /c17: throw wire contract actual peer terminal/],
    ['hidden transport error', value => value.c18.wire.transportError = null, /c18: throw wire contract actual transport error/],
    ['duplicate client message', value => value.c17.client.seen.push('queued', 'queued'), /c17: throw wire contract exact allowed client messages/],
    ['wrong client message', value => value.c18.client.seen = ['bad'], /c18: throw wire contract exact delivered client message/],
    ['wrong client Close', value => value.c18.client.close.reason = 'bad', /c18: throw wire contract delivered client close/],
    ['lost message with clean client Close', value => value.c18.client.seen = [], /c18: throw wire contract discarded client close/],
]) test(`comparison rejects ${name}`, () => {
    const value = sample();
    corrupt(value);
    assert.throws(() => assertHandlerThrowObservation(value), diagnostic);
});
