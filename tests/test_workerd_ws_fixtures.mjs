// workerd WebSocket fixture runner (v0.3.0 WS-A/T04).
//
// Two branches:
//  - REFERENCE (workerd): with --workerd <binary> the runner serves each
//    fixture on the locked reference (tests/fixtures/workerd-ws/
//    reference-lock.json binds the identity) and drives it with Node's
//    native WebSocket client, asserting each /selftest contract for
//    real. The reference branch never skips.
//  - CAPSID: the same fixtures run against all three real Host backends.
//    The required comparison rejects missing features, cases and skips.
import nativeAssert from 'node:assert/strict';
import { startCapsid, stopCapsid, withCleanup } from './helpers/ws_host_backend.mjs';
import { responseSession } from './helpers/ws_response_client.mjs';
import { observeHandlerErrors } from './helpers/ws_handler_errors.mjs';
import { assertHandlerErrorDiagnostics } from '../tools/workerd-handler-error-contract.mjs';
import { fixtureIdentities, observationExceptions, assertResponseConstructors, assertResponseObservation, assertHandlerThrowCase, readLock, lockPath } from '../tools/workerd-reference.mjs';
import { execFileSync } from 'node:child_process';
let assertionCount = 0;
const assert = new Proxy(nativeAssert, { get(target, name) {
    const value = target[name];
    return typeof value === 'function' ? (...args) => { assertionCount++; return value(...args); } : value;
} });
import fs from 'node:fs';
import net from 'node:net';
import http from 'node:http';
import crypto from 'node:crypto';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import url from 'node:url';

// Reviewed public event outcomes from workerd 8f311ae3, date 2026-08-20, flags [].
// Kept in the driver so its provenance digest also binds the golden contract.
// Same-input observations from the pinned workerd binary; never derived from Capsid.
const eventHandlerContract = [
    {"name":"WebSocket/open/replace","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/open/reassign","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/open/duplicate","trace":["same","same","|","same","same"],"error":null},
    {"name":"WebSocket/open/remove","trace":["handler","|","handler"],"error":null},
    {"name":"WebSocket/open/clear","trace":["listener","|","listener"],"error":null},
    {"name":"WebSocket/open/reset","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/open/once","trace":["same","same","|","same"],"error":null},
    {"name":"WebSocket/open/nonfunction","trace":["object","|"],"error":null},
    {"name":"WebSocket/open/initial","trace":[{"isNull":false,"kind":"undefined"},"|"],"error":null},
    {"name":"WebSocket/open/null-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/open/object-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/open/replace-during-dispatch","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/open/clear-during-dispatch","trace":["old","listener","|","listener"],"error":null},
    {"name":"WebSocket/open/reset-during-dispatch","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/open/self-replace","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/open/receiver","trace":[{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2},"|",{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2}],"error":null},
    {"name":"WebSocket/open/override-add-remove","trace":["handler","|","handler"],"error":null},
    {"name":"WebSocket/open/handler-throw","trace":["handler"],"error":"TypeError"},
    {"name":"WebSocket/open/returns-true","trace":[{"returned":false,"canceled":true},"|"],"error":null},
    {"name":"WebSocket/open/returns-false","trace":[{"returned":true,"canceled":false},"|"],"error":null},
    {"name":"WebSocket/open/own-value","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/open/own-getter","trace":["get","own","|","get","own"],"error":null},
    {"name":"WebSocket/open/delete-property","trace":[true,"undefined","|"],"error":null},
    {"name":"WebSocket/open/descriptors","trace":[null,null,{"enumerable":true,"configurable":true,"writable":true,"get":"undefined","set":"undefined"},null,"|"],"error":null},
    {"name":"WebSocket/open/own-value-after-assignment","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/open/own-value-after-clear","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/open/getter-adds-listener","trace":["get","own","added","|","get","own","added"],"error":null},
    {"name":"WebSocket/open/getter-throws","trace":[{"error":"TypeError","phase":0,"current":true},"|"],"error":null},
    {"name":"WebSocket/open/assigned-null","trace":[{"same":true,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"WebSocket/open/assigned-undefined","trace":[{"same":true,"kind":"undefined","isNull":false},"|"],"error":null},
    {"name":"WebSocket/open/assigned-number","trace":[{"same":true,"kind":"number","isNull":false},"|"],"error":null},
    {"name":"WebSocket/open/assigned-false","trace":[{"same":true,"kind":"boolean","isNull":false},"|"],"error":null},
    {"name":"WebSocket/open/assigned-string","trace":[{"same":true,"kind":"string","isNull":false},"|"],"error":null},
    {"name":"WebSocket/open/assigned-symbol","trace":[{"same":true,"kind":"symbol","isNull":false},"|"],"error":null},
    {"name":"WebSocket/open/assigned-bigint","trace":[{"same":true,"kind":"bigint","isNull":false},"|"],"error":null},
    {"name":"WebSocket/open/assigned-object","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"WebSocket/open/assigned-handleEvent","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"WebSocket/message/replace","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/message/reassign","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/message/duplicate","trace":["same","same","|","same","same"],"error":null},
    {"name":"WebSocket/message/remove","trace":["handler","|","handler"],"error":null},
    {"name":"WebSocket/message/clear","trace":["listener","|","listener"],"error":null},
    {"name":"WebSocket/message/reset","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/message/once","trace":["same","same","|","same"],"error":null},
    {"name":"WebSocket/message/nonfunction","trace":["object","|"],"error":null},
    {"name":"WebSocket/message/initial","trace":[{"isNull":false,"kind":"undefined"},"|"],"error":null},
    {"name":"WebSocket/message/null-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/message/object-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/message/replace-during-dispatch","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/message/clear-during-dispatch","trace":["old","listener","|","listener"],"error":null},
    {"name":"WebSocket/message/reset-during-dispatch","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/message/self-replace","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/message/receiver","trace":[{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2},"|",{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2}],"error":null},
    {"name":"WebSocket/message/override-add-remove","trace":["handler","|","handler"],"error":null},
    {"name":"WebSocket/message/handler-throw","trace":["handler"],"error":"TypeError"},
    {"name":"WebSocket/message/returns-true","trace":[{"returned":false,"canceled":true},"|"],"error":null},
    {"name":"WebSocket/message/returns-false","trace":[{"returned":true,"canceled":false},"|"],"error":null},
    {"name":"WebSocket/message/own-value","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/message/own-getter","trace":["get","own","|","get","own"],"error":null},
    {"name":"WebSocket/message/delete-property","trace":[true,"undefined","|"],"error":null},
    {"name":"WebSocket/message/descriptors","trace":[null,null,{"enumerable":true,"configurable":true,"writable":true,"get":"undefined","set":"undefined"},null,"|"],"error":null},
    {"name":"WebSocket/message/own-value-after-assignment","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/message/own-value-after-clear","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/message/getter-adds-listener","trace":["get","own","added","|","get","own","added"],"error":null},
    {"name":"WebSocket/message/getter-throws","trace":[{"error":"TypeError","phase":0,"current":true},"|"],"error":null},
    {"name":"WebSocket/message/assigned-null","trace":[{"same":true,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"WebSocket/message/assigned-undefined","trace":[{"same":true,"kind":"undefined","isNull":false},"|"],"error":null},
    {"name":"WebSocket/message/assigned-number","trace":[{"same":true,"kind":"number","isNull":false},"|"],"error":null},
    {"name":"WebSocket/message/assigned-false","trace":[{"same":true,"kind":"boolean","isNull":false},"|"],"error":null},
    {"name":"WebSocket/message/assigned-string","trace":[{"same":true,"kind":"string","isNull":false},"|"],"error":null},
    {"name":"WebSocket/message/assigned-symbol","trace":[{"same":true,"kind":"symbol","isNull":false},"|"],"error":null},
    {"name":"WebSocket/message/assigned-bigint","trace":[{"same":true,"kind":"bigint","isNull":false},"|"],"error":null},
    {"name":"WebSocket/message/assigned-object","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"WebSocket/message/assigned-handleEvent","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"WebSocket/error/replace","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/error/reassign","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/error/duplicate","trace":["same","same","|","same","same"],"error":null},
    {"name":"WebSocket/error/remove","trace":["handler","|","handler"],"error":null},
    {"name":"WebSocket/error/clear","trace":["listener","|","listener"],"error":null},
    {"name":"WebSocket/error/reset","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/error/once","trace":["same","same","|","same"],"error":null},
    {"name":"WebSocket/error/nonfunction","trace":["object","|"],"error":null},
    {"name":"WebSocket/error/initial","trace":[{"isNull":false,"kind":"undefined"},"|"],"error":null},
    {"name":"WebSocket/error/null-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/error/object-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/error/replace-during-dispatch","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/error/clear-during-dispatch","trace":["old","listener","|","listener"],"error":null},
    {"name":"WebSocket/error/reset-during-dispatch","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/error/self-replace","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/error/receiver","trace":[{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2},"|",{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2}],"error":null},
    {"name":"WebSocket/error/override-add-remove","trace":["handler","|","handler"],"error":null},
    {"name":"WebSocket/error/handler-throw","trace":["handler"],"error":"TypeError"},
    {"name":"WebSocket/error/returns-true","trace":[{"returned":false,"canceled":true},"|"],"error":null},
    {"name":"WebSocket/error/returns-false","trace":[{"returned":true,"canceled":false},"|"],"error":null},
    {"name":"WebSocket/error/own-value","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/error/own-getter","trace":["get","own","|","get","own"],"error":null},
    {"name":"WebSocket/error/delete-property","trace":[true,"undefined","|"],"error":null},
    {"name":"WebSocket/error/descriptors","trace":[null,null,{"enumerable":true,"configurable":true,"writable":true,"get":"undefined","set":"undefined"},null,"|"],"error":null},
    {"name":"WebSocket/error/own-value-after-assignment","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/error/own-value-after-clear","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/error/getter-adds-listener","trace":["get","own","added","|","get","own","added"],"error":null},
    {"name":"WebSocket/error/getter-throws","trace":[{"error":"TypeError","phase":0,"current":true},"|"],"error":null},
    {"name":"WebSocket/error/assigned-null","trace":[{"same":true,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"WebSocket/error/assigned-undefined","trace":[{"same":true,"kind":"undefined","isNull":false},"|"],"error":null},
    {"name":"WebSocket/error/assigned-number","trace":[{"same":true,"kind":"number","isNull":false},"|"],"error":null},
    {"name":"WebSocket/error/assigned-false","trace":[{"same":true,"kind":"boolean","isNull":false},"|"],"error":null},
    {"name":"WebSocket/error/assigned-string","trace":[{"same":true,"kind":"string","isNull":false},"|"],"error":null},
    {"name":"WebSocket/error/assigned-symbol","trace":[{"same":true,"kind":"symbol","isNull":false},"|"],"error":null},
    {"name":"WebSocket/error/assigned-bigint","trace":[{"same":true,"kind":"bigint","isNull":false},"|"],"error":null},
    {"name":"WebSocket/error/assigned-object","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"WebSocket/error/assigned-handleEvent","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"WebSocket/close/replace","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/close/reassign","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/close/duplicate","trace":["same","same","|","same","same"],"error":null},
    {"name":"WebSocket/close/remove","trace":["handler","|","handler"],"error":null},
    {"name":"WebSocket/close/clear","trace":["listener","|","listener"],"error":null},
    {"name":"WebSocket/close/reset","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/close/once","trace":["same","same","|","same"],"error":null},
    {"name":"WebSocket/close/nonfunction","trace":["object","|"],"error":null},
    {"name":"WebSocket/close/initial","trace":[{"isNull":false,"kind":"undefined"},"|"],"error":null},
    {"name":"WebSocket/close/null-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/close/object-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"WebSocket/close/replace-during-dispatch","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/close/clear-during-dispatch","trace":["old","listener","|","listener"],"error":null},
    {"name":"WebSocket/close/reset-during-dispatch","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/close/self-replace","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"WebSocket/close/receiver","trace":[{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2},"|",{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2}],"error":null},
    {"name":"WebSocket/close/override-add-remove","trace":["handler","|","handler"],"error":null},
    {"name":"WebSocket/close/handler-throw","trace":["handler"],"error":"TypeError"},
    {"name":"WebSocket/close/returns-true","trace":[{"returned":false,"canceled":true},"|"],"error":null},
    {"name":"WebSocket/close/returns-false","trace":[{"returned":true,"canceled":false},"|"],"error":null},
    {"name":"WebSocket/close/own-value","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/close/own-getter","trace":["get","own","|","get","own"],"error":null},
    {"name":"WebSocket/close/delete-property","trace":[true,"undefined","|"],"error":null},
    {"name":"WebSocket/close/descriptors","trace":[null,null,{"enumerable":true,"configurable":true,"writable":true,"get":"undefined","set":"undefined"},null,"|"],"error":null},
    {"name":"WebSocket/close/own-value-after-assignment","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/close/own-value-after-clear","trace":["own","|","own"],"error":null},
    {"name":"WebSocket/close/getter-adds-listener","trace":["get","own","added","|","get","own","added"],"error":null},
    {"name":"WebSocket/close/getter-throws","trace":[{"error":"TypeError","phase":0,"current":true},"|"],"error":null},
    {"name":"WebSocket/close/assigned-null","trace":[{"same":true,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"WebSocket/close/assigned-undefined","trace":[{"same":true,"kind":"undefined","isNull":false},"|"],"error":null},
    {"name":"WebSocket/close/assigned-number","trace":[{"same":true,"kind":"number","isNull":false},"|"],"error":null},
    {"name":"WebSocket/close/assigned-false","trace":[{"same":true,"kind":"boolean","isNull":false},"|"],"error":null},
    {"name":"WebSocket/close/assigned-string","trace":[{"same":true,"kind":"string","isNull":false},"|"],"error":null},
    {"name":"WebSocket/close/assigned-symbol","trace":[{"same":true,"kind":"symbol","isNull":false},"|"],"error":null},
    {"name":"WebSocket/close/assigned-bigint","trace":[{"same":true,"kind":"bigint","isNull":false},"|"],"error":null},
    {"name":"WebSocket/close/assigned-object","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"WebSocket/close/assigned-handleEvent","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"MessagePort/message/replace","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"MessagePort/message/reassign","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"MessagePort/message/duplicate","trace":["same","same","|","same","same"],"error":null},
    {"name":"MessagePort/message/remove","trace":["handler","|","handler"],"error":null},
    {"name":"MessagePort/message/clear","trace":["listener","|","listener"],"error":null},
    {"name":"MessagePort/message/reset","trace":["listener","new","|","listener","new"],"error":null},
    {"name":"MessagePort/message/once","trace":["same","same","|","same"],"error":null},
    {"name":"MessagePort/message/nonfunction","trace":["object","|"],"error":null},
    {"name":"MessagePort/message/initial","trace":[{"isNull":true,"kind":"object"},"|"],"error":null},
    {"name":"MessagePort/message/null-first","trace":["listener","handler","|","listener","handler"],"error":null},
    {"name":"MessagePort/message/object-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"MessagePort/message/replace-during-dispatch","trace":["listener","new","|","listener","new"],"error":null},
    {"name":"MessagePort/message/clear-during-dispatch","trace":["listener","|","listener"],"error":null},
    {"name":"MessagePort/message/reset-during-dispatch","trace":["listener","|","listener"],"error":null},
    {"name":"MessagePort/message/self-replace","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"MessagePort/message/receiver","trace":[{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2},"|",{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2}],"error":null},
    {"name":"MessagePort/message/override-add-remove","trace":["handler","|","handler"],"error":null},
    {"name":"MessagePort/message/handler-throw","trace":["handler"],"error":"TypeError"},
    {"name":"MessagePort/message/returns-true","trace":[{"returned":false,"canceled":true},"|"],"error":null},
    {"name":"MessagePort/message/returns-false","trace":[{"returned":true,"canceled":false},"|"],"error":null},
    {"name":"MessagePort/message/own-value","trace":["own","|","own"],"error":null},
    {"name":"MessagePort/message/own-getter","trace":["get","own","|","get","own"],"error":null},
    {"name":"MessagePort/message/delete-property","trace":[true,"function","handler","|","handler"],"error":null},
    {"name":"MessagePort/message/descriptors","trace":[null,{"enumerable":true,"configurable":true,"writable":null,"get":"function","set":"function"},null,{"enumerable":true,"configurable":true,"writable":null,"get":"function","set":"function"},"|"],"error":null},
    {"name":"MessagePort/message/own-value-after-assignment","trace":["registered","|","registered"],"error":null},
    {"name":"MessagePort/message/own-value-after-clear","trace":["|"],"error":null},
    {"name":"MessagePort/message/getter-adds-listener","trace":["get","own","added","|","get","own","added"],"error":null},
    {"name":"MessagePort/message/getter-throws","trace":[{"error":"TypeError","phase":0,"current":true},"|"],"error":null},
    {"name":"MessagePort/message/assigned-null","trace":[{"same":true,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"MessagePort/message/assigned-undefined","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"MessagePort/message/assigned-number","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"MessagePort/message/assigned-false","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"MessagePort/message/assigned-string","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"MessagePort/message/assigned-symbol","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"MessagePort/message/assigned-bigint","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"MessagePort/message/assigned-object","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"MessagePort/message/assigned-handleEvent","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"EventTarget/message/replace","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"EventTarget/message/reassign","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"EventTarget/message/duplicate","trace":["same","same","|","same","same"],"error":null},
    {"name":"EventTarget/message/remove","trace":["handler","|","handler"],"error":null},
    {"name":"EventTarget/message/clear","trace":["listener","|","listener"],"error":null},
    {"name":"EventTarget/message/reset","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"EventTarget/message/once","trace":["same","same","|","same"],"error":null},
    {"name":"EventTarget/message/nonfunction","trace":["object","|"],"error":null},
    {"name":"EventTarget/message/initial","trace":[{"isNull":false,"kind":"undefined"},"|"],"error":null},
    {"name":"EventTarget/message/null-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"EventTarget/message/object-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"EventTarget/message/replace-during-dispatch","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"EventTarget/message/clear-during-dispatch","trace":["old","listener","|","listener"],"error":null},
    {"name":"EventTarget/message/reset-during-dispatch","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"EventTarget/message/self-replace","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"EventTarget/message/receiver","trace":[{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2},"|",{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2}],"error":null},
    {"name":"EventTarget/message/override-add-remove","trace":["handler","|","handler"],"error":null},
    {"name":"EventTarget/message/handler-throw","trace":["handler"],"error":"TypeError"},
    {"name":"EventTarget/message/returns-true","trace":[{"returned":false,"canceled":true},"|"],"error":null},
    {"name":"EventTarget/message/returns-false","trace":[{"returned":true,"canceled":false},"|"],"error":null},
    {"name":"EventTarget/message/own-value","trace":["own","|","own"],"error":null},
    {"name":"EventTarget/message/own-getter","trace":["get","own","|","get","own"],"error":null},
    {"name":"EventTarget/message/delete-property","trace":[true,"undefined","|"],"error":null},
    {"name":"EventTarget/message/descriptors","trace":[null,null,{"enumerable":true,"configurable":true,"writable":true,"get":"undefined","set":"undefined"},null,"|"],"error":null},
    {"name":"EventTarget/message/own-value-after-assignment","trace":["own","|","own"],"error":null},
    {"name":"EventTarget/message/own-value-after-clear","trace":["own","|","own"],"error":null},
    {"name":"EventTarget/message/getter-adds-listener","trace":["get","own","added","|","get","own","added"],"error":null},
    {"name":"EventTarget/message/getter-throws","trace":[{"error":"TypeError","phase":0,"current":true},"|"],"error":null},
    {"name":"EventTarget/message/assigned-null","trace":[{"same":true,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"EventTarget/message/assigned-undefined","trace":[{"same":true,"kind":"undefined","isNull":false},"|"],"error":null},
    {"name":"EventTarget/message/assigned-number","trace":[{"same":true,"kind":"number","isNull":false},"|"],"error":null},
    {"name":"EventTarget/message/assigned-false","trace":[{"same":true,"kind":"boolean","isNull":false},"|"],"error":null},
    {"name":"EventTarget/message/assigned-string","trace":[{"same":true,"kind":"string","isNull":false},"|"],"error":null},
    {"name":"EventTarget/message/assigned-symbol","trace":[{"same":true,"kind":"symbol","isNull":false},"|"],"error":null},
    {"name":"EventTarget/message/assigned-bigint","trace":[{"same":true,"kind":"bigint","isNull":false},"|"],"error":null},
    {"name":"EventTarget/message/assigned-object","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"EventTarget/message/assigned-handleEvent","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"AbortSignal/abort/replace","trace":["new","listener","|","new","listener"],"error":null},
    {"name":"AbortSignal/abort/reassign","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"AbortSignal/abort/duplicate","trace":["same","same","|","same","same"],"error":null},
    {"name":"AbortSignal/abort/remove","trace":["handler","|","handler"],"error":null},
    {"name":"AbortSignal/abort/clear","trace":["listener","|","listener"],"error":null},
    {"name":"AbortSignal/abort/reset","trace":["listener","new","|","listener","new"],"error":null},
    {"name":"AbortSignal/abort/once","trace":["same","same","|","same"],"error":null},
    {"name":"AbortSignal/abort/nonfunction","trace":["object","|"],"error":null},
    {"name":"AbortSignal/abort/initial","trace":[{"isNull":true,"kind":"object"},"|"],"error":null},
    {"name":"AbortSignal/abort/null-first","trace":["listener","handler","|","listener","handler"],"error":null},
    {"name":"AbortSignal/abort/object-first","trace":["handler","listener","|","handler","listener"],"error":null},
    {"name":"AbortSignal/abort/replace-during-dispatch","trace":["listener","new","|","listener","new"],"error":null},
    {"name":"AbortSignal/abort/clear-during-dispatch","trace":["listener","|","listener"],"error":null},
    {"name":"AbortSignal/abort/reset-during-dispatch","trace":["listener","|","listener"],"error":null},
    {"name":"AbortSignal/abort/self-replace","trace":["old","listener","|","new","listener"],"error":null},
    {"name":"AbortSignal/abort/receiver","trace":[{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2},"|",{"self":false,"undefinedReceiver":false,"globalReceiver":true,"target":true,"current":true,"phase":2}],"error":null},
    {"name":"AbortSignal/abort/override-add-remove","trace":["handler","|","handler"],"error":null},
    {"name":"AbortSignal/abort/handler-throw","trace":["handler"],"error":"TypeError"},
    {"name":"AbortSignal/abort/returns-true","trace":[{"returned":false,"canceled":true},"|"],"error":null},
    {"name":"AbortSignal/abort/returns-false","trace":[{"returned":true,"canceled":false},"|"],"error":null},
    {"name":"AbortSignal/abort/own-value","trace":["own","|","own"],"error":null},
    {"name":"AbortSignal/abort/own-getter","trace":["get","own","|","get","own"],"error":null},
    {"name":"AbortSignal/abort/delete-property","trace":[true,"function","handler","|","handler"],"error":null},
    {"name":"AbortSignal/abort/descriptors","trace":[null,{"enumerable":true,"configurable":true,"writable":null,"get":"function","set":"function"},null,{"enumerable":true,"configurable":true,"writable":null,"get":"function","set":"function"},"|"],"error":null},
    {"name":"AbortSignal/abort/own-value-after-assignment","trace":["registered","|","registered"],"error":null},
    {"name":"AbortSignal/abort/own-value-after-clear","trace":["|"],"error":null},
    {"name":"AbortSignal/abort/getter-adds-listener","trace":["get","own","added","|","get","own","added"],"error":null},
    {"name":"AbortSignal/abort/getter-throws","trace":[{"error":"TypeError","phase":0,"current":true},"|"],"error":null},
    {"name":"AbortSignal/abort/assigned-null","trace":[{"same":true,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"AbortSignal/abort/assigned-undefined","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"AbortSignal/abort/assigned-number","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"AbortSignal/abort/assigned-false","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"AbortSignal/abort/assigned-string","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"AbortSignal/abort/assigned-symbol","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"AbortSignal/abort/assigned-bigint","trace":[{"same":false,"kind":"object","isNull":true},"|"],"error":null},
    {"name":"AbortSignal/abort/assigned-object","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null},
    {"name":"AbortSignal/abort/assigned-handleEvent","trace":[{"same":true,"kind":"object","isNull":false},"|"],"error":null}
];

const listenerEventContract = {
    "EventTarget/exception": {"ok":true,"value":{"sameError":true,"first":["first"],"after":{"phase":0,"current":true,"canceled":true,"path":0},"second":["second"],"returned":true,"canceled":false}},
    "EventTarget/object-exception": {"ok":true,"value":{"sameError":true,"receiver":true,"reported":0,"phase":0}},
    "EventTarget/remove-next-once": {"ok":true,"value":["first"]},
    "EventTarget/remove-next": {"ok":true,"value":["first"]},
    "EventTarget/add-next": {"ok":true,"value":{"first":["first","second"],"second":["first","second","added"]}},
    "EventTarget/remove-readd": {"ok":true,"value":{"first":["first","second"],"second":["second"]}},
    "EventTarget/self-remove-readd": {"ok":true,"value":{"initial":["first","second"],"next":["second","first"]}},
    "EventTarget/nested-once": {"ok":true,"value":{"order":[["first",true,2],["second",false,2],["resumed",2,0],["second",true,2]],"phases":[0,0]}},
    "EventTarget/nested-add-remove": {"ok":true,"value":["first","inner-added"]},
    "EventTarget/duplicate-once": {"ok":true,"value":1},
    "EventTarget/stop-immediate-redispatch": {"ok":true,"value":{"order":["first"],"returns":[false,true]}},
    "EventTarget/stop-propagation": {"ok":true,"value":["first","second"]},
    "EventTarget/passive-cancel": {"ok":true,"value":{"stage":"registration","error":"TypeError"}},
    "EventTarget/capture-add": {"ok":true,"value":{"errors":["TypeError","TypeError"],"calls":0}},
    "EventTarget/capture-remove": {"ok":true,"value":{"errors":["TypeError","TypeError"],"calls":1}},
    "EventTarget/handleEvent-snapshot": {"ok":true,"value":["original"]},
    "EventTarget/return-true": {"ok":true,"value":{"returned":false,"canceled":true}},
    "EventTarget/abort-before-turn": {"ok":true,"value":["first"]},
    "EventTarget/already-aborted": {"ok":true,"value":0},
    "EventTarget/abort-after-readd": {"ok":true,"value":1},
    "EventTarget/abort-stopped": {"ok":true,"value":0},
    "EventTarget/option-read-order": {"ok":true,"value":["capture","passive","once","signal","capture"]},
    "WebSocket/exception": {"ok":true,"value":{"sameError":true,"first":["first"],"after":{"phase":0,"current":true,"canceled":true,"path":0},"second":["second"],"returned":true,"canceled":false}},
    "WebSocket/object-exception": {"ok":true,"value":{"sameError":true,"receiver":true,"reported":0,"phase":0}},
    "WebSocket/remove-next-once": {"ok":true,"value":["first"]},
    "WebSocket/remove-next": {"ok":true,"value":["first"]},
    "WebSocket/add-next": {"ok":true,"value":{"first":["first","second"],"second":["first","second","added"]}},
    "WebSocket/remove-readd": {"ok":true,"value":{"first":["first","second"],"second":["second"]}},
    "WebSocket/self-remove-readd": {"ok":true,"value":{"initial":["first","second"],"next":["second","first"]}},
    "WebSocket/nested-once": {"ok":true,"value":{"order":[["first",true,2],["second",false,2],["resumed",2,0],["second",true,2]],"phases":[0,0]}},
    "WebSocket/nested-add-remove": {"ok":true,"value":["first","inner-added"]},
    "WebSocket/duplicate-once": {"ok":true,"value":1},
    "WebSocket/stop-immediate-redispatch": {"ok":true,"value":{"order":["first"],"returns":[false,true]}},
    "WebSocket/stop-propagation": {"ok":true,"value":["first","second"]},
    "WebSocket/passive-cancel": {"ok":true,"value":{"stage":"registration","error":"TypeError"}},
    "WebSocket/capture-add": {"ok":true,"value":{"errors":["TypeError","TypeError"],"calls":0}},
    "WebSocket/capture-remove": {"ok":true,"value":{"errors":["TypeError","TypeError"],"calls":1}},
    "WebSocket/handleEvent-snapshot": {"ok":true,"value":["original"]},
    "WebSocket/return-true": {"ok":true,"value":{"returned":false,"canceled":true}},
    "WebSocket/abort-before-turn": {"ok":true,"value":["first"]},
    "WebSocket/already-aborted": {"ok":true,"value":0},
    "WebSocket/abort-after-readd": {"ok":true,"value":1},
    "WebSocket/abort-stopped": {"ok":true,"value":0},
    "WebSocket/option-read-order": {"ok":true,"value":["capture","passive","once","signal","capture"]},
    "MessagePort/exception": {"ok":true,"value":{"sameError":true,"first":["first"],"after":{"phase":0,"current":true,"canceled":true,"path":0},"second":["second"],"returned":true,"canceled":false}},
    "MessagePort/object-exception": {"ok":true,"value":{"sameError":true,"receiver":true,"reported":0,"phase":0}},
    "MessagePort/remove-next-once": {"ok":true,"value":["first"]},
    "MessagePort/remove-next": {"ok":true,"value":["first"]},
    "MessagePort/add-next": {"ok":true,"value":{"first":["first","second"],"second":["first","second","added"]}},
    "MessagePort/remove-readd": {"ok":true,"value":{"first":["first","second"],"second":["second"]}},
    "MessagePort/self-remove-readd": {"ok":true,"value":{"initial":["first","second"],"next":["second","first"]}},
    "MessagePort/nested-once": {"ok":true,"value":{"order":[["first",true,2],["second",false,2],["resumed",2,0],["second",true,2]],"phases":[0,0]}},
    "MessagePort/nested-add-remove": {"ok":true,"value":["first","inner-added"]},
    "MessagePort/duplicate-once": {"ok":true,"value":1},
    "MessagePort/stop-immediate-redispatch": {"ok":true,"value":{"order":["first"],"returns":[false,true]}},
    "MessagePort/stop-propagation": {"ok":true,"value":["first","second"]},
    "MessagePort/passive-cancel": {"ok":true,"value":{"stage":"registration","error":"TypeError"}},
    "MessagePort/capture-add": {"ok":true,"value":{"errors":["TypeError","TypeError"],"calls":0}},
    "MessagePort/capture-remove": {"ok":true,"value":{"errors":["TypeError","TypeError"],"calls":1}},
    "MessagePort/handleEvent-snapshot": {"ok":true,"value":["original"]},
    "MessagePort/return-true": {"ok":true,"value":{"returned":false,"canceled":true}},
    "MessagePort/abort-before-turn": {"ok":true,"value":["first"]},
    "MessagePort/already-aborted": {"ok":true,"value":0},
    "MessagePort/abort-after-readd": {"ok":true,"value":1},
    "MessagePort/abort-stopped": {"ok":true,"value":0},
    "MessagePort/option-read-order": {"ok":true,"value":["capture","passive","once","signal","capture"]},
};

const publicEventContract = {
    "Event/synchronous-dispatch": {"ok":true,"value":["first","second","returned"]},
    "Event/missing": {"ok":false,"error":"TypeError"},
    "Event/defaults": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object Event]"}},
    "Event/null": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object Event]"}},
    "Event/symbol-type": {"ok":false,"error":"TypeError"},
    "Event/surrogate-type": {"ok":true,"value":"\ufffd\ufffd\ufffd"},
    "Event/init-false": {"ok":false,"error":"TypeError"},
    "Event/init-zero": {"ok":false,"error":"TypeError"},
    "Event/init-string": {"ok":false,"error":"TypeError"},
    "Event/init-symbol": {"ok":false,"error":"TypeError"},
    "Event/init-function": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object Event]"}},
    "ErrorEvent/missing": {"ok":false,"error":"TypeError"},
    "ErrorEvent/defaults": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object ErrorEvent]","message":"","filename":"","lineno":0,"colno":0,"error":{"undefined":true}}},
    "ErrorEvent/null": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object ErrorEvent]","message":"","filename":"","lineno":0,"colno":0,"error":{"undefined":true}}},
    "ErrorEvent/symbol-type": {"ok":false,"error":"TypeError"},
    "ErrorEvent/surrogate-type": {"ok":true,"value":"\ufffd\ufffd\ufffd"},
    "ErrorEvent/init-false": {"ok":false,"error":"TypeError"},
    "ErrorEvent/init-zero": {"ok":false,"error":"TypeError"},
    "ErrorEvent/init-string": {"ok":false,"error":"TypeError"},
    "ErrorEvent/init-symbol": {"ok":false,"error":"TypeError"},
    "ErrorEvent/init-function": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object ErrorEvent]"}},
    "MessageEvent/missing": {"ok":false,"error":"TypeError"},
    "MessageEvent/defaults": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object MessageEvent]","data":null,"origin":"","lastEventId":"","source":null,"ports":[]}},
    "MessageEvent/null": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object MessageEvent]","data":null,"origin":"","lastEventId":"","source":null,"ports":[]}},
    "MessageEvent/symbol-type": {"ok":false,"error":"TypeError"},
    "MessageEvent/surrogate-type": {"ok":true,"value":"\ufffd\ufffd\ufffd"},
    "MessageEvent/init-false": {"ok":false,"error":"TypeError"},
    "MessageEvent/init-zero": {"ok":false,"error":"TypeError"},
    "MessageEvent/init-string": {"ok":false,"error":"TypeError"},
    "MessageEvent/init-symbol": {"ok":false,"error":"TypeError"},
    "MessageEvent/init-function": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object MessageEvent]"}},
    "CloseEvent/missing": {"ok":false,"error":"TypeError"},
    "CloseEvent/defaults": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object CloseEvent]","code":0,"reason":"","wasClean":false}},
    "CloseEvent/null": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object CloseEvent]","code":0,"reason":"","wasClean":false}},
    "CloseEvent/symbol-type": {"ok":false,"error":"TypeError"},
    "CloseEvent/surrogate-type": {"ok":true,"value":"\ufffd\ufffd\ufffd"},
    "CloseEvent/init-false": {"ok":false,"error":"TypeError"},
    "CloseEvent/init-zero": {"ok":false,"error":"TypeError"},
    "CloseEvent/init-string": {"ok":false,"error":"TypeError"},
    "CloseEvent/init-symbol": {"ok":false,"error":"TypeError"},
    "CloseEvent/init-function": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object CloseEvent]"}},
    "CustomEvent/missing": {"ok":false,"error":"TypeError"},
    "CustomEvent/defaults": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object CustomEvent]","detail":{"undefined":true}}},
    "CustomEvent/null": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object CustomEvent]","detail":{"undefined":true}}},
    "CustomEvent/symbol-type": {"ok":false,"error":"TypeError"},
    "CustomEvent/surrogate-type": {"ok":true,"value":"\ufffd\ufffd\ufffd"},
    "CustomEvent/init-false": {"ok":false,"error":"TypeError"},
    "CustomEvent/init-zero": {"ok":false,"error":"TypeError"},
    "CustomEvent/init-string": {"ok":false,"error":"TypeError"},
    "CustomEvent/init-symbol": {"ok":false,"error":"TypeError"},
    "CustomEvent/init-function": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object CustomEvent]"}},
    "Event/snapshot": {"ok":true,"value":{"reads":["bubbles","cancelable","composed"],"value":{"type":"x","bubbles":true,"cancelable":true,"composed":true,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object Event]"}}},
    "Event/lifecycle": {"ok":true,"value":{"states":[{"phase":0,"target":false,"current":false,"path":[],"canceled":false},{"phase":2,"target":true,"current":true,"path":[true],"canceled":false},{"phase":0,"target":true,"current":true,"path":[],"canceled":true},{"phase":2,"target":true,"current":true,"path":[true],"canceled":false},{"phase":0,"target":true,"current":true,"path":[],"canceled":true}],"returns":[false,false]}},
    "Event/no-listeners": {"ok":true,"value":{"result":true,"phase":0,"target":true,"current":true,"path":0}},
    "Event/reentrant": {"ok":true,"value":"InvalidStateError"},
    "Event/trust-descriptor": {"ok":true,"value":{"own":false,"descriptor":{"get":"function","set":"undefined","configurable":true,"enumerable":true},"assignment":"TypeError","value":false}},
    "ErrorEvent/reads": {"ok":true,"value":{"reads":["bubbles","cancelable","composed","message","filename","lineno","colno","error"],"value":{"type":"x","bubbles":true,"cancelable":true,"composed":true,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object ErrorEvent]","message":"m","filename":"f","lineno":2,"colno":3,"error":4}}},
    "MessageEvent/reads": {"ok":true,"value":{"reads":["bubbles","cancelable","composed","data","origin","lastEventId","source","ports"],"value":{"type":"x","bubbles":true,"cancelable":true,"composed":true,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object MessageEvent]","data":5,"origin":"o","lastEventId":"i","source":null,"ports":[]}}},
    "CloseEvent/reads": {"ok":true,"value":{"reads":["bubbles","cancelable","composed","code","reason","wasClean"],"value":{"type":"x","bubbles":true,"cancelable":true,"composed":true,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object CloseEvent]","code":1000,"reason":"r","wasClean":true}}},
    "CustomEvent/reads": {"ok":true,"value":{"reads":["bubbles","cancelable","composed","detail"],"value":{"type":"x","bubbles":true,"cancelable":true,"composed":true,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object CustomEvent]","detail":6}}},
    "ErrorEvent/number-negative": {"ok":true,"value":[-1,-1]},
    "ErrorEvent/number-fraction": {"ok":true,"value":[1,1]},
    "ErrorEvent/number-smallNegative": {"ok":true,"value":[0,0]},
    "ErrorEvent/number-min": {"ok":true,"value":[-2147483648,-2147483648]},
    "ErrorEvent/number-under": {"ok":false,"error":"TypeError"},
    "ErrorEvent/number-max": {"ok":true,"value":[2147483647,2147483647]},
    "ErrorEvent/number-over": {"ok":false,"error":"TypeError"},
    "ErrorEvent/number-maxFraction": {"ok":false,"error":"TypeError"},
    "ErrorEvent/number-nan": {"ok":true,"value":[0,0]},
    "ErrorEvent/number-infinity": {"ok":true,"value":[0,0]},
    "ErrorEvent/number-negativeInfinity": {"ok":true,"value":[0,0]},
    "ErrorEvent/number-bigint": {"ok":false,"error":"TypeError"},
    "ErrorEvent/number-boxedBigint": {"ok":false,"error":"TypeError"},
    "ErrorEvent/number-null": {"ok":true,"value":[0,0]},
    "ErrorEvent/number-undefined": {"ok":true,"value":[0,0]},
    "ErrorEvent/strings": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object ErrorEvent]","message":"123","filename":"null","lineno":0,"colno":0,"error":{"undefined":true}}},
    "ErrorEvent/surrogates": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object ErrorEvent]","message":"\ufffd\ufffd\ufffd","filename":"\ufffd\ufffd\ufffd","lineno":0,"colno":0,"error":{"undefined":true}}},
    "ErrorEvent/identity": {"ok":true,"value":true},
    "MessageEvent/identity": {"ok":true,"value":true},
    "MessageEvent/strings": {"ok":true,"value":{"type":"x","bubbles":false,"cancelable":false,"composed":false,"phase":0,"trusted":false,"timeStamp":0,"tag":"[object MessageEvent]","data":null,"origin":"123","lastEventId":"null","source":null,"ports":[]}},
    "MessageEvent/source-object": {"ok":false,"error":"TypeError"},
    "MessageEvent/source-number": {"ok":false,"error":"TypeError"},
    "MessageEvent/source-string": {"ok":false,"error":"TypeError"},
    "MessageEvent/source-null": {"ok":true,"value":{"same":true,"null":true}},
    "MessageEvent/source-undefined": {"ok":true,"value":{"same":false,"null":true}},
    "MessageEvent/source-port": {"ok":true,"value":{"same":true,"null":false}},
    "MessageEvent/source-forged": {"ok":false,"error":"TypeError"},
    "MessageEvent/ports-object": {"ok":false,"error":"TypeError"},
    "MessageEvent/ports-number": {"ok":false,"error":"TypeError"},
    "MessageEvent/ports-string": {"ok":false,"error":"TypeError"},
    "MessageEvent/ports-null": {"ok":false,"error":"TypeError"},
    "MessageEvent/ports-undefined": {"ok":true,"value":{"length":0,"frozen":false,"same":false,"stable":false,"first":false}},
    "MessageEvent/ports-empty": {"ok":true,"value":{"length":0,"frozen":false,"same":false,"stable":false,"first":false}},
    "MessageEvent/ports-invalid": {"ok":false,"error":"TypeError"},
    "MessageEvent/ports-list": {"ok":true,"value":{"length":1,"frozen":false,"same":false,"stable":false,"first":true}},
    "MessageEvent/ports-set": {"ok":false,"error":"TypeError"},
    "MessageEvent/ports-arrayLike": {"ok":false,"error":"TypeError"},
    "CloseEvent/number-negative": {"ok":true,"value":-1},
    "CloseEvent/number-max": {"ok":true,"value":65535},
    "CloseEvent/number-high": {"ok":true,"value":65536},
    "CloseEvent/number-maxFraction": {"ok":true,"value":65535},
    "CloseEvent/number-int32": {"ok":true,"value":2147483647},
    "CloseEvent/number-over32": {"ok":false,"error":"TypeError"},
    "CustomEvent/identity": {"ok":true,"value":true},
    "MessagePort/constructor": {"ok":false,"error":"TypeError"}
};

const here = path.dirname(url.fileURLToPath(import.meta.url));
const fixtureDir = path.join(here, 'fixtures', 'workerd-ws');
let routePrefix = '';
let backend = 'workerd';

// Each control request owns its HTTP connection; reference shutdown must
// not wait for an unrelated client pool's idle keep-alive sockets.
function fixtureFetch(target) {
    return fetch(target, { headers: { Connection: 'close' } });
}

function parseArgs(argv) {
    const values = new Map();
    for (let index = 0; index < argv.length; index += 1) {
        const token = argv[index];
        if (!token?.startsWith('--')) {
            continue;
        }
        if (token.includes('=')) {
            const eq = token.indexOf('=');
            values.set(token.slice(2, eq), token.slice(eq + 1));
        } else {
            const next = argv[index + 1];
            if (next && !next.startsWith('--')) {
                values.set(token.slice(2), next);
                index++;
            } else values.set(token.slice(2), true);
        }
    }
    return values;
}

const fixtures = fixtureIdentities(fixtureDir).map(x => x.fixture);

// ---------- reference branch (workerd) ----------

function waitPort(port, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    return new Promise((resolve, reject) => {
        const attempt = () => {
            const socket = net.connect(port, '127.0.0.1');
            socket.once('connect', () => { socket.destroy(); resolve(); });
            socket.once('error', () => {
                socket.destroy();
                if (Date.now() > deadline) {
                    reject(new Error(`workerd did not listen on ${port}`));
                } else {
                    setTimeout(attempt, 50);
                }
            });
        };
        attempt();
    });
}

function startWorkerd(binary, fixture, port, dir, lock) {
    const module = path.join(dir, 'main.mjs');
    fs.copyFileSync(path.join(fixtureDir, fixture), module);
    const capnp = path.join(dir, 'config.capnp');
    fs.writeFileSync(capnp, [
        'using Workerd = import "/workerd/workerd.capnp";',
        '',
        'const config :Workerd.Config = (',
        // "internet" network service: fixtures that dial outbound
        // (api-compat C16) need loopback egress; restrictPeers blocks
        // it otherwise and the failure is fatal, not an event.
        '  services = [ (name = "main", worker = .mainWorker),',
        '    (name = "internet", network = (allow = ["private"])) ],',
        '  sockets = [',
        `    (name = "http", address = "127.0.0.1:${port}", http = (),`,
        '     service = "main"),',
        '  ],',
        ');',
        '',
        'const mainWorker :Workerd.Worker = (',
        '  globalOutbound = "internet",',
        `  compatibilityDate = ${JSON.stringify(lock.compatibility_date)},`,
        `  compatibilityFlags = [${lock.compatibility_flags.map((flag) => JSON.stringify(flag)).join(', ')}],`,
        '  modules = [ (name = "main", esModule = embed "main.mjs") ],',
        ');',
        '',
    ].join('\n'));
    const child = spawn(binary, ['serve', capnp], { stdio: 'pipe' });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    return { child, stderr: () => stderr };
}

function failedUpgrade(port, target, waitForClose = false) {
    return new Promise((resolve, reject) => {
        const socket = net.connect(port, '127.0.0.1');
        let buffer = '', status, done = false;
        const finish = error => {
            if (done) return;
            done = true; clearTimeout(timer); socket.destroy();
            if (error) reject(error); else resolve(status);
        };
        const timer = setTimeout(() => finish(new Error('failed upgrade terminal deadline')), 4000);
        socket.on('connect', () => socket.write(
            `GET ${routePrefix}${target} HTTP/1.1\r\nHost: public.example\r\n` +
            'Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n' +
            `Sec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\n\r\n`));
        socket.on('data', bytes => {
            buffer += bytes.toString();
            if (buffer.includes('\r\n\r\n')) {
                status = Number(/^HTTP\/1.1 (\d+)/.exec(buffer)?.[1]);
                if (status !== 101 || !waitForClose) finish();
            }
        });
        socket.on('error', error => status ? finish() : finish(error));
        socket.on('close', () => status ? finish() : finish(new Error('no HTTP terminal')));
    });
}

function wsSession(port, target, script,
                    { binaryType, protocols,
                      tolerateErrorAfterOpen } = {}) {
    // One WebSocket session against ws://127.0.0.1:port/target; the
    // script drives the client and resolves with its result.
    // binaryType is applied before any frame can arrive.
    // tolerateErrorAfterOpen: abnormal terminations (worker crash,
    // transport reset) surface as a client-side transport error; the
    // script still observes the close through its own onclose.
    return new Promise((resolve, reject) => {
        const timer = setTimeout(
            () => { ws.close(); reject(new Error('ws session timeout')); }, 10000);
        const ws = new WebSocket(`ws://127.0.0.1:${port}${routePrefix}${target}`, protocols);
        const closed = new Promise(done => ws.addEventListener('close', done, { once: true }));
        if (binaryType) {
            ws.binaryType = binaryType;
        }
        let opened = false;
        ws.onopen = () => {
            opened = true;
            Promise.resolve().then(() => script(ws)).then(async result => {
                if (ws.readyState === WebSocket.OPEN) ws.close(1000);
                await closed;
                return result;
            }).then(
                (result) => { clearTimeout(timer); resolve(result); },
                (error) => { clearTimeout(timer); ws.close(); reject(error); });
        };
        ws.onerror = (event) => {
            if (opened && tolerateErrorAfterOpen) {
                return;
            }
            clearTimeout(timer);
            reject(new Error(
                'ws error before open: ' +
                (event?.error?.code ?? event?.error?.message ??
                    event?.message ?? 'no detail')));
        };
    });
}

function rawWsSession(port, target, driver, { timeoutMs = 10000, coalesce } = {}) {
    // Minimal raw client: performs the HTTP upgrade by hand, then lets
    // the driver write arbitrary frames and observe raw reads. Used
    // only for protocol-violation scenarios the native client refuses
    // to originate.
    return new Promise((resolve, reject) => {
        const timer = setTimeout(
            () => { socket.destroy(); reject(new Error('raw ws session timeout')); }, timeoutMs);
        const socket = net.connect(port, '127.0.0.1');
        const chunks = [];
        let header = null;
        socket.on('data', (chunk) => {
            try {
            if (header === null) {
                chunks.push(chunk);
                const joined = Buffer.concat(chunks);
                const split = joined.indexOf('\r\n\r\n');
                if (split === -1) {
                    return;
                }
                header = joined.subarray(0, split).toString('latin1');
                const status = Number(header.split(' ')[1]);
                if (status !== 101) {
                    clearTimeout(timer);
                    socket.destroy();
                    reject(new Error(`upgrade rejected: ${status}`));
                    return;
                }
                const rest = joined.subarray(split + 4);
                if (rest.length > 0) {
                    driver.onFrame?.(rest, socket);
                }
                driver.onOpen?.(socket);
                return;
            }
            driver.onFrame?.(chunk, socket);
            } catch (error) { clearTimeout(timer); socket.destroy(); reject(error); }
        });
        socket.on('error', (error) => {
            clearTimeout(timer);
            reject(error);
        });
        socket.on('close', () => {
            clearTimeout(timer);
            try { resolve(driver.result?.()); } catch (error) { reject(error); }
        });
        socket.on('connect', () => {
            const key = Buffer.from(
                crypto.getRandomValues(new Uint8Array(16))).toString('base64');
            const handshake =
                `GET ${routePrefix}${target} HTTP/1.1\r\n` +
                `Host: 127.0.0.1:${port}\r\n` +
                `Upgrade: websocket\r\n` +
                `Connection: Upgrade\r\n` +
                `Sec-WebSocket-Key: ${key}\r\n` +
                `Sec-WebSocket-Version: 13\r\n\r\n`;
            socket.write(coalesce ? Buffer.concat([Buffer.from(handshake),coalesce]) : handshake);
        });
    });
}

// minimal helpers over raw sockets ------------------------------

function rawTextFrame(payload, { mask = true } = {}) {
    const body = Buffer.from(payload, 'latin1');
    return rawFrame(0x1, body, { mask });
}

function rawFrame(opcode, body, { mask = true, fin = true } = {}) {
    const maskBit = mask ? 0x80 : 0;
    const length = body.length;
    let header;
    if (length < 126) {
        header = Buffer.from([(fin ? 0x80 : 0) | opcode, maskBit | length]);
    } else if (length < 65536) {
        header = Buffer.alloc(4);
        header[0] = (fin ? 0x80 : 0) | opcode;
        header[1] = maskBit | 126;
        header.writeUInt16BE(length, 2);
    } else {
        header = Buffer.alloc(10);
        header[0] = (fin ? 0x80 : 0) | opcode;
        header[1] = maskBit | 127;
        header.writeBigUInt64BE(BigInt(length), 2);
    }
    let masked = body;
    if (mask) {
        const key = crypto.getRandomValues(new Uint8Array(4));
        masked = Buffer.from(
            body.map((byte, index) => byte ^ key[index % 4]));
        return Buffer.concat([header, Buffer.from(key), masked]);
    }
    return Buffer.concat([header, masked]);
}

function rawServerFrames(buffer) {
    // decode every complete server->client (unmasked) frame in buffer;
    // returns {frames, rest}
    const frames = [];
    let offset = 0;
    while (offset + 2 <= buffer.length) {
        const opcode = buffer[offset] & 0x0f;
        const masked = (buffer[offset + 1] & 0x80) !== 0;
        let length = buffer[offset + 1] & 0x7f;
        let headerLength = 2;
        if (length === 126) {
            if (offset + 4 > buffer.length) {
                break;
            }
            length = buffer.readUInt16BE(offset + 2);
            headerLength = 4;
        } else if (length === 127) {
            if (offset + 10 > buffer.length) {
                break;
            }
            length = Number(buffer.readBigUInt64BE(offset + 2));
            headerLength = 10;
        }
        if (masked) {
            // server->client frames must be unmasked; treat as corrupt
            throw new Error('masked server frame');
        }
        if (offset + headerLength + length > buffer.length) {
            break;
        }
        frames.push({
            fin: (buffer[offset] & 0x80) !== 0,
            opcode,
            payload: buffer.subarray(
                offset + headerLength, offset + headerLength + length),
        });
        offset += headerLength + length;
    }
    return { frames, rest: buffer.subarray(offset) };
}

function rawServerMessages(buffer, state) {
    const parsed = rawServerFrames(buffer);
    const messages = [];
    state.assembly ??= { opcode: null, chunks: [], size: 0 };
    const assembly = state.assembly;
    for (const frame of parsed.frames) {
        if (frame.opcode >= 8) { messages.push(frame); continue; }
        if (frame.opcode !== 0) {
            assert.equal(assembly.opcode, null, 'no overlapping fragmented messages');
            assembly.opcode = frame.opcode;
        } else assert.notEqual(assembly.opcode, null, 'continuation has a message');
        assembly.size += frame.payload.length;
        assert.ok(assembly.size <= 32 * 1024 * 1024, 'raw client message bound');
        assembly.chunks.push(frame.payload);
        if (frame.fin) {
            messages.push({opcode:assembly.opcode,payload:Buffer.concat(assembly.chunks),fin:true});
            assembly.opcode=null; assembly.chunks=[]; assembly.size=0;
        }
    }
    return {frames:messages,rest:parsed.rest};
}

function parseCloseFrame(buffer) {
    // returns {code, reason} from the first complete close frame in a
    // raw buffer, or null
    if (buffer.length < 2) {
        return null;
    }
    const opcode = buffer[0] & 0x0f;
    if (opcode !== 0x8) {
        return null;
    }
    const length = buffer[1] & 0x7f;
    const payload = buffer.subarray(2, 2 + length);
    const code = payload.length >= 2 ? payload.readUInt16BE(0) : null;
    const reason = payload.length > 2
        ? payload.subarray(2).toString('utf8') : '';
    return { code, reason };
}

const scenarios = {
    'response.mjs': async port => {
        const sessions=[];
        const base=`http://127.0.0.1:${port}${routePrefix}`;
        const request=async target=>{
            const response=await fixtureFetch(base+target);
            const text=await response.text();
            if(response.status!==200)throw new Error(`Response fixture HTTP ${response.status}: ${text}`);
            return text;
        };
        const start=(kind,id,extra={})=>{
            const session=responseSession(port,routePrefix+'/upgrade?'+new URLSearchParams({kind,id,...extra}));
            sessions.push(session);return session;
        };
        const until=async(read,condition,label)=>{
            const deadline=performance.now()+4000;
            for(;;) {
                const value=await read();if(condition(value))return value;
                if(performance.now()>=deadline)throw new Error(label+': '+JSON.stringify(value));
                await new Promise(resolve=>setTimeout(resolve,5));
            }
        };
        const control=async phase=>{
            const pair=[start('hold',phase+'-0'),start('hold',phase+'-1')];
            const heads=await Promise.all(pair.map(x=>x.headers));
            for(const head of heads)assert.equal(head.status,101,'both live service slots admit');
            const simultaneous=pair.every(x=>!x.snapshot().peerTerminated);
            pair.forEach((x,i)=>x.send(phase+':'+i));
            assert.deepEqual(await Promise.all(pair.map(x=>x.text())),['reply:'+phase+':0','reply:'+phase+':1']);
            pair.forEach(x=>x.close());
            return {phase,simultaneous,connections:await Promise.all(pair.map(x=>x.complete))};
        };
        const sockets=new Set();let upgradeUpstream=true,delayedPending=false;
        const upstream=net.createServer(socket=>{
            sockets.add(socket);socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>{});
            let input='';socket.on('data',chunk=>{
                input+=chunk.toString();if(!input.includes('\r\n\r\n'))return;
                socket.removeAllListeners('data');
                const key=/sec-websocket-key: ([^\r\n]+)/i.exec(input)?.[1];
                if(!key){socket.destroy();return}
                if(upgradeUpstream)socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Protocol: chat\r\nSec-WebSocket-Accept: '+crypto.createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64')+'\r\n\r\n');
            });
        });
        try {
            const constructors=JSON.parse(await request('/construct'));
            assertResponseConstructors(constructors,assert);
            const observation={constructors,controls:[await control('before')],aborted:[],outbound:[]};
            for(let iteration=0;iteration<6;++iteration)for(const kind of ['accepted','both','closed']) {
                const session=start(kind,kind+'-'+iteration);
                const wire=await session.complete;
                assert.equal(wire.status,101,'failed coupling commits 101 before transport abort');
                observation.aborted.push({kind,iteration,wire});
            }
            const delayed=start('delayed','delayed');
            delayedPending='delayed';
            const head=await delayed.headers;
            assert.equal(head.status,101,'101 is visible while the peer remains unaccepted');
            delayed.send('early');
            const before=JSON.parse(await request('/state?id=delayed'));
            const beforeWire=delayed.snapshot();
            const releaseText=await request('/release?id=delayed');
            delayedPending=false;
            const wire=await delayed.complete;
            const after=await until(async()=>JSON.parse(await request('/state?id=delayed')),
                x=>x.events.some(e=>e.type==='close'),'delayed close callback');
            observation.delayed={before,beforeWire,release:{status:200,text:releaseText},wire,after};
            const earlyClose=start('delayed-close','delayed-close');
            delayedPending='delayed-close';
            assert.equal((await earlyClose.headers).status,101,'101 precedes DATA and Close before accept');
            earlyClose.send('early');earlyClose.close();
            const beforeClose=JSON.parse(await request('/state?id=delayed-close'));
            const beforeCloseWire=earlyClose.snapshot();
            const closeRelease=await request('/release?id=delayed-close');delayedPending=false;
            const closeWire=await earlyClose.complete;
            const afterClose=await until(async()=>JSON.parse(await request('/state?id=delayed-close')),
                x=>x.events.some(e=>e.type==='close'),'pre-accept Close callback');
            observation.delayedClose={before:beforeClose,beforeWire:beforeCloseWire,
                release:{status:200,text:closeRelease},wire:closeWire,after:afterClose};
            assert.equal(await request('/prepare'),'prepared');
            observation.crossContext=await new Promise((resolve,reject)=>{
                const req=http.get({host:'127.0.0.1',port,path:routePrefix+'/upgrade?kind=prepared',
                    headers:{Connection:'Upgrade',Upgrade:'websocket','Sec-WebSocket-Version':'13',
                        'Sec-WebSocket-Key':crypto.randomBytes(16).toString('base64'),'Sec-WebSocket-Protocol':'chat'}},response=>{
                    const parts=[];response.on('data',x=>parts.push(x));response.on('error',reject);
                    response.on('end',()=>resolve({status:response.statusCode,bodyComplete:response.complete,bodyNonempty:Buffer.concat(parts).length>0}));
                });
                req.on('upgrade',(_response,socket)=>{socket.destroy();reject(new Error('cross-request endpoint unexpectedly upgraded'))});
                req.setTimeout(4000,()=>req.destroy(new Error('cross-request Response deadline')));req.on('error',reject);
            });
            await new Promise((resolve,reject)=>{upstream.once('error',reject);upstream.listen(0,'127.0.0.1',resolve)});
            const target=`ws://127.0.0.1:${upstream.address().port}/response`;
            for(const kind of ['outbound-open','outbound-connecting']) {
                upgradeUpstream=kind==='outbound-open';
                const session=start(kind,kind,{target});const wire=await session.complete;
                // The fixture owns the independent upstream. Close that real
                // transport explicitly; object GC is not a cleanup barrier.
                for(const socket of sockets)socket.destroy();
                await until(()=>sockets.size,x=>x===0,'fixture upstream closes');
                await request('/healthy');
                observation.outbound.push({kind,wire,upstreamStopped:sockets.size===0});
            }
            observation.controls.push(await control('after'));
            observation.healthy={status:200,text:await request('/healthy')};
            assertResponseObservation(observation,assert);
            return observation;
        } finally {
            if(delayedPending) {try{await request('/release?id='+encodeURIComponent(delayedPending))}catch{}}
            for(const session of sessions)session.dispose();
            for(const socket of sockets)socket.destroy();
            if(upstream.listening)await new Promise(resolve=>upstream.close(resolve));
        }
    },
    'transport.mjs': async port => {
        const large = await wsSession(port,'/large',async socket => {
            const bytes=Uint8Array.from({length:180000},(_,i)=>(i*17)&255);
            const echoed=await new Promise(resolve=>{socket.onmessage=event=>resolve(event.data);socket.send(bytes);});
            assert.deepEqual(new Uint8Array(echoed),bytes,'both network directions cross initial credit');
            const clean=await new Promise(resolve=>{socket.onclose=resolve;socket.close(1000,'large\0done');});
            assert.equal(clean.wasClean,true);
            assert.equal(clean.reason,'large\0done','Close reason preserves embedded NUL across IPC');
            return { bytes: echoed.byteLength, sha256: crypto.createHash('sha256').update(new Uint8Array(echoed)).digest('hex'),
                close: { code: clean.code, reason: clean.reason, wasClean: clean.wasClean } };
        },{binaryType:'arraybuffer'});
        const observed=[];
        await rawWsSession(port,'/leftovers',{
            buffer:Buffer.alloc(0),onOpen(){},
            onFrame(chunk,socket){this.buffer=Buffer.concat([this.buffer,chunk]);const parsed=rawServerMessages(this.buffer,this);this.buffer=parsed.rest;
                for(const frame of parsed.frames) {
                    if(frame.opcode===1){observed.push(frame.payload.toString());socket.write(rawFrame(8,Buffer.from([3,0xe8])));}
                    if(frame.opcode===8)socket.end();
                }
            },result(){return observed;},
        },{coalesce:rawTextFrame('coalesced')});
        assert.deepEqual(observed,['coalesced'],'Beast leftovers become WS input after serial 101');
        const text=[];let pong=false;
        await rawWsSession(port,'/utf8',{
            buffer:Buffer.alloc(0),onOpen(socket){socket.write(Buffer.concat([
                rawFrame(1,Buffer.from([0xe7]),{fin:false}),rawFrame(9,Buffer.from('between')),
                rawFrame(0,Buffer.concat([Buffer.from([0x95,0x8c]),Buffer.from('text')]))]));},
            onFrame(chunk,socket){this.buffer=Buffer.concat([this.buffer,chunk]);const parsed=rawServerMessages(this.buffer,this);this.buffer=parsed.rest;
                for(const frame of parsed.frames){
                    if(frame.opcode===10){assert.equal(frame.payload.toString(),'between');pong=true;}
                    if(frame.opcode===1){text.push(frame.payload.toString());socket.write(rawFrame(8,Buffer.from([3,0xe8])));}
                    if(frame.opcode===8)socket.end();
                }
            },result(){return text;},
        });
        assert.deepEqual(text,['界text'],'UTF-8 survives RFC continuation and interleaved Ping');assert.equal(pong,true);
        const protocol = await wsSession(port,'/protocol',async socket=>{
            assert.equal(socket.protocol,'chat','selected subprotocol was offered');
            await new Promise(resolve=>{socket.onclose=resolve;socket.close(1000);});
            return socket.protocol;
        },{protocols:['chat','other']});
        return { large, leftovers: observed, text, pong, protocol };
    },
    'lifecycle.mjs': async port => {
        let buffer = Buffer.alloc(0);
        const observed = [];
        await rawWsSession(port, '/half', {
            onOpen(socket) {
                const payload = Buffer.concat([Buffer.from([0x0b, 0xb9]), Buffer.from('peer done')]);
                socket.write(rawFrame(8, payload));
            },
            onFrame(chunk, socket) {
                buffer = Buffer.concat([buffer, chunk]);
                const parsed = rawServerMessages(buffer, this); buffer = parsed.rest;
                for (const frame of parsed.frames) {
                    observed.push({opcode:frame.opcode, payload:frame.payload});
                    if (frame.opcode === 8) socket.end();
                }
            }, result() { return observed; },
        });
        assert.equal(observed.length, 2);
        assert.equal(observed[0].opcode, 1);
        assert.deepEqual(JSON.parse(observed[0].payload.toString()), {code:3001,reason:'peer done',clean:true,state:2});
        assert.equal(observed[1].opcode, 8);
        assert.equal(observed[1].payload.readUInt16BE(), 1000);
        buffer = Buffer.alloc(0);
        let receivedClose = false;
        await rawWsSession(port, '/sender-close', {
            onOpen() {},
            onFrame(chunk, socket) {
                buffer = Buffer.concat([buffer, chunk]);
                const parsed = rawServerMessages(buffer, this); buffer = parsed.rest;
                for (const frame of parsed.frames) if (frame.opcode === 8) {
                    receivedClose = true;
                    socket.write(rawTextFrame('after sender Close'));
                    setTimeout(() => { socket.write(rawFrame(8, Buffer.from([0x03,0xe8]))); socket.end(); }, 100);
                }
            }, result() { return receivedClose; },
        });
        assert.equal(receivedClose, true);
        const stats = await (await fixtureFetch(`http://127.0.0.1:${port}${routePrefix}/stats`)).json();
        assert.equal(stats.lateMessages, 1);
        return { half: observed.map(frame => ({ opcode: frame.opcode, payload: frame.payload.toString('hex') })),
            receivedClose, lateMessages: stats.lateMessages };
    },
    'semantics.mjs': async (port) => {
        const values = await (await fixtureFetch(`http://127.0.0.1:${port}${routePrefix}/selftest`)).json();
        assert.equal(values.defaultBinaryType, 'blob');
        assert.deepEqual(values.keys, ['0', '1']);
        assert.equal(values.iterableLength, 2);
        assert.equal(values.beforeAcceptError, 'TypeError');
        assert.equal(values.repeatedAcceptError, null);
        assert.equal(values.readyState, 1);
        assert.equal(values.invalidBinaryType, 'blob');
        const exchange = await wsSession(port, '/semantics', async (ws) => {
            const replies = await new Promise((resolve) => {
                const messages = [];
                ws.onmessage = (event) => { messages.push(event.data); if (messages.length === 3) resolve(messages); };
                ws.send(new Uint8Array([7, 8]));
            });
            assert.deepEqual(JSON.parse(replies[0]), { binaryType: 'blob', dataType: 'Blob' });
            assert.deepEqual([...new Uint8Array(replies[1])], [2, 3]);
            assert.equal(replies[2], '[object Blob]');
            ws.close(1000);
            return [JSON.parse(replies[0]), [...new Uint8Array(replies[1])], replies[2]];
        }, { binaryType: 'arraybuffer' });
        return { values, exchange };
    },
    'api-compat.mjs': async (port, {server}) => {
        const observation = {dial:[]};
        observation.publicEvents = await (await fixtureFetch(
            `http://127.0.0.1:${port}${routePrefix}/public-events`)).json();
        assert.equal(Object.keys(publicEventContract).length, 105, 'reviewed public event case inventory');
        assert.deepEqual(Object.keys(observation.publicEvents).sort(), Object.keys(publicEventContract).sort(),
            'complete public event case inventory');
        for (const [name, expected] of Object.entries(publicEventContract)) {
            assert.deepEqual(observation.publicEvents[name], expected, `public event contract: ${name}`);
        }

        observation.listenerEvents = await (await fixtureFetch(
            `http://127.0.0.1:${port}${routePrefix}/listener-events`)).json();
        assert.equal(Object.keys(listenerEventContract).length, 66, 'reviewed listener case inventory');
        assert.deepEqual(Object.keys(observation.listenerEvents).sort(), Object.keys(listenerEventContract).sort(),
            'complete listener case inventory');
        for (const [name, expected] of Object.entries(listenerEventContract)) {
            assert.deepEqual(observation.listenerEvents[name], expected, `listener contract: ${name}`);
        }

        observation.handlerAttributes = await (await fixtureFetch(
            `http://127.0.0.1:${port}${routePrefix}/handler-attributes`)).json();
        assert.equal(eventHandlerContract.length, 259, 'reviewed event handler case inventory');
        assert.deepEqual(observation.handlerAttributes.map(x => x.name), eventHandlerContract.map(x => x.name),
            'complete event handler case inventory');
        for (let i = 0; i < eventHandlerContract.length; i++) {
            assert.deepEqual(observation.handlerAttributes[i], eventHandlerContract[i],
                `event handler contract: ${eventHandlerContract[i].name}`);
        }

        const runtimeEventContract = {
            "error": {"type":"error","trusted":true,"phase":2,"target":true,"currentTarget":true,"tag":"[object ErrorEvent]","identity":true},
            "errorAfter": {"type":"error","trusted":true,"phase":0,"target":true,"currentTarget":true,"tag":"[object ErrorEvent]"},
            "message": {"type":"message","trusted":true,"phase":2,"target":true,"currentTarget":true,"tag":"[object MessageEvent]","data":"runtime","sourceIsSender":false,"sourceIsReceiver":true,"sourceIsPort":true,"ports":0},
            "messageAfter": {"type":"message","trusted":true,"phase":0,"target":true,"currentTarget":true,"tag":"[object MessageEvent]"},
            "messageOrder": ["first","second","microtask"],
            "unhandled": {"type":"unhandledrejection","trusted":true,"phase":2,"target":true,"currentTarget":true,"tag":"[object PromiseRejectionEvent]","identity":true},
            "unhandledAfter": {"type":"unhandledrejection","trusted":true,"phase":0,"target":true,"currentTarget":true,"tag":"[object PromiseRejectionEvent]"},
            "handled": {"type":"rejectionhandled","trusted":true,"phase":2,"target":true,"currentTarget":true,"tag":"[object PromiseRejectionEvent]","promiseIdentity":true,"reasonIdentity":false,"reasonType":"undefined"},
            "handledAfter": {"type":"rejectionhandled","trusted":true,"phase":0,"target":true,"currentTarget":true,"tag":"[object PromiseRejectionEvent]"},
        };
        observation.runtimeEvents = await (await fixtureFetch(
            `http://127.0.0.1:${port}${routePrefix}/runtime-events`)).json();
        assert.deepEqual(observation.runtimeEvents, runtimeEventContract,
            'runtime event delivery matches the pinned default workerd profile');

        observation.messagePortQueue = await (await fixtureFetch(
            `http://127.0.0.1:${port}${routePrefix}/message-port-queue`)).json();
        const queueContract = {
            beforeStart: ['first', 'second'],
            immediatelyAfterStart: ['first', 'second'],
            received: ['first', 'second', 'third'],
        };
        assert.deepEqual(observation.messagePortQueue, {
            explicit: queueContract, handler: queueContract, nullHandler: queueContract,
        }, 'MessagePort delivery matches the pinned default workerd profile');

        // C01-C17 golden table from the locked workerd reference
        // (docs/v0.3.0-test-completeness-review.md §3).
        const state = async () => {
            const response = await fixtureFetch(
                `http://127.0.0.1:${port}${routePrefix}/state`);
            assert.equal(response.status, 200);
            return (await response.json());
        };
        const byId = entries => {
            const map = new Map();
            for (const entry of entries) {
                map.set(entry.id, entry.outcome);
            }
            return map;
        };

        observation.nativeHandlerMessages = await wsSession(port, '/ws?case=handler-attributes', async ws => {
            const messages = [];
            for (const data of ['first', 'second', 'third']) {
                const message = new Promise((resolve, reject) => {
                    ws.onmessage = event => { try { resolve(JSON.parse(event.data)); } catch (error) { reject(error); } };
                    ws.onerror = () => reject(new Error('native handler transport failed'));
                });
                ws.send(data); messages.push(await message);
            }
            ws.close(1000, 'handler attributes');
            return messages;
        });
        assert.deepEqual(observation.nativeHandlerMessages, [
            {data:'first',calls:['attribute','listener']},
            {data:'second',calls:['listener']},
            {data:'third',calls:['restored','listener']},
        ], 'native messages preserve independent handler and listener registrations');
        observation.nativeHandlerClose = (await state()).filter(x => x.id === 'handler-close').map(x => x.outcome);
        assert.deepEqual(observation.nativeHandlerClose, ['attribute','listener'],
            'native close invokes both registrations in reference order');

        // The pinned send() preserves surrogate encodings (WTF-8), whereas
        // close(reason) replaces them. Read actual bytes: a validating SDK
        // disconnects at the first malformed UTF-8 text frame and hides the
        // remaining conversion outcomes. These literals are independent of
        // TextEncoder/toWellFormed and of the implementation under test.
        const expectedTextHex = ['', '415343494900656e64', 'c3a9e4b8ad', 'f09f9a80',
            'eda080', 'edb080', 'eda080eda081', 'edb080eda080',
            'eda08041edbfbf42f48fbfbf', '313233', '6e756c6c', '756e646566696e6564',
            'eda0806f626a656374edb080', 'edb0806d6574686f64', 'eda0806361707475726564'];
        observation.stringSend = await rawWsSession(port, '/ws?case=string-send', {
            buffer: Buffer.alloc(0), messages: [], conversion: null, closes: [],
            onOpen(socket) { socket.write(rawTextFrame('go')); },
            onFrame(chunk, socket) {
                this.buffer = Buffer.concat([this.buffer, chunk]);
                const parsed = rawServerFrames(this.buffer);
                this.buffer = parsed.rest;
                for (const frame of parsed.frames) {
                    assert.equal(frame.fin, true, 'string conversion sends final frames');
                    if (frame.opcode === 8) {
                        this.closes.push(frame.payload.toString('hex'));
                        assert.equal(this.messages.length, expectedTextHex.length,
                            'all string conversion messages precede close');
                        assert.ok(this.conversion, 'conversion report precedes close');
                        assert.equal(frame.payload.toString('hex'), '03e8efbfbd41efbfbd42f48fbfbf',
                            'close reason uses USVString with the captured intrinsic');
                        assert.equal(this.closes.length, 1, 'one close frame');
                        socket.end(rawFrame(8, frame.payload));
                        continue;
                    }
                    assert.equal(frame.opcode, 1, 'string conversion remains a text frame');
                    assert.equal(this.closes.length, 0, 'no message follows close');
                    const index = this.messages.length;
                    if (index < expectedTextHex.length) {
                        const hex = frame.payload.toString('hex');
                        assert.equal(hex, expectedTextHex[index], `DOMString wire message ${index}`);
                        this.messages.push(hex);
                    } else {
                        assert.equal(this.conversion, null, 'no extra conversion messages');
                        this.conversion = JSON.parse(frame.payload.toString());
                        assert.deepEqual(this.conversion, {
                            reads: ['primitive:string', 'toString', 'symbol:string', 'throws'],
                            errors: ['TypeError', 'TypeError', 'TypeError'],
                            sameError: true, replacedCalls: 0,
                        }, 'WebSocket send converts once and propagates conversion failures');
                    }
                }
            },
            result() {
                assert.equal(this.buffer.length, 0, 'no truncated trailing frame');
                assert.deepEqual(this.messages, expectedTextHex, 'complete string conversion wire inventory');
                assert.ok(this.conversion, 'actual conversion report received');
                assert.deepEqual(this.closes, ['03e8efbfbd41efbfbd42f48fbfbf'], 'clean USVString close observed');
                return { messages: this.messages, conversion: this.conversion, closes: this.closes };
            },
        });

        // main battery: argument/object-conversion probes
                await wsSession(port, '/ws?case=main', async (ws) => {
            const done = await new Promise((resolve, reject) => {
                ws.onmessage = event => {
                    if (event.data === 'done') resolve();
                };
                ws.onerror = () => reject(new Error('probe socket failed'));
                ws.send('run');
            });
            assert.ok(done === undefined, 'main battery replies');
        });
        const main = byId(await state());
        observation.main = [...main].sort((a,b)=>a[0].localeCompare(b[0]));
        assert.equal(main.get('C01'), 'TypeError', 'send() without arguments');
        assert.equal(main.get('C02'), 'TypeError', 'send(Symbol)');
        assert.equal(main.get('C05'), '1000', 'CloseEvent code truncates');
        assert.equal(main.get('C06'), '0', 'CloseEvent null init defaults');
        assert.equal(main.get('C07'), 'arraybuffer',
            'binaryType converts via ToString');
        assert.equal(main.get('C08'), 'TypeError',
            'addEventListener with a non-callback');
        assert.equal(main.get('C09'), '2',
            'once is captured at registration');
        assert.equal(main.get('C10'), 'TypeError',
            'WebSocket constructor without a URL');
        assert.equal(main.get('C11'), 'TypeError', 'WebSocket(Symbol)');
        assert.equal(main.get('C12'), 'constructed:0',
            'WebSocket(url, null) constructs and dials');
        assert.equal(main.get('C13'), 'RangeError',
            'Response 101 without an endpoint');
        assert.equal(main.get('C14'), 'constructed',
            'Response 101 with an endpoint constructs');

        // round5-7 §3 goldens
        assert.equal(main.get('E-ce-reason-bigint'), '123', 'CE reason BigInt to string');
        assert.equal(main.get('E-ce-reason-null'), 'null', 'CE null reason');
        assert.equal(main.get('E-ce-boxed-bigint'), 'threw:TypeError', 'CE boxed BigInt code');
        assert.equal(main.get('E-ce-code-nan'), '0', 'NaN code -> 0');
        assert.equal(main.get('E-ce-code-inf'), '0', 'Infinity code -> 0');
        assert.equal(main.get('E-ce-lone-surrogate'), '\uFFFD', 'lone surrogate -> U+FFFD');
        assert.deepEqual(JSON.parse(main.get('E-getter-order')),
            { reads: ['code', 'reason', 'wasClean'], values: [1000, 'r', true] },
            'input getters are read once in conversion order');
        assert.equal(main.get('E-ce-string-init'), 'TypeError');
        assert.equal(main.get('E-ce-boolean-init'), 'TypeError');
        assert.equal(main.get('E-ce-null-type'), 'null');
        assert.equal(main.get('E-close-reason-null'), 'accepted');
        assert.equal(main.get('E-close-read-order'), '["code","reason"]');
        assert.equal(main.get('E-ce-primitive'), '1000');
        assert.equal(main.get('E-close-primitive'), 'accepted');
        for (const [id, expected] of Object.entries({
            'P-close-nan': {error:'InvalidAccessError'},
            'P-close-reason-without-code': {error:'InvalidAccessError'},
            'P-local-closing-state': {value:2},
            'P-invalid-listener-object': {error:'TypeError'},
            'P-noncallable-handler': {error:'TypeError'},
            'P-abort-readd-false': {value:1}, 'P-abort-readd-true': {value:1},
            'P-listener-receiver': {value:true},
        })) assert.deepEqual(JSON.parse(main.get(id)), expected, id);
        // fix-review §2/§4/§5 goldens
        assert.equal(main.get('N-close-66536'), 'InvalidAccessError',
            'close(66536) is out of range');
        assert.equal(main.get('N-close-neg'), 'InvalidAccessError',
            'close(-64536) is out of range');
        assert.equal(main.get('N-close-bigint'), 'TypeError',
            'close(BigInt) rejects the conversion');
        assert.equal(main.get('N-ce-65537'), '65537',
            'CloseEvent code truncates without modulo');
        assert.equal(main.get('N-ce-neg1'), '-1',
            'CloseEvent keeps negative codes');
        assert.equal(main.get('N-ce-bigint'), 'threw:TypeError',
            'CloseEvent code BigInt is a TypeError');
        assert.equal(main.get('N-ce-frac'), '1000',
            'CloseEvent code truncates fractions');
        assert.equal(main.get('N-ce-reason-symbol'), 'TypeError',
            'CloseEvent reason Symbol is a TypeError');
        assert.equal(main.get('N-bt-symbol'), 'TypeError',
            'binaryType Symbol is a TypeError');
        assert.equal(main.get('R-obj'), 'TypeError',
            'Response 101 webSocket {} is a TypeError');
        assert.equal(main.get('R-bool'), 'TypeError',
            'Response 101 webSocket true is a TypeError');
        assert.equal(main.get('R-num'), 'TypeError',
            'Response 101 webSocket 1 is a TypeError');

        // C03: close(1000.5) is accepted (WebIDL truncation)
        observation.closes = {};
        async function convertedCloseEvent(probe, expected) {
            const observed = await wsSession(port, '/ws?case=' + probe, async ws => {
                const closed = new Promise(resolve => { ws.onclose = resolve; });
                ws.send('go');
                const event = await closed;
                return { code: event.code, reason: event.reason, wasClean: event.wasClean };
            });
            assert.deepEqual(observed, expected, `${probe}: actual wire close conversion`);
            observation.closes[probe] = observed;
        }
        await convertedCloseEvent('c03', { code: 1000, reason: '', wasClean: true });
        assert.equal(byId(await state()).get('C03'), 'accepted',
            'close(1000.5) accepts');

        // C04: close(1000, Symbol) throws on reason conversion
        await convertedCloseEvent('c04', { code: 1000, reason: '', wasClean: true });
        assert.equal(byId(await state()).get('C04'), 'TypeError',
            'close(1000, Symbol) throws');

        await convertedCloseEvent('e-close', { code: 1000, reason: '123', wasClean: true });
        const convertedClose = byId(await state());
        assert.equal(convertedClose.get('E-close-reason-bigint'), 'accepted');
        assert.equal(convertedClose.get('E-close-boxed-bigint'), 'TypeError');

        // C15: concurrent message handlers (a awaits, b runs through)
                await wsSession(port, '/ws?case=c15', async (ws) => {
            ws.send('a');
            ws.send('b');
            await new Promise(resolve => setTimeout(resolve, 400));
        });
        const order = (await state()).map(entry => entry.outcome);
        observation.concurrent = order;
        // C15 (review §3.2): handlers run CONCURRENTLY — the golden in
        // BOTH modes. The earlier frame loss under concurrent dispatch
        // was root-caused as the send-side async-context guard
        // (js_channel_send from the commit chain) plus the per-endpoint
        // serial commit chain; with those in place concurrent dispatch
        // is stable (ping-flood 6/6).
        assert.deepEqual(order,
            ['start:a', 'start:b', 'end:b', 'end:a'],
            'handlers run concurrently, per-message ordering kept');

        // Both official pinned assets send the accepted frames before the
        // throw terminates the connection. Undici may discard queued events
        // on transport disposal, so capture actual TCP bytes independently.
        observation.handlerThrows = {};
        async function observeThrow(scenario) {
            // The C18 terminal must not depend on a client's Close reply.
            // The independent throw-wire regression also retains the replying peer.
            const peer = responseSession(port, `${routePrefix}/ws?case=${scenario}`,
                {autoCloseReply: scenario !== 'c18'});
            const headers = await peer.headers;
            assert.equal(headers.status, 101, `${scenario}: throw wire upgrade`);
            const accept = crypto.createHash('sha1')
                .update(headers.requestKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
            assert.equal(/^sec-websocket-accept:\s*(\S+)\s*$/im.exec(headers.rawHeader)?.[1],
                accept, `${scenario}: throw wire handshake`);
            peer.send('go');
            const raw = await peer.complete;
            assert.deepEqual(await state(), scenario === 'c17'
                ? [{id: 'C17-listener', outcome: 'first'}] : [],
                `${scenario}: raw listener order and healthy HTTP recovery`);
            const client = await wsSession(port, `/ws?case=${scenario}`, async ws => {
                const seen = [];
                const close = await new Promise(resolve => {
                    ws.onmessage = event => seen.push(event.data);
                    ws.onclose = event => resolve({code: event.code, reason: event.reason, wasClean: event.wasClean});
                    ws.send('go');
                });
                return {seen, close};
            }, {tolerateErrorAfterOpen: true});
            const observed = {wire: {hex: Buffer.from(raw.rawTail, 'base64').toString('hex'),
                frames: raw.frames, peerTerminated: raw.peerTerminated,
                completion: raw.completion, transportError: raw.transportError}, client};
            assertHandlerThrowCase(scenario, observed, assert);
            observation.handlerThrows[scenario] = observed;
        }
        await observeThrow('c17');

        observation.nativeThrowListeners = (await state())
            .filter(entry => entry.id === 'C17-listener').map(entry => entry.outcome);
        assert.deepEqual(observation.nativeThrowListeners, ['first'],
            'native throwing listener stops subsequent listeners');

        await observeThrow('c18');
        observation.handlerErrors = await observeHandlerErrors(port, routePrefix, assert);
        assertHandlerErrorDiagnostics(observation.handlerErrors, server.stderr(), assert);

        // fix-review §5: async rejections are not fatal
                await wsSession(port, '/ws?case=c19', async (ws) => {
            const seen = [];
            await new Promise((resolve, reject) => {
                ws.onmessage = event => {
                    seen.push(event.data);
                    if (event.data === 'b-after:b') resolve();
                };
                ws.onerror = () => reject(new Error('c19 transport died'));
                ws.send('a');
                ws.send('await');
                ws.send('b');
            });
            assert.deepEqual(seen, ['a-sent', 'await-sent', 'b-after:b'],
                'messages keep flowing across async rejections: '
                + JSON.stringify(seen));
            const closed = await new Promise(resolve => {
                ws.onclose = event =>
                    resolve({ code: event.code, wasClean: event.wasClean });
                ws.close(1000);
            });
            assert.equal(closed.code, 1000,
                'client-initiated close after async rejections');
            assert.equal(closed.wasClean, true,
                'async rejections leave a clean channel');
        });

        // fix-review §10: real protocol-header matrix. A raw TCP peer
        // answers the worker's dial, records Sec-WebSocket-Protocol,
        // selects the first token, and completes a clean close.
        {
            const headerLog = [];
            const peerSockets = new Set();
            const peer = net.createServer(socket => {
                peerSockets.add(socket);
                socket.on('close',()=>peerSockets.delete(socket));
                socket.on('error', () => {});
                let buffer = Buffer.alloc(0);
                let upgraded = false;
                socket.on('data', chunk => {
                    buffer = Buffer.concat([buffer, chunk]);
                    if (!upgraded) {
                        const end = buffer.indexOf('\r\n\r\n');
                        if (end < 0) return;
                        const head = buffer.subarray(0, end).toString();
                        const raw = /^sec-websocket-protocol: *(.*)$/mi.exec(head)?.[1] ?? null;
                        const header = raw === null ? null : raw.replace(/\r$/, '');
                        headerLog.push(header);
                        const key = /^sec-websocket-key: *([^\r\n]*)/mi.exec(head)?.[1];
                        assert.ok(key, 'peer parses the handshake key');
                        const badAccept = head.startsWith('GET /badAccept ');
                        const accept = crypto.createHash('sha1')
                            .update((badAccept ? 'wrong-key' : key) + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
                            .digest('base64');
                        const first = header !== null && header !== ''
                            ? header.split(',')[0].trim() : null;
                        socket.write(
                            'HTTP/1.1 101 Switching Protocols\r\n' +
                            'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
                            `Sec-WebSocket-Accept: ${accept}\r\n` +
                            (first !== null
                                ? `Sec-WebSocket-Protocol: ${first}\r\n`
                                : '') +
                            '\r\n');
                        upgraded = true;
                        buffer = buffer.subarray(end + 4);
                        // the peer initiates a clean close shortly
                        // after the upgrade (the reference does not
                        // flush a client-initiated close in this
                        // probe's request context)
                        setTimeout(() => {
                            if (!socket.destroyed) {
                                socket.write(Buffer.from(
                                    [0x88, 0x02, 0x03, 0xe8]));
                                setTimeout(() => socket.end(), 150);
                            }
                        }, 80);
                    }
                    // reply to a client close with an unmasked echo
                    // of its (masked) status code
                    if (buffer.length >= 2 && (buffer[0] & 0x0f) === 0x8) {
                        const length = buffer[1] & 0x7f;
                        if (length === 126) return;   // not in this probe
                        const masked = (buffer[1] & 0x80) !== 0;
                        const base = masked ? 6 : 2;
                        if (buffer.length < base + length) return;
                        let code = buffer.readUInt16BE(2);
                        if (masked) {
                            const key = buffer.subarray(2, 6);
                            code = ((buffer[6] ^ key[0]) << 8) |
                                (buffer[7] ^ key[1]);
                        }
                        assert.equal(code, 1000, 'client replies with the peer close code');
                        buffer = Buffer.alloc(0);
                        socket.end();
                    }
                });
            });
            await new Promise((resolve, reject) => {
                peer.once('error', reject);
                peer.listen(0, '127.0.0.1', resolve);
            });
            const dialTarget =
                `ws://127.0.0.1:${peer.address().port}/echo`;
            const matrix = {
                omitted: { header: null, protocol: null },
                undef: { header: null, protocol: null },
                null: { header: 'null', protocol: 'null' },
                num: { header: '123', protocol: '123' },
                bool: { header: 'true', protocol: 'true' },
                obj: { header: 'chat', protocol: 'chat' },
                str: { header: 'chat', protocol: 'chat' },
                arr: { header: 'chat, super', protocol: 'chat' },
                iter: { threw: 'SyntaxError' },
                badAccept: { header: null, failed: true },
            };
            try {
                for (const tag of Object.keys(matrix)) {
                    headerLog.length = 0;
                    const response = await fixtureFetch(
                        `http://127.0.0.1:${port}${routePrefix}/dial` +
                        `?case=${tag}&target=${encodeURIComponent(tag==='badAccept' ? dialTarget.replace('/echo','/badAccept') : dialTarget)}`,
                        { signal: AbortSignal.timeout(15000) });
                    await response.text();
                    let outcome = null;
                    const recorded = [];
                    for (let attempt = 0; attempt < 100 && outcome === null;
                         ++attempt) {
                        const entries = await state();
                        recorded.push(...entries.filter(e=>e.id==='C12-'+tag).map(e=>JSON.parse(e.outcome)));
                        outcome = recorded.find(e=>e.state===3 || e.constructed===false) ?? null;
                        if (!outcome) await new Promise(r => setTimeout(r, 50));
                    }
                    assert.ok(outcome !== null, 'C12 ' + tag + ' recorded');
                    const expected = matrix[tag];
                    if (expected.threw !== undefined) {
                        assert.equal(outcome.constructed, false,
                            'C12 ' + tag + ' constructor throws');
                        assert.equal(outcome.threw, expected.threw,
                            'C12 ' + tag + ' error name');
                        assert.equal(headerLog.length,0,'invalid protocols never dial');
                        observation.dial.push({tag,header:null,events:recorded});
                        continue;
                    }
                    assert.ok(outcome.constructed, 'C12 ' + tag + ' constructs');
                    const opened = recorded.find(e=>e.state===1);
                    if (expected.failed) {
                        assert.equal(opened,undefined,'wrong Accept never opens');
                        assert.equal(recorded.filter(e=>e.error).length,1);
                        assert.equal(outcome.code,1006); assert.equal(outcome.clean,false);
                    } else {
                        assert.ok(opened,'valid handshake opens');
                        assert.equal(opened.protocolAtOpen,expected.protocol);
                        assert.equal(outcome.protocolAtClose,expected.protocol);
                        assert.equal(outcome.code,1000); assert.equal(outcome.clean,true);
                        assert.equal(recorded.filter(e=>e.error).length,0,
                            'C12 '+tag+' events: '+JSON.stringify(recorded));
                    }
                    observation.dial.push({tag,header:headerLog[0]??null,events:recorded});
                    assert.equal(headerLog[0] ?? null, expected.header,
                        'C12 ' + tag + ' request header');
                }
            } finally {
                for(const socket of peerSockets) socket.destroy();
                await new Promise(resolve=>peer.close(resolve));
            }
        }

        // fix-review §11: delayed accept upgrades and still echoes
                await wsSession(port, '/delayed', async (ws) => {
            ws.send('late');
            assert.equal(await new Promise(res => {
                ws.onmessage = e => res(e.data);
            }), 'late', 'delayed-accept echo works');
            const closed = await new Promise(res => {
                ws.onclose = res;
                ws.close(1000);
            });
            assert.equal(closed.code, 1000);
            assert.equal(closed.wasClean, true);
        });

        // §11: duplicate transfer terminates explicitly (500), no hang
        observation.transfers = [];
        for (const route of ['/retransfer', '/precreated']) {
            const first = await wsSession(port, route, async ws => {
                const message = new Promise(resolve => { ws.onmessage = e => resolve(e.data); });
                ws.send('one'); assert.equal(await message, 'one');
                const closed = new Promise(resolve => { ws.onclose = resolve; });
                ws.close(1000); return closed;
            });
            assert.equal(first.code,1000); assert.equal(first.wasClean,true);
            const second = await failedUpgrade(port,route);
            assert.equal(second,500,route+': second real Upgrade fails');
            observation.transfers.push({route,first:101,second});
        }
        // Fresh endpoints from each current request remain valid controls.
        for(let i=0;i<2;i++) await wsSession(port,'/fresh',async ws => {
            const message=new Promise(resolve=>{ws.onmessage=e=>resolve(e.data);});
            ws.send('fresh'); assert.equal(await message,'fresh');
            const closed=new Promise(resolve=>{ws.onclose=resolve;});
            ws.close(1000); assert.equal((await closed).code,1000);
        });
        // Coupling an accepted endpoint fails after 101 commits. The dedicated
        // Response fixture additionally checks the wire terminal and quota reuse.
        observation.acceptedEndpointStatus = await failedUpgrade(port,'/accepted-endpoint',true);
        assert.equal(observation.acceptedEndpointStatus,101);

        // C16: outbound connection refused reports an ErrorEvent. The
        // port is an unprivileged, freshly closed listener: ECONNREFUSED
        // without tripping the reference's privileged-port peer rules.
        const closedPort = await new Promise((resolve, reject) => {
            const probe = net.createServer();
            probe.once('error', reject);
            probe.listen(0, '127.0.0.1', () => {
                const bound = probe.address().port;
                probe.close(() => resolve(bound));
            });
        });
        const refused = await (await fixtureFetch(
            `http://127.0.0.1:${port}${routePrefix}/refuse` +
            `?target=${encodeURIComponent(
                `ws://127.0.0.1:${closedPort}/refuse`)}`
        )).text();
        assert.equal(refused, 'refused-probed');
        const entries = await state();
        const found = entries.find(entry => entry.id === 'C16');
        assert.ok(found !== undefined,
            'C16 outcome recorded: ' + JSON.stringify(entries));
        const errorShape = JSON.parse(found.outcome);
        assert.ok(!('threw' in errorShape),
            'construction must not throw: ' + found.outcome);
        assert.equal(errorShape.ctor, 'ErrorEvent',
            'dial failure dispatches an ErrorEvent');
        assert.equal(errorShape.hasMessage, true, 'ErrorEvent.message exists');
        assert.equal(errorShape.hasError, true, 'ErrorEvent.error exists');
        assert.deepEqual(errorShape, {
            ctor: 'ErrorEvent', hasMessage: true, hasError: true,
            errorType: 'object', errorIsError: true, errorName: 'Error',
            errorMessage: 'Network connection lost.',
            message: 'Uncaught Error: Network connection lost.', state: 3, target: true,
            currentTarget: true, isTrusted: true, phase: 2, filename: '', lineno: 0, colno: 0,
        }, 'refused dial carries the reference Error object and stable diagnostic');
        observation.refusedError = errorShape;
        const lifecycle = entries.find(entry => entry.id === 'C16-lifecycle');
        assert.ok(lifecycle, 'runtime event lifecycle was observed');
        observation.runtimeEventLifecycle = JSON.parse(lifecycle.outcome);
        assert.deepEqual(observation.runtimeEventLifecycle, {
            after: { phase: 0, isTrusted: true, currentTarget: true },
            redispatched: { phase: 2, isTrusted: true, currentTarget: true },
        }, 'runtime event trust and dispatch lifecycle match the reference');
        return observation;
    },
    'echo.mjs': async (port) => {
        // text echo, binary echo, clean close 1000
        const text = await wsSession(port, '/echo', async (ws) => {
            const got = await new Promise((res) => {
                ws.onmessage = (e) => res(e.data);
                ws.send('hello-ref');
            });
            assert.equal(got, 'hello-ref');
            assert.equal(typeof got, 'string');
            const closed = await new Promise((res) => {
                ws.onclose = (e) => res(e);
                ws.close(1000, 'echo-done');
            });
            assert.equal(closed.code, 1000);
            assert.equal(closed.wasClean, true);
            return { message: got, close: { code: closed.code, reason: closed.reason, wasClean: closed.wasClean } };
        });
        const binary = await wsSession(
            port, '/echo',
            async (ws) => {
                const got = await new Promise((res) => {
                    ws.onmessage = (e) => res(e.data);
                    ws.send(new Uint8Array([1, 2, 3]));
                });
                assert.ok(got instanceof ArrayBuffer);
                assert.deepEqual([...new Uint8Array(got)], [1, 2, 3]);
                return [...new Uint8Array(got)];
            },
            { binaryType: 'arraybuffer' });
        return { text, binary };
    },
    'server-close.mjs': async (port) => {
        return await wsSession(port, '/close', async (ws) => {
            const closed = await new Promise((res) => {
                ws.onclose = (e) => res(e);
            });
            assert.equal(closed.code, 3000);
            assert.equal(closed.reason, 'planned');
            assert.equal(closed.wasClean, true);
            return { code: closed.code, reason: closed.reason, wasClean: closed.wasClean };
        });
    },
    'fragmented.mjs': async (port) => {
        return { message: await wsSession(
            port, '/fragmented',
            async (ws) => {
                // The native client delivers messages whole regardless
                // of how the peer fragments writes: send one 100-byte
                // message, expect the byte-length answer.
                const got = await new Promise((res) => {
                    ws.onmessage = (e) => res(e.data);
                    ws.send(new Uint8Array(100).fill(7));
                });
                assert.ok(got instanceof ArrayBuffer);
                assert.deepEqual([...new Uint8Array(got)], [100]);
                return [...new Uint8Array(got)];
            },
            { binaryType: 'arraybuffer' }) };
    },
    'double-accept.mjs': async (port) => {
        return await wsSession(port, '/double-accept', async (ws) => {
            const report = await new Promise((res) => {
                ws.onmessage = (e) => res(e.data);
                ws.send('probe');
            });
            // verified: silent no-op at the pinned commit
            assert.equal(report, 'double-accept:null');
            // and the socket still works after the throw
            const echo = await new Promise((res) => {
                ws.onmessage = (e) => res(e.data);
                ws.send('still-alive');
            });
            assert.equal(echo, 'still-alive');
            const closed = await new Promise((res) => {
                ws.onclose = (e) => res(e);
                ws.close(1000);
            });
            assert.equal(closed.wasClean, true);
            return { report, echo, close: { code: closed.code, reason: closed.reason, wasClean: closed.wasClean } };
        });
    },
    'close-boundary.mjs': async (port) => {
        const planned = await wsSession(port, '/close-boundary', async (ws) => {
            const report = await new Promise((res) => {
                ws.onmessage = (e) => res(e.data);
                ws.send('go');
            });
            assert.equal(report, 'close-999-threw:InvalidAccessError');
            const closed = await new Promise((res) => {
                ws.onclose = (e) => res(e);
            });
            assert.equal(closed.code, 4321);
            assert.equal(closed.reason, 'edge');
            assert.equal(closed.wasClean, true);
            return { report, close: { code: closed.code, reason: closed.reason, wasClean: closed.wasClean } };
        });
        // client-originated invalid close code (999) is a wire
        // protocol error: server fails the connection, native client
        // reports 1006 / not clean
        const invalidClose = await new Promise((resolve, reject) => {
            const observer = {
                sawReply: false,
                sentInvalid: false,
                lastClose: 'pending',
                result() {
                    // terminal state: a close frame if one arrived,
                    // else 'dropped' for a hard transport failure
                    if (this.lastClose === 'pending') {
                        this.lastClose = 'dropped';
                    }
                    return this.lastClose;
                },
                onOpen(socket) {
                    this.socket = socket;
                    this.buffer = Buffer.alloc(0);
                    socket.write(rawTextFrame('trigger'));
                },
                onFrame(chunk, socket) {
                    this.buffer = Buffer.concat([this.buffer, chunk]);
                    if (!this.sawReply && this.buffer.includes(
                        Buffer.from('close-999-threw'))) {
                        this.sawReply = true;
                        this.buffer = Buffer.alloc(0);
                        // 999 is outside every valid range: the server
                        // must fail the connection
                        socket.write(rawFrame(
                            0x8,
                            Buffer.from([0x03, 0xe7, 0x69, 0x6e])));
                        return;
                    }
                    const close = parseCloseFrame(this.buffer);
                    if (close !== null) {
                        this.lastClose = close;
                    }
                },
            };
            const timer = setTimeout(
                () => reject(new Error('close-boundary raw timeout')), 8000);
            rawWsSession(port, '/invalid-client-close', observer).then(
                (outcome) => {
                    clearTimeout(timer);
                    try {
                        // a hard drop is the 1006-equivalent; a close
                        // frame, if any, must be a protocol-failure code
                        // (1002/1003), never 999 echoing back
                        if (backend === 'workerd') {
                            assert.deepEqual(outcome,{code:999,reason:'in'},'reference auto-replies without validating the received close code');
                        } else {
                            assert.equal(outcome.code,1002,'Capsid rejects the invalid received close code at the RFC layer');
                        }
                        resolve(outcome);
                    } catch (error) {
                        reject(error);
                    }
                },
                reject);
        });
        return { planned, invalidClose };
    },
    'fanout.mjs': async (port) => {
        // verified: workerd refuses cross-request I/O on inbound
        // sockets; the sender gets the failure report, the peer
        // nothing, and both sockets remain usable
        const makePeer = () => new Promise((resolve, reject) => {
            const ws = new WebSocket(`ws://127.0.0.1:${port}${routePrefix}/fanout`);
            ws.binaryType = 'arraybuffer';
            const peer = { ws, inbox: [] };
            ws.onopen = () => resolve(peer);
            ws.onerror = () => reject(new Error('fanout peer failed'));
            ws.onmessage = (e) => {
                peer.inbox.push(Buffer.from(e.data).toString('utf8'));
            };
        });
        const [a, b] = await Promise.all([makePeer(), makePeer()]);
        const identities=[];
        for (const [index, peer] of [a,b].entries()) {
            const challenge=`fixture-identity-${index}`;
            peer.ws.send('@capsid-fanout:'+JSON.stringify({op:'identity',challenge}));
            const identityDeadline=performance.now()+5000;
            while(peer.inbox.length===0) {
                assert.ok(performance.now()<identityDeadline,'fanout identity reply deadline');
                await new Promise(resolve=>setTimeout(resolve,10));
            }
            const identity=JSON.parse(peer.inbox.shift());
            assert.equal(identity.op,'identity');assert.equal(identity.challenge,challenge);
            assert.match(identity.instance,/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
            assert.match(identity.connection,/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
            identities.push({instance:identity.instance,connection:identity.connection});
        }
        assert.equal(identities[0].instance,identities[1].instance,'legacy fanout peers share one actual Worker instance');
        assert.notEqual(identities[0].connection,identities[1].connection,'distinct actual fanout connections');
        a.ws.send('from-a');
        const deadline = Date.now() + 8000;
        while (a.inbox.length === 0 && Date.now() < deadline) {
            await new Promise((r) => setTimeout(r, 20));
        }
        assert.equal(a.inbox.length, 1, 'exactly one report to sender');
        assert.ok(a.inbox[0].startsWith('fanout:blocked:'),
            `expected the I/O-context block, got ${a.inbox[0]}`);
        assert.ok(a.inbox[0].includes(
            'Cannot perform I/O on behalf of a different request'),
            'error text pins the verified workerd limitation');
        await new Promise((r) => setTimeout(r, 200));
        assert.equal(b.inbox.length, 0, 'peer receives nothing');
        // both sockets stay usable after the failed relay
        const echo = await new Promise((res, rej) => {
            const timer = setTimeout(
                () => rej(new Error('post-failure exchange timed out')), 5000);
            b.ws.onmessage = (e) => {
                clearTimeout(timer);
                res(Buffer.from(e.data).toString('utf8'));
            };
            b.ws.send('still-usable');
        });
        // b's own relay attempt also reports blocked back to b
        assert.ok(echo.startsWith('fanout:blocked:'));
        const closed = await Promise.all([a, b].map((peer) =>
            new Promise((res) => {
                peer.ws.onclose = res;
                peer.ws.close(1000);
            })));
        assert.ok(closed.every((e) => e.wasClean));
        // Engines add different stack diagnostics. Keep the stable observed
        // failure category; assertions above validate the diagnostic itself.
        return { identities, sender: a.inbox[0].split(':').slice(0, 2).join(':'), peerMessages: b.inbox,
            survivor: echo.split(':').slice(0, 2).join(':'),
            closes: closed.map(e => ({ code: e.code, reason: e.reason, wasClean: e.wasClean })) };
    },
    'ping-flood.mjs': async (port) => {
        // verified terminal contract: pongs mirror payloads in order
        // (partial under flood), data interleaves fully, then the
        // The driver observes control/data progress, then ends the session.
        return await new Promise((resolve, reject) => {
            const observer = {
                buffer: Buffer.alloc(0),
                pongs: 0,
                echoes: 0,
                pongPayloads: [],
                dataPayloads: [],
                lastPayload: null,
                lastFrameAt: Date.now(),
                settled: false,
                result() { return null; },
                onOpen(socket) {
                    this.socket = socket;
                    for (let index = 0; index < 50; index += 1) {
                        socket.write(rawFrame(0x9, Buffer.from(
                            'ping-' + index.toString().padStart(3, '0'))));
                        if (index % 10 === 0) {
                            socket.write(rawTextFrame('data-' + index));
                        }
                    }
                },
                onFrame(chunk, socket) {
                    if (chunk.length > 0) {
                        this.lastFrameAt = Date.now();
                    }
                    this.buffer = Buffer.concat([this.buffer, chunk]);
                    const { frames, rest } = rawServerMessages(this.buffer, this);
                    this.buffer = Buffer.from(rest);
                    for (const frame of frames) {
                        if (frame.opcode === 0xa) {
                            const text = frame.payload.toString('utf8');
                            assert.match(text, /^ping-0[0-4][0-9]$/, 'pong payload mirrors a sent ping');
                            if (this.lastPayload !== null) {
                                assert.ok(
                                    Number(text.slice(5)) >
                                        Number(this.lastPayload.slice(5)),
                                    `pongs arrive in order: ${this.lastPayload} then ${text}`);
                            }
                            this.lastPayload = text;
                            this.pongPayloads.push(text);
                            this.pongs += 1;
                        } else if (frame.opcode === 0x1) {
                            assert.equal(
                                frame.payload.toString('utf8'),
                                'data-' + this.echoes * 10);
                            this.echoes += 1;
                            this.dataPayloads.push(frame.payload.toString('utf8'));
                        }
                    }
                },
            };
            const timer = setTimeout(() => reject(new Error(
                `ping-flood: no settled terminal state ` +
                `(pongs=${observer.pongs}, echoes=${observer.echoes})`)),
                20000);
            // End after data delivery and a quiet observation window. RFC 6455
            // permits coalesced pongs; both partial and complete delivery are legal.
            const settle = setInterval(() => {
                if (observer.settled) {
                    return;
                }
                if (observer.echoes === 5 && observer.pongs >= 1 &&
                        Date.now() - observer.lastFrameAt > 3000) {
                    observer.settled = true;
                    clearInterval(settle);
                    clearTimeout(timer);
                    try {
                        observer.socket.destroy();
                        resolve({ pongs: observer.pongPayloads, echoes: observer.dataPayloads, driver_closed_socket: true });
                    } catch (error) {
                        reject(error);
                    }
                }
            }, 250);
            rawWsSession(
                port, '/ping-flood', observer, { timeoutMs: 22000 }).then(
                () => {
                    clearInterval(settle);
                    clearTimeout(timer);
                    reject(new Error(
                        'ping-flood: connection ended without stall'));
                },
                (error) => {
                    clearInterval(settle);
                    clearTimeout(timer);
                    reject(error);
                });
        });
    },
    'disconnect-mid-stream.mjs': async port => {
        const received = await rawWsSession(port, '/disconnect-mid-stream', {
            bytes: 0,
            onOpen(socket) { socket.write(rawTextFrame('go')); },
            onFrame(chunk, socket) {
                this.bytes += chunk.length;
                if (this.bytes > 32768) {
                    if (socket.resetAndDestroy) socket.resetAndDestroy(); else socket.destroy();
                }
            }, result() { return this.bytes; },
        });
        assert.ok(received > 32768, 'reset interrupts an active burst');
        const survivor = await rawWsSession(port, '/disconnect-mid-stream/2', {
            buffer: Buffer.alloc(0), observed: null,
            onOpen(socket) { socket.write(rawTextFrame('survivor')); },
            onFrame(chunk, socket) {
                if (this.observed) return;
                this.buffer = Buffer.concat([this.buffer, chunk]);
                const parsed = rawServerMessages(this.buffer, this); this.buffer = parsed.rest;
                const data = parsed.frames.find(f => f.opcode === 2);
                if (!data) return;
                assert.equal(data.payload.length, 65536);
                assert.ok(data.payload.every(byte => byte === 0x41), 'whole survivor payload is intact');
                this.observed = { bytes: data.payload.length,
                    sha256: crypto.createHash('sha256').update(data.payload).digest('hex') };
                socket.destroy();
            }, result() { return this.observed; },
        });
        assert.ok(survivor, 'same process delivers a new burst after reset');
        return { resetAfterData: received > 32768, survivor };
    },
    'slow-consumer.mjs': async port => {
        const observation = { frames: 0, bytes: 0, closeFrame: null,
            peerClosed: false, completionMessage: null, payloadByte: 0x5a };
        await rawWsSession(port, '/slow-consumer', {
            buffer: Buffer.alloc(0), paused: true, closing: false,
            onOpen(socket) {
                socket.pause(); socket.write(rawTextFrame('go'));
                setTimeout(() => { this.paused = false; socket.resume(); }, 600);
            },
            onFrame(chunk, socket) {
                assert.equal(this.paused, false, 'no delivery while reads are paused');
                this.buffer = Buffer.concat([this.buffer, chunk]);
                const parsed = rawServerMessages(this.buffer, this); this.buffer = parsed.rest;
                for (const frame of parsed.frames) {
                    if (frame.opcode === 2) {
                        assert.equal(frame.payload.length, 65536);
                        assert.ok(frame.payload.every(byte => byte === 0x5a), 'every burst byte is intact');
                        observation.frames++; observation.bytes += frame.payload.length;
                    } else if (frame.opcode === 1) {
                        observation.completionMessage = frame.payload.toString();
                        assert.equal(observation.completionMessage, 'slow-burst-done');
                    } else if (frame.opcode === 8) {
                        observation.closeFrame = frame.payload.readUInt16BE(); socket.end();
                    }
                }
                if (observation.completionMessage && !this.closing) {
                    this.closing = true; socket.write(rawFrame(8, Buffer.from([3, 0xe8])));
                }
            }, result() { observation.peerClosed = true; return observation; },
        }, { timeoutMs: 20000 });
        assert.equal(observation.frames, 128); assert.equal(observation.bytes, 8388608);
        assert.equal(observation.closeFrame, 1000);
        const response = await fixtureFetch(`http://127.0.0.1:${port}${routePrefix}/selftest`);
        observation.healthyStatus = response.status; assert.equal(response.status, 200);
        return observation;
    },
    'invalid-utf8.mjs': async (port) => {
        // Both backends decode malformed message text lossily. Close
        // reasons remain separately validated by the transport.
        const cases = [
            { bytes: [0xff, 0xfe, 0xfd, 0x62], expected: 'cps:fffd,fffd,fffd,62' },
            { bytes: [0xed, 0xa0, 0x80], expected: 'cps:fffd,fffd,fffd' },
            { bytes: [0xf4, 0x90, 0x80, 0x80], expected: 'cps:fffd,fffd,fffd,fffd' },
            { bytes: [0xe2, 0x82], expected: 'cps:fffd' },
        ];
        const observed = [];
        let next = 0;
        await rawWsSession(port, '/invalid-utf8', {
            buffer: Buffer.alloc(0), stage: 'probe',
            onOpen(socket) { socket.write(rawTextFrame('probe')); },
            onFrame(chunk, socket) {
                this.buffer = Buffer.concat([this.buffer, chunk]);
                const parsed = rawServerMessages(this.buffer, this);
                this.buffer = parsed.rest;
                for (const frame of parsed.frames) {
                    if (this.stage === 'probe') {
                        assert.equal(frame.opcode, 1);
                        assert.equal(frame.payload.toString(), 'cps:70,72,6f,62,65');
                        this.stage = 'invalid';
                    } else if (this.stage === 'invalid') {
                        assert.equal(frame.opcode, 1, 'malformed text remains a message');
                        const text = frame.payload.toString();
                        assert.equal(text, cases[next - 1].expected);
                        observed.push(text);
                    } else if (this.stage === 'binary') {
                        assert.equal(frame.opcode, 2);
                        assert.deepEqual([...frame.payload], [1, 2, 3]);
                        this.stage = 'closing';
                        socket.write(rawFrame(8, Buffer.from([3, 232])));
                        continue;
                    } else if (this.stage === 'closing') {
                        assert.equal(frame.opcode, 8);
                        assert.equal(frame.payload.readUInt16BE(), 1000);
                        this.stage = 'done'; socket.end(); continue;
                    } else assert.fail('unexpected frame after clean close');
                    if (next < cases.length)
                        socket.write(rawFrame(1, Buffer.from(cases[next++].bytes)));
                    else { this.stage = 'binary'; socket.write(rawFrame(2, Buffer.from([1, 2, 3]))); }
                }
            },
            result() { assert.equal(this.stage, 'done', 'malformed text connection closes cleanly'); return observed; },
        });
        assert.equal(observed.length, cases.length);
        return observed;
    },
};

async function runReference(binary) {
    const lock = readLock(path.join(fixtureDir, path.basename(lockPath)));
    const sha = crypto.createHash('sha256').update(fs.readFileSync(binary)).digest('hex');
    assert.equal(sha, lock.binary_sha256,
        `workerd binary ${sha} does not match the reference lock ` +
        `${lock.binary_sha256} (source commit ${lock.source_commit})`);

    assert.equal(process.version, lock.client.node_version, 'locked Node client version');
    assert.equal(process.versions.undici, lock.client.undici_version, 'locked bundled undici version');
    assert.equal(execFileSync(binary, ['--version'], { encoding: 'utf8' }).trim(),
        lock.binary_version_string, 'locked binary version');
    assert.equal(lock.compatibility_date, '2026-08-20');
    assert.deepEqual(lock.compatibility_flags, [], 'reference flags must match the reviewed profile');
    const results = [];
    const filter = process.env.CAPSID_WDREF_FILTER;
    const selected = filter
        ? filter.split(',').map(
            (name) => name.trim().endsWith('.mjs') || name === ''
                ? name.trim()
                : name.trim() + '.mjs')
        : fixtures;
    assert.ok(selected.length > 0 && new Set(selected).size === selected.length,
        'reference selection is nonempty and unique');
    for (const fixture of selected) {
        assert.ok(fixtures.includes(fixture), `unknown fixture ${fixture}`);
        const scenario = scenarios[fixture];
        assert.equal(typeof scenario, 'function', `driver missing for ${fixture}`);
        const dir = fs.mkdtempSync(
            path.join(os.tmpdir(), 'capsid-wdref-'));
        const port = 18990 + (fixtures.indexOf(fixture) % 8);
        const server = startWorkerd(binary, fixture, port, dir, lock);
        await withCleanup(async () => {
        try {
            await waitPort(port, 5000);
            const before = assertionCount;
            const observation = await scenario(port, { server });
            const assertions = assertionCount - before;
            assert.ok(assertions > 0, `${fixture} executed at least one absolute assertion`);
            results.push({ fixture, passed: true, assertions, observation: observation ?? null });
        } catch (error) {
            console.error(`fixture=${fixture} port=${port} pid=` +
                `${server.child.pid} exit=${server.child.exitCode} ` +
                `signal=${server.child.signalCode}`);
            console.error('workerd stderr:', server.stderr() || '(empty)');
            throw error;
        }
        }, async () => {
            try {
                const shutdown = await stopCapsid(server.child);
                assert.equal(shutdown.forced, false, `${fixture}: reference shutdown ${JSON.stringify(shutdown)}`);
                assert.ok(shutdown.code===0 || shutdown.signal==='SIGTERM', 'reference shutdown terminal');
                if(results.at(-1)?.fixture===fixture) results.at(-1).shutdown=shutdown;
            } finally {
                fs.rmSync(dir, { recursive: true, force: true });
            }
        });
    }
    const passed = results.filter((r) => r.passed);
    assert.equal(passed.length, selected.length, 'every selected fixture actually ran');
    const report = {
        schema: 'capsid-workerd-ws-reference-v1', lock,
        generated_at: new Date().toISOString(),
        runner_sha256: crypto.createHash('sha256').update(fs.readFileSync(url.fileURLToPath(import.meta.url))).digest('hex'),
        binary_sha256: sha, client: { node: process.version, undici: process.versions.undici },
        fixtures: fixtures.map((fixture) => ({ fixture, sha256: crypto.createHash('sha256')
            .update(fs.readFileSync(path.join(fixtureDir, fixture))).digest('hex') })),
        complete: selected.length === fixtures.length, results,
    };
    const reportPath = parseArgs(process.argv.slice(2)).get('report') ??
        (process.env.CAPSID_WS_REPORT_DIR ? path.join(process.env.CAPSID_WS_REPORT_DIR,'reference.json') : null);
    if (reportPath) fs.writeFileSync(reportPath, JSON.stringify(report, null, 2) + '\n');
    console.log(`workerd reference: ${passed.length}/${selected.length} fixtures passed; ` +
        `${results.reduce((n, item) => n + item.assertions, 0)} assertions`);
    return results;
}

// ---------- capsid branch ----------

async function capsidBranch() {
    const manifestText = fs.readFileSync(
        path.join(here, '..', 'js', 'profile-manifest.js'), 'utf8');
    assert.match(manifestText, /'WebSocketPair'/,
        'required WebSocketPair feature is missing from the Capsid profile');
    const args=parseArgs(process.argv.slice(2));
    const host=args.get('host'),worker=args.get('worker'),mode=args.get('mode')??'single-worker';
    assert.equal(typeof host,'string','Capsid --host is required');
    assert.equal(typeof worker,'string','Capsid --worker is required');
    routePrefix='/@capsid/orders'; backend='capsid';
    const filter=process.env.CAPSID_WDREF_FILTER;
    const selected=filter?filter.split(',').map(s=>s.trim().endsWith('.mjs')?s.trim():s.trim()+'.mjs'):fixtures;
    assert.ok(selected.length>0 && new Set(selected).size===selected.length,'nonempty unique fixture selection');
    const results=[];
    for(const fixture of selected) {
        assert.ok(fixtures.includes(fixture),`unknown fixture ${fixture}`);
        assert.equal(typeof scenarios[fixture],'function',`driver missing for ${fixture}`);
        const dir=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-ws-host-'));
        const bundle=path.join(dir,'main.mjs');fs.copyFileSync(path.join(fixtureDir,fixture),bundle);
        let server;
        await withCleanup(async () => {
        try {
            // api-compat's C12 matrix dials a loopback peer; the
            // reference allows it, so capsid needs matching egress
            // (managed also requires the host-level fetchTargets grant)
            server=await startCapsid(path.resolve(host),path.resolve(worker),bundle,dir,mode,
                {permissions:{modules:[],fetch:{allow:['127.0.0.1']}},
                 fetchTargets:['127.0.0.1'],
                 ...(fixture==='response.mjs'?{channelCapacity:{channelsTotal:2},requestTimeoutMs:30000}:{})});
            const before=assertionCount;
            const observation=fixture==='slow-consumer.mjs'
                ? await capsidBoundedBurst(server.port) : await scenarios[fixture](server.port,{server});
            const assertions=assertionCount-before;
            assert.ok(assertions>0,`${fixture} has executed absolute assertions`);
            results.push({fixture,passed:true,assertions,observation:observation??null,
                deviation:fixture==='close-boundary.mjs'?'strict received close-code validation (1002 instead of echoing 999)':fixture==='slow-consumer.mjs'?'finite 4 MiB send queue rejects synchronous 8 MiB burst':null});
            console.log(`Capsid ${mode}: ${fixture} PASS (${assertions} assertions)`);
        } catch(error) {console.error(`fixture=${fixture}; stderr=${server?.stderr()??'(startup failed)'}`);throw error;}
        }, async () => {
            try {
                if(server) {
                    const shutdown=await stopCapsid(server.child);
                    if(results.at(-1)?.fixture===fixture) results.at(-1).shutdown=shutdown;
                    assert.equal(shutdown.forced,false,`${fixture}: shutdown ${JSON.stringify(shutdown)} ${server.stderr()}`);
                    assert.equal(shutdown.code,0,`${fixture}: Host exited normally`);
                    assert.equal(shutdown.signal,null,`${fixture}: Host exits without a signal`);
                }
            } finally {
                fs.rmSync(dir,{recursive:true,force:true});
            }
        });
    }
    assert.equal(results.length,selected.length,'all selected Capsid fixtures executed');
    const report={schema:'capsid-ws-fixtures-v1',lock:readLock(path.join(fixtureDir,path.basename(lockPath))),generated_at:new Date().toISOString(),mode,
        complete:selected.length===fixtures.length,
        reference_parity:!results.some(r=>r.deviation || observationExceptions[r.fixture]),results,
        runner_sha256:crypto.createHash('sha256').update(fs.readFileSync(url.fileURLToPath(import.meta.url))).digest('hex'),
        host_sha256:crypto.createHash('sha256').update(fs.readFileSync(host)).digest('hex'),
        worker_sha256:crypto.createHash('sha256').update(fs.readFileSync(worker)).digest('hex'),
        client:{node:process.version,undici:process.versions.undici},
        fixtures:fixtures.map(fixture=>({fixture,sha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(fixtureDir,fixture))).digest('hex')}))};
    const reportPath=args.get('report') ?? (process.env.CAPSID_WS_REPORT_DIR ? path.join(process.env.CAPSID_WS_REPORT_DIR,mode+'.json') : null);
    if(reportPath) fs.writeFileSync(reportPath,JSON.stringify(report,null,2)+'\n');
    console.log(`Capsid ${mode}: ${results.length}/${selected.length} fixtures passed; ${results.reduce((n,r)=>n+r.assertions,0)} assertions`);
}

async function capsidBoundedBurst(port) {
    const observation={frames:0,bytes:0,closeFrame:null,peerClosed:false,completionMessage:null,payloadByte:0x5a};
    await rawWsSession(port,'/slow-consumer',{
        buffer:Buffer.alloc(0),
        onOpen(socket) {socket.pause();socket.write(rawTextFrame('go'));setTimeout(()=>socket.resume(),600);},
        onFrame(chunk,socket) {
            this.buffer=Buffer.concat([this.buffer,chunk]);
            const parsed=rawServerMessages(this.buffer,this);this.buffer=parsed.rest;
            for(const frame of parsed.frames) {
                if(frame.opcode===2) {assert.equal(frame.payload.length,65536);assert.ok(frame.payload.every(b=>b===0x5a));observation.frames++;observation.bytes+=frame.payload.length;}
                if(frame.opcode===8) {observation.closeFrame=frame.payload.readUInt16BE();socket.write(rawFrame(8,frame.payload));socket.end();}
            }
        },result(){observation.peerClosed=true;return observation;},
    },{timeoutMs:20000});
    // C17-aligned (review §3.2): a native budget rejection inside the
    // handler commit is an INTERNAL error — the transport aborts with
    // no close frame (the client observes 1006/wasClean=false), not a
    // 1011 network Close.
    assert.equal(observation.closeFrame,null,'budget abort carries no close frame');
    assert.ok(observation.frames>0 && observation.frames<128,'bounded send rejects the burst before unlimited queue growth');
    const response=await fixtureFetch(`http://127.0.0.1:${port}${routePrefix}/selftest`);
    observation.healthyStatus=response.status;
    assert.equal(response.status,200,'same worker remains responsive after scoped pressure');
    return observation;
}

// ---------- main ----------

const args = parseArgs(process.argv.slice(2));
const explicitBinary = typeof args.get('workerd') === 'string'
    ? args.get('workerd')
    : null;
const referenceBinary = explicitBinary !== null
    ? explicitBinary
    : (args.get('reference-from-env') === true
        ? (process.env.CAPSID_WORKERD_REF ?? '')
        : null);
if (referenceBinary !== null && referenceBinary !== '') {
    runReference(referenceBinary).then(
        () => { process.exitCode = 0; },
        (error) => { console.error(error); process.exitCode = 1; });
} else if (referenceBinary === '') {
    if(process.env.CAPSID_WORKERD_REQUIRED==='1') throw new Error('CAPSID_WORKERD_REF is required');
    console.log('SKIP: CAPSID_WORKERD_REF is not set ' +
        '(locked workerd reference binary unavailable)');
    process.exit(77);
} else {
    capsidBranch().then(()=>{process.exitCode=0;},error=>{console.error(error);process.exitCode=1;});
}
