import assert from 'node:assert/strict';

// Literal same-input observations on both pinned official x64 workerd assets.
// The unit controls independently pin this list and each expected trace.
export const handlerErrorKinds = Object.freeze(['Error', 'TypeError', 'RangeError', 'ReferenceError',
    'SyntaxError', 'URIError', 'EvalError', 'string', 'undefined', 'null', 'number',
    'subclass', 'renamed-type', 'renamed-error', 'number-message', 'empty-message',
    'message-getter', 'message-getter-throws', 'custom-tostring', 'tostring-throws',
    'symbol', 'bigint', 'override-timer', 'override-hasown', 'async-error-listener',
    'microtask-message-mutation', 'microtask-name-mutation',
    'error-send', 'error-send-symbol', 'error-send-object', 'error-send-conversion-throws',
    'error-close', 'error-close-send', 'error-invalid-close', 'error-microtask-send',
    'getter-send', 'getter-close', 'getter-close-send',
    'object', 'object-primitive', 'object-tostring-throws', 'null-prototype',
    'object-data', 'object-cycle', 'object-getter', 'object-symbol-value', 'object-proxy',
    'object-tostringtag', 'object-fake-error',
    'dom-exception', 'dom-custom-name', 'dom-number-message', 'dom-extra', 'dom-name-getter',
    'dom-message-getter', 'dom-null-prototype', 'dom-fake', 'dom-combined-getters',
    'aggregate-error', 'aggregate-data', 'aggregate-cycle', 'aggregate-errors-getter',
    'error-cause', 'error-properties', 'error-cycle', 'error-fixed-stack', 'error-null-prototype',
    'error-name-number', 'error-name-null', 'error-name-undefined', 'error-name-object', 'error-name-symbol',
    'error-own-function', 'error-cycle-plain', 'error-ordered-getters', 'error-symbol-getter',
    'override-string', 'name-getter-throws', 'array', 'function']);
export const handlerErrorCaseIds = Object.freeze(['c17', 'c18'].flatMap(scenario =>
    [...handlerErrorKinds, ...(scenario === 'c18' ? ['error-listener-throw'] : [])].map(kind => scenario + '/' + kind)));
// This upstream case retains a transport and prevents cooperative Linux
// reference shutdown even after client disposal. Keep its raw observations
// and product regression separate from a passing shared reference process.
export const handlerErrorReferenceBoundary = 'c17/error-listener-throw';

export function assertHandlerErrorCase(id, value, check = assert) {
    check.ok(handlerErrorCaseIds.includes(id) || id === handlerErrorReferenceBoundary, 'handler error known case');
    const [scenario, variant] = id.split('/');
    const kind = variant === 'error-listener-throw' ? 'TypeError' : variant;
    const retainedTransport = scenario === 'c17' && variant === 'error-listener-throw';
    const label = id + ': handler error contract';
    check.ok(value && typeof value === 'object', label + ' observation required');
    check.deepEqual(Object.keys(value).sort(), ['events', 'wire'], label + ' complete observation');
    const enhanced = handlerErrorKinds.indexOf(kind) >= 49;
    let internalMessage;
    if (kind === 'name-getter-throws') {
        internalMessage = value.events?.find(event => event.type === 'error')?.errorMessage;
        check.match(internalMessage ?? '', /^internal error; reference = [0-9a-v]{24}$/, label + ' actual internal diagnostic reference');
    }
    const enhancedError = enhanced ? expectedEnhancedError(kind, scenario, internalMessage) : null;
    const primitive = {string: 'thrown-string', undefined: 'undefined', null: 'null', number: '42'};
    const extended = handlerErrorKinds.indexOf(kind) >= 11;
    const ordinary = ['object-primitive', 'null-prototype', 'object-data', 'object-cycle',
        'object-getter', 'object-tostringtag', 'object-fake-error', 'dom-fake', 'array'].includes(kind);
    const extra = {
        subclass: ['CustomError', 'custom-message'], 'renamed-type': ['Renamed', 'handler-error/' + scenario],
        'renamed-error': ['TypeError', 'changed-type'], 'number-message': ['TypeError', ''],
        'empty-message': ['TypeError', ''], 'message-getter': ['TypeError', 'getter-message'],
        'message-getter-throws': ['TypeError', ''], symbol: ['Error', 'Symbol(handler-symbol)'],
        bigint: ['Error', '12345678901234567890'],
        'getter-send': ['TypeError', 'getter-message'], 'getter-close': ['TypeError', 'getter-message'],
        'getter-close-send': ['TypeError', 'getter-message'],
        object: ['Error', '[object Object]'], 'object-tostring-throws': ['Error', '[object Object]'],
        'object-symbol-value': ['Error', '#<Object>'], 'object-proxy': ['Error', '#<Object>'],
        'object-primitive': [undefined, undefined], 'null-prototype': [undefined, undefined],
        'object-data': [undefined, 'plain-message'], 'object-cycle': [undefined, undefined],
        'object-getter': [undefined, undefined], 'object-tostringtag': [undefined, undefined],
        'object-fake-error': [undefined, 'fake-message'],
    };
    const [errorName, errorMessage] = enhancedError ? [enhancedError.name, enhancedError.message] : extra[kind] ??
        (Object.hasOwn(primitive, kind) ? ['Error', primitive[kind]] :
            [extended ? 'TypeError' : kind, 'handler-error/' + scenario]);
    const errorText = enhancedError?.text ?? (ordinary ? '[object Object]' : errorName + (errorMessage ? ': ' + errorMessage : ''));
    const getterClose = scenario === 'c17' && ['getter-close', 'getter-close-send'].includes(kind);
    const state = scenario === 'c17' && !getterClose ? 1 : 2;
    const send = state === 1 ? {type: 'microtask-send', readyState: 1, outcome: 'accepted'}
        : {type: 'microtask-send', readyState: 2, outcome: 'threw', name: 'TypeError',
            message: "Can't call WebSocket send() after close()."};
    check.deepEqual(value.events, [
        {type: 'message', readyState: 1}, ...getterIOTrace(scenario, kind), {type: 'microtask', readyState: state},
        {type: 'nested-microtask', readyState: state}, send,
        {type: 'error', readyState: 3, message: enhancedError?.eventMessage ?? (ordinary ? 'Uncaught #<Object>' : 'Uncaught ' + errorText),
            ...(errorName === undefined ? {} : {errorName}), ...(errorMessage === undefined ? {} : {errorMessage}),
            errorText, errorType: 'object', originalError: false, trusted: true, bubbles: false, cancelable: false,
            ...(extended ? {conversionCalls: enhancedError?.calls ?? (['message-getter', 'message-getter-throws'].includes(kind)
                ? [{operation: 'message', readyState: state, after: ['message']}]
                : kind === 'object-getter' ? [{operation: 'detail', readyState: state, after: ['message']}] : []),
                overrideCalls: [], errorIsError: !ordinary, errorIsTypeError: errorName === 'TypeError'} : {}),
            ...(handlerErrorKinds.indexOf(kind) >= 38 && !enhanced ? {objectGraph: expectedObjectGraph(kind)} : {}),
            ...(enhanced ? {errorGraph: enhancedError.graph, ...(enhancedError.stack === undefined ? {} : {errorStack: enhancedError.stack}),
                errorIsAggregateError: kind.startsWith('aggregate-'), errorIsDOMException: enhancedError.dom} : {})},
        ...errorIOTrace(scenario, kind),
    ], label + ' exact microtask order, state, send and trusted error fields');
    const wire = value.wire;
    check.deepEqual(Object.keys(wire).sort(), ['closeReplySent', 'completion', 'frames', 'hex', 'peerTerminated', 'transportError',
        ...(retainedTransport ? ['observedOpenMs'] : [])].sort(),
        label + ' exact wire fields');
    const frames = scenario === 'c18'
        ? [{opcode: 1, payload: 'YS1iZWZvcmU='}, {opcode: 8, payload: 'A+hkb25l'}]
        : getterClose ? [{opcode: 1, payload: 'cXVldWVk'}, {opcode: 8, payload: 'A+hnZXR0ZXItZG9uZQ=='}]
        : [{opcode: 1, payload: 'cXVldWVk'},
            ...(kind === 'getter-send' ? [{opcode: 1, payload: 'Z2V0dGVyLXZhbHVl'}] : []),
            {opcode: 1, payload: 'bGF0ZQ=='},
            ...({ 'error-send': [{opcode: 1, payload: 'ZnJvbS1lcnJvcg=='}],
                'error-send-object': [{opcode: 1, payload: 'ZXJyb3ItdmFsdWU='}],
                'error-close-send': [{opcode: 1, payload: 'YWZ0ZXItY2xvc2UtdmFsdWU='}]}[kind] ?? [])];
    const hex = frames.map(frame => {
        const payload = Buffer.from(frame.payload, 'base64');
        return Buffer.concat([Buffer.from([0x80 | frame.opcode, payload.length]), payload]).toString('hex');
    }).join('');
    check.equal(wire.hex, hex,
        label + ' exact accepted wire bytes');
    check.deepEqual(wire.frames, frames, label + ' exact complete wire frames');
    check.equal(wire.peerTerminated, !retainedTransport, label + ' actual peer terminal');
    if (retainedTransport) {
        // Both official assets retain C17's transport if its error listener
        // itself throws. Local cleanup is explicit and never counted as EOF.
        check.equal(wire.completion, 'client-disposed', label + ' explicit local cleanup of retained transport');
        check.ok(Number.isFinite(wire.observedOpenMs) && wire.observedOpenMs >= 100,
            label + ' actual retained-transport observation');
    } else check.ok(['peer-ended', 'peer-reset'].includes(wire.completion), label + ' no deadline or local disposal');
    check.equal(wire.transportError, wire.completion === 'peer-reset' ? 'ECONNRESET' : null, label + ' actual terminal error');
    check.equal(wire.closeReplySent, false, label + ' no peer Close reply');
    // Keep raw observations; only unstable TCP EOF/reset is omitted here.
    // Only the fresh diagnostic ID is unstable. Its exact shape, field
    // linkage, graph, getter count, event order and wire were validated above.
    const events = internalMessage ? JSON.parse(JSON.stringify(value.events).replaceAll(internalMessage.slice(-24), '<diagnostic-id>')) : value.events;
    return {events, hex: wire.hex, frames: wire.frames, peerTerminated: wire.peerTerminated,
        closeReplySent: false, retainedAfterErrorListenerThrow: retainedTransport};
}

export function assertHandlerErrorInventory(values, check = assert, includeReferenceBoundary = false) {
    check.ok(values && typeof values === 'object', 'handler error observations required');
    check.deepEqual(Object.keys(values).sort(), [...handlerErrorCaseIds,
        ...(includeReferenceBoundary ? [handlerErrorReferenceBoundary] : [])].sort(), 'handler error complete161-case inventory and explicit boundary');
    const ids = ['c17', 'c18'].map(scenario => values[scenario + '/name-getter-throws'].events.find(event => event.type === 'error').errorMessage);
    check.equal(new Set(ids).size, 2, 'handler error fresh diagnostic references');
}

export function assertHandlerErrorDiagnostics(values, stderr, check = assert) {
    for (const scenario of ['c17', 'c18']) {
        const message = values[scenario + '/name-getter-throws'].events.find(event => event.type === 'error').errorMessage;
        check.ok(stderr.includes(message.slice(-24)), scenario + ': handler error diagnostic reference has a real log record');
    }
}

function expectedEnhancedError(kind, scenario, internalMessage) {
    const primitive = value => value === undefined ? {type: 'undefined'} : {type: typeof value, value};
    const property = (value, enumerable = true) => ({writable: true, enumerable, configurable: true, value});
    const object = (id, properties = {}) => ({id, array: false, prototype: 'Object', error: false, symbols: 0, properties});
    const array = (id, length, properties) => ({id, array: true, prototype: 'Array', error: false, symbols: 0,
        properties: {...properties, length: {writable: true, enumerable: false, configurable: false, value: primitive(length)}}});
    const stack = enumerable => ({enumerable, configurable: true, get: 'function', set: 'function'});
    const native = (id, message, properties = {}) => ({id, array: false, prototype: 'other', error: true, symbols: 0,
        properties: {stack: stack(false), message: property(primitive(message), false), ...properties}});
    const child = object(1, {x: property(primitive(7))});
    const dom = kind.startsWith('dom-') && kind !== 'dom-fake';
    let name = dom ? 'AbortError' : kind.startsWith('aggregate-') ? 'AggregateError' : 'TypeError';
    let message = dom ? 'dom-message' : kind.startsWith('aggregate-') ? 'aggregate-message' : 'handler-error/' + scenario;
    let graph, calls = [];
    if (kind === 'dom-fake' || kind === 'array') {
        return {name: undefined, message: undefined, text: kind === 'array' ? '1,,,x' : '[object Object]',
            eventMessage: kind === 'array' ? 'Uncaught [object Array]' : 'Uncaught #<Object>', dom: false, calls,
            graph: kind === 'array' ? array(0, 4, {0: property(primitive(1)), 2: property(primitive(undefined)), 3: property(primitive('x'))}) : object(0)};
    }
    if (kind === 'error-cause') message = 'cause-message';
    if (kind === 'function') {name = 'Error'; message = 'function fixtureFunction(){return 7;}';}
    if (kind === 'name-getter-throws') {name = 'Error'; message = internalMessage; calls = ['name', 'name'];}
    if (kind === 'error-name-object' || kind === 'error-name-symbol') name = 'Error';
    if (kind === 'error-name-object') calls = ['name-conversion'];
    if (kind === 'error-name-number') name = 7;
    if (kind === 'error-name-null') name = null;
    if (kind === 'error-name-undefined' || kind === 'error-null-prototype') name = undefined;
    const text = (name === undefined ? 'Error' : String(name)) + ': ' + message;
    graph = native(0, message);
    if (dom) {
        graph.properties = {code: {...property(primitive(20)), writable: false},
            name: {...property(primitive('AbortError')), writable: false},
            message: {...property(primitive('dom-message')), writable: false}, stack: stack(true)};
        if (kind === 'dom-name-getter') calls = ['dom-name', 'dom-name'];
        if (kind === 'dom-message-getter') calls = ['dom-message'];
        if (kind === 'dom-combined-getters') calls = ['name', 'message', 'name'];
    }
    if (typeof name !== 'string') graph.properties.name = property(primitive(name));
    if (kind === 'aggregate-error') graph.properties.errors = property(array(1, 1, {0: property(native(2, 'child'))}));
    if (kind === 'aggregate-data') {
        graph.properties.cause = property(child);
        graph.properties.errors = property(array(2, 3, {0: property({ref: 1}), 1: property(primitive('child')), 2: property(primitive(null))}));
    }
    if (kind === 'aggregate-errors-getter') {calls = ['errors']; graph.properties.errors = property(array(1, 1,
        {0: property(object(2, {x: property(primitive(7))}))}));}
    if (kind === 'error-cause') graph.properties.cause = property(child);
    if (kind === 'error-properties') {graph.properties.hidden = property(child); graph.properties.extra = property(primitive(9));}
    if (kind === 'error-ordered-getters') {
        calls = ['first', 'second', 'nested'];
        graph.properties.first = property(object(1, {nested: property(primitive(7))}));
        graph.properties.second = property(primitive(8));
    }
    if (kind === 'error-symbol-getter') calls = ['symbol'];
    return {name, message, text, graph, calls, dom, stack: text,
        eventMessage: dom ? 'Uncaught ' : 'Uncaught ' + (typeof name === 'string' ? text : message)};
}

function expectedObjectGraph(kind) {
    const primitive = value => value === undefined ? {type: 'undefined'} : {type: typeof value, value};
    const property = (value, enumerable = true) => ({writable: true, enumerable, configurable: true, value});
    const object = (id, properties = {}) => ({id, array: false, prototype: 'Object', error: false, symbols: 0, properties});
    const child = object(1, {x: property(primitive(7))});
    switch (kind) {
        case 'object': case 'object-tostring-throws': case 'object-symbol-value': case 'object-proxy':
            return {id: 0, array: false, prototype: 'other', error: true, symbols: 0,
                properties: {message: property(primitive(['object', 'object-tostring-throws'].includes(kind) ? '[object Object]' : '#<Object>'), false)}};
        case 'object-data': return object(0, {message: property(primitive('plain-message')), nested: property(child),
            items: property({id:2,array:true,prototype:'Array',error:false,symbols:0,properties:{
                0:property(primitive(1)), 2:property(primitive(undefined)), 3:property({ref:1}), 4:property({ref:2}),
                length:{writable:true,enumerable:false,configurable:false,value:primitive(5)}, note:property(primitive('array-note'))}}),
            ['__proto__']:property(object(3,{marker:property(primitive('data'))}))});
        case 'object-cycle': return object(0, {left: property(child), right: property({ref: 1}), self: property({ref: 0})});
        case 'object-getter': return object(0, {detail: property(child)});
        case 'object-fake-error': return object(0, {message: property(primitive('fake-message'))});
        default: return object(0);
    }
}

// These traces pin argument conversion before state validation, synchronous
// error-listener writes, and silent post-dispatch sends as separate outcomes.
const closedSendMessage = "Can't call WebSocket send() after close().";
const accepted = (type, readyState) => ({type, readyState, outcome: 'accepted'});
const rejected = (type, readyState, message, name = 'TypeError') => ({type, readyState, outcome: 'threw', name, message});
function getterIOTrace(scenario, kind) {
    if (!['getter-send', 'getter-close', 'getter-close-send'].includes(kind)) return [];
    const initial = scenario === 'c17' ? 1 : 2;
    const trace = [{type: 'message-getter', readyState: initial}];
    if (kind === 'getter-send') return [...trace, {type: 'getter-conversion', readyState: initial},
        initial === 1 ? accepted('getter-send', 1) : rejected('getter-send', 2, closedSendMessage)];
    trace.push(accepted('getter-close', 2));
    if (kind === 'getter-close-send') trace.push({type: 'getter-after-close-conversion', readyState: 2},
        rejected('getter-send', 2, closedSendMessage));
    return trace;
}
function errorIOTrace(scenario, kind) {
    const open = scenario === 'c17';
    const send = type => open ? accepted(type, 3) : rejected(type, 3, closedSendMessage);
    switch (kind) {
        case 'error-send': return [send('error-send')];
        case 'error-send-symbol': return [rejected('error-send', 3, 'Cannot convert a Symbol value to a string')];
        case 'error-send-object': return [{type: 'error-conversion', readyState: 3}, send('error-send')];
        case 'error-send-conversion-throws': return [{type: 'error-conversion', readyState: 3}, rejected('error-send', 3, 'conversion/error')];
        case 'error-close': return [open ? rejected('error-close', 3, 'handler-error/c17') : accepted('error-close', 3)];
        case 'error-close-send': return [open ? rejected('error-close', 3, 'handler-error/c17') : accepted('error-close', 3),
            {type: 'after-close-conversion', readyState: 3}, send('error-send')];
        case 'error-invalid-close': return [rejected('error-close', 3, 'Invalid WebSocket close code: 1006.', 'InvalidAccessError')];
        case 'error-microtask-send': return [{type: 'post-error-conversion', readyState: 3}, send('post-error-send'),
            accepted('post-error-close', 3), {type: 'post-error-after-close-conversion', readyState: 3}, send('post-error-send-after-close')];
        default: return [];
    }
}

export function assertHandlerErrorObservations(values, check = assert) {
    assertHandlerErrorInventory(values, check);
    return Object.fromEntries(handlerErrorCaseIds.map(id => [id, assertHandlerErrorCase(id, values[id], check)]));
}
