// Run the real API driver against isolated fixture faults. A passing baseline
// is required first, so an unrelated startup error cannot count as detection.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createHash, randomUUID } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { fixtureIdentities, fileDigest, readLock, verifyClient, root } from '../tools/workerd-reference.mjs';
import { prepareMutationRunner } from '../tools/workerd-mutation-contract.mjs';

const args = process.argv.slice(2);
const option = name => args[args.indexOf(name) + 1];
assert.ok(args.includes('--host') && args.includes('--worker'));
const host = path.resolve(option('--host')), worker = path.resolve(option('--worker'));
const lock = readLock();
verifyClient(lock);
const identity = {schema:'capsid-workerd-mutations-v1', mode:'single-worker', lock,
    client:{node:process.version, undici:process.versions.undici},
    runId:process.env.CAPSID_WS_RUN_ID ?? randomUUID(), hostSHA256:fileDigest(host), workerSHA256:fileDigest(worker),
    controllerSHA256:fileDigest(fileURLToPath(import.meta.url)),
    contractSHA256:fileDigest(path.join(root,'tools/workerd-mutation-contract.mjs')),
    handlerObserverSHA256:fileDigest(path.join(root,'tests/helpers/ws_handler_errors.mjs')),
    sourceRunnerSHA256:fileDigest(path.join(root,'tests/test_workerd_ws_fixtures.mjs')),
    fixtures:fixtureIdentities()};
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-ws-mutations-'));
const fixtureDir = path.join(directory, 'fixtures/workerd-ws');
fs.mkdirSync(fixtureDir, {recursive: true});
const sourceDir = path.join(root, 'tests/fixtures/workerd-ws');
const source = fs.readFileSync(path.join(sourceDir, 'api-compat.mjs'), 'utf8');
const runner = prepareMutationRunner(fs.readFileSync(path.join(root, 'tests/test_workerd_ws_fixtures.mjs'), 'utf8'));
const runnerPath = path.join(directory, 'runner.mjs');
fs.writeFileSync(runnerPath, runner);
for (const file of [...fixtureIdentities().map(x => x.fixture), 'reference-lock.json', 'reference-lock-windows.json',
    'reference-lock-darwin-x64.json', 'reference-lock-darwin-arm64.json'])
    fs.copyFileSync(path.join(sourceDir, file), path.join(fixtureDir, file));
const replaceOnce = (from, to) => {
    assert.equal(source.split(from).length, 2, 'mutation anchor is unique');
    return source.replace(from, to);
};
const variants = [
    ['baseline', source, null],
    ['wire-code', replaceOnce('server.close(1000.5)', 'server.close(3001)'), /c03: actual wire close conversion/],
    ['wire-reason', replaceOnce('server.close(1000, 123n)', 'server.close(1000, "CORRUPTED")'), /e-close: actual wire close conversion/],
    ['throw-send-dropped', replaceOnce('server.send("queued");', ''), /c17: throw wire contract exact bytes/],
    ['throw-close-corrupted', replaceOnce('server.close(1000, "done");', 'server.close(3001, "done");'), /c18: throw wire contract exact bytes/],
    ['handler-error-microtask-dropped', replaceOnce("observe('nested-microtask');", ''),
        /c17\/Error: handler error contract exact microtask order/],
    ['handler-error-type-lost', replaceOnce("new (constructors[kind] ?? constructors.TypeError)('handler-error/' + scenario)",
        "new Error('handler-error/' + scenario)"), /c17\/TypeError: handler error contract exact microtask order/],
    ['handler-error-custom-name-lost', replaceOnce("thrown.name = 'CustomError'", "thrown.name = 'Error'"),
        /c17\/subclass: handler error contract exact microtask order/],
    ['handler-error-number-stringified', replaceOnce('thrown.message = 7', "thrown.message = '7'"),
        /c17\/number-message: handler error contract exact microtask order/],
    ['handler-error-message-getter-twice', replaceOnce("observeMessageRead(); return 'getter-message'",
        "observeMessageRead(); observeMessageRead(); return 'getter-message'"),
        /c17\/message-getter: handler error contract exact microtask order/],
    ['handler-error-replacement-timer-called', replaceOnce("case 'override-timer': globalThis.setTimeout",
        "case 'override-timer': overrideCalls.push('setTimeout'); globalThis.setTimeout"),
        /c17\/override-timer: handler error contract exact microtask order/],
    ['handler-error-listener-write-dropped', replaceOnce("attempt('error-send', () => server.send('from-error'))",
        "attempt('error-send', () => {})"), /c17\/error-send: handler error contract exact accepted wire bytes/],
    ['handler-error-conversion-skipped', replaceOnce("server.send(sendValue('error'))", "server.send('error-value')"),
        /c17\/error-send-object: handler error contract exact microtask order/],
    ['handler-error-getter-close-ignored', replaceOnce("else attempt('getter-close', () => server.close(1000, 'getter-done'))",
        "else attempt('getter-close', () => {})"), /c17\/getter-close: handler error contract exact microtask order/],
    ['handler-object-conversion-called', replaceOnce("case 'object': thrown =", "case 'object': conversionCalls.push('toString'); thrown ="),
        /c17\/object: handler error contract exact microtask order/],
    ['handler-object-shared-reference-lost', replaceOnce('thrown = {left: child, right: child}', 'thrown = {left: child, right: {x: 7}}'),
        /c17\/object-cycle: handler error contract exact microtask order/],
    ['handler-object-error-prototype', replaceOnce("observe('error', {message: event.message",
        "if (kind === 'object-fake-error') Object.setPrototypeOf(event.error, TypeError.prototype);\n        observe('error', {message: event.message"),
        /c17\/object-fake-error: handler error contract exact microtask order/],
    ['handler-dom-private-name-lost', replaceOnce("case 'dom-exception': thrown = new DOMException('dom-message', 'AbortError')",
        "case 'dom-exception': thrown = new DOMException('dom-message', 'NotFoundError')"),
        /c17\/dom-exception: handler error contract exact microtask order/],
    ['handler-aggregate-alias-lost', replaceOnce("new AggregateError([child, 'child', null], 'aggregate-message', {cause: child})",
        "new AggregateError([{x: 7}, 'child', null], 'aggregate-message', {cause: child})"),
        /c17\/aggregate-data: handler error contract exact microtask order/],
    ['handler-error-getter-order-lost', replaceOnce("conversionCalls.push('first')", "conversionCalls.push('second')"),
        /c17\/error-ordered-getters: handler error contract exact microtask order/],
    ['handler-error-original-stack-retained', replaceOnce('errorStack: event.error?.stack',
        "errorStack: kind === 'error-fixed-stack' ? 'fixed-stack' : event.error?.stack"),
        /c17\/error-fixed-stack: handler error contract exact microtask order/],
    ['handler-error-diagnostic-unlinked', replaceOnce('errorMessage: event.error?.message, errorText:',
        "errorMessage: kind === 'name-getter-throws' ? 'internal error; reference = 000000000000000000000000' : event.error?.message, errorText:"),
        /c17\/name-getter-throws: handler error contract exact microtask order/],
    ['send-surrogate-normalized', replaceOnce("'\\ud800', '\\udc00', '\\ud800\\ud801'",
        "'\\ufffd', '\\udc00', '\\ud800\\ud801'"), /DOMString wire message 4/],
    ['close-surrogate-corrupted', replaceOnce("server.close(1000, '\\ud800A\\udfffB\\udbff\\udfff')",
        "server.close(1000, 'CORRUPTED')"), /close reason uses USVString with the captured intrinsic/],
    ['getter-reads', `const OriginalCloseEvent = globalThis.CloseEvent;
globalThis.CloseEvent = class CloseEvent extends OriginalCloseEvent {
  constructor(type, init) { if (init) { void init.code; void init.reason; void init.wasClean; } super(...arguments); }
};\n` + source, /public event contract: CloseEvent\/reads/],
    ['error-payload', replaceOnce('socket.onerror = event => {',
        "socket.onerror = event => { Object.defineProperty(event, 'error', {value: 'error'});"),
        /refused dial carries the reference Error object/],
    ['event-phase', `Object.defineProperty(Event.prototype, 'eventPhase', { get() { return 2; } });\n` + source,
        /public event contract: Event\/defaults/],
    ['runtime-trust', replaceOnce('socket.onerror = event => {',
        "socket.onerror = event => { Object.defineProperty(event, 'isTrusted', {value: false});"),
        /refused dial carries the reference Error object/],
    ['handled-reason', replaceOnce('            handled = event;',
        "            handled = event; Object.defineProperty(event, 'reason', {value: reason});"),
        /runtime event delivery matches the pinned default workerd profile/],
    ['message-source', replaceOnce('messageEvent = event;',
        "messageEvent = event; Object.defineProperty(event, 'source', {value: channel.port1});"),
        /runtime event delivery matches the pinned default workerd profile/],
    ['cached-message-ports', `Object.defineProperty(MessageEvent.prototype, 'ports', {
  get() { return this._cachedPorts ??= []; }
});\n` + source, /public event contract: MessageEvent\/ports-undefined/],
    ['listener-exception-swallowed', `const originalDispatch = EventTarget.prototype.dispatchEvent;
EventTarget.prototype.dispatchEvent = function(event) {
  try { return originalDispatch.call(this, event); }
  catch (error) { if (event?.type === 'message') return true; throw error; }
};\n` + source, /listener contract: EventTarget\/exception/],
    ['listener-removal-ignored', `const originalRemove = EventTarget.prototype.removeEventListener;
EventTarget.prototype.removeEventListener = function(type, ...args) {
  if (type !== 'message') return originalRemove.call(this, type, ...args);
};\n` + source, /listener contract: EventTarget\/remove-next-once/],
    ['listener-option-reread', `const originalAdd = EventTarget.prototype.addEventListener;
EventTarget.prototype.addEventListener = function(type, listener, options) {
  if (options && typeof options === 'object') void options.once;
  return originalAdd.call(this, type, listener, options);
};\n` + source, /listener contract: EventTarget\/option-read-order/],
    ['handler-order', `const originalHandlerDispatch = EventTarget.prototype.dispatchEvent;
EventTarget.prototype.dispatchEvent = function(event) {
  const callback = this.onopen;
  if (!(this instanceof WebSocket) || event?.type !== 'open' || typeof callback !== 'function')
    return originalHandlerDispatch.call(this,event);
  this.onopen = null;
  try { const result = originalHandlerDispatch.call(this,event); callback.call(globalThis,event); return result; }
  finally { this.onopen = callback; }
};\n` + source, /event handler contract: WebSocket\/open\/replace/],
    ['handler-clear-removes-listener', `const originalMessageHandler = Object.getOwnPropertyDescriptor(MessagePort.prototype,'onmessage');
Object.defineProperty(MessagePort.prototype,'onmessage',{...originalMessageHandler,set(value){
  const previous = originalMessageHandler.get.call(this);
  originalMessageHandler.set.call(this,value);
  if (value === null && typeof previous === 'function') this.removeEventListener('message',previous);
}});\n` + source, /event handler contract: MessagePort\/message\/clear/],
];
const results = [];
const digest = text => createHash('sha256').update(text).digest('hex');
const reportPath = args.includes('--report') ? option('--report')
    : process.env.CAPSID_WS_REPORT_DIR
        ? path.join(process.env.CAPSID_WS_REPORT_DIR, 'mutation-controls.json') : null;
try {
    for (const [name, fixture, expectedError] of variants) {
        fs.writeFileSync(path.join(fixtureDir, 'api-compat.mjs'), fixture);
        const program = prepareMutationRunner(fs.readFileSync(path.join(root, 'tests/test_workerd_ws_fixtures.mjs'), 'utf8'), name);
        const handlerErrorCase = expectedError?.source.includes(': handler error contract')
            ? expectedError.source.split(': handler error contract')[0].replaceAll('\\/', '/') : null;
        if (handlerErrorCase) assert.ok(program.includes('assert,' + JSON.stringify(handlerErrorCase) + ')'),
            name + ': generated driver executes the diagnostic case');
        fs.writeFileSync(runnerPath, program);
        const env = {...process.env, CAPSID_WDREF_FILTER: 'api-compat'};
        delete env.CAPSID_WS_REPORT_DIR;
        const driverReportPath = path.join(directory, name+'.json');
        const result = spawnSync(process.execPath, [runnerPath, '--host', host,
            '--worker', worker, '--mode', 'single-worker', '--report', driverReportPath],
            {env, encoding: 'utf8', timeout: 25000, maxBuffer: 4 * 1024 * 1024});
        const output = String(result.stdout ?? '') + String(result.stderr ?? '');
        assert.equal(result.error, undefined, `${name}: driver completed: ${result.error}`);
        assert.equal(result.signal, null, `${name}: no forced termination`);
        assert.equal(result.status, expectedError ? 1 : 0, `${name}: ${output}`);
        if (expectedError) assert.match(output, expectedError, `${name}: intended assertion detects the fault`);
        results.push({name, exit: result.status, detected: !!expectedError,
            handlerErrorCase, runnerSHA256:digest(program),
            signal:result.signal, error:result.error ?? null,
            fixtureSHA256: digest(fixture), expectedDiagnostic: expectedError?.source ?? null,
            stdout: result.stdout, stderr: result.stderr,
            ...(!expectedError ? {driverReport:JSON.parse(fs.readFileSync(driverReportPath,'utf8'))} : {})});
        console.log(`${name}: ${expectedError ? 'detected' : 'passed'}`);
    }
    if (reportPath) fs.writeFileSync(reportPath, JSON.stringify({...identity, complete: true,
        fixtureSHA256: digest(source), runnerSHA256: digest(runner), results}, null, 2));
} finally { fs.rmSync(directory, {recursive: true, force: true}); }
