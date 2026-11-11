// Minimal MessageChannel/MessagePort polyfill for v26.6.0.
import core from 'tjs:internal/core';
import { MessageEvent, registerMessagePort } from './events.js';
import { defineEventAttribute, dispatchTrustedEvent } from 'txiki-polyfills/event-target.js';
const portConstruction = Symbol('MessageChannel port');

// The locked workerd default delivers messages through Promise reactions.
// Capture the intrinsic so public Promise overrides cannot change scheduling.
const portResolvedPromise = Promise.resolve();
const enqueuePortJob = Function.prototype.call.bind(Promise.prototype.then, portResolvedPromise);

class MessagePort extends EventTarget {
    constructor(token) {
        super();
        if (token !== portConstruction) throw new TypeError('Illegal constructor');
        registerMessagePort(this);
        this._entangled = null;
        this._closed = false;
        this._started = true;
    }

    get [Symbol.toStringTag]() {
        return 'MessagePort';
    }

    postMessage(data, transferList) {
        if (!this._entangled) {
            return;
        }
        const transferPorts = [];
        if (transferList && transferList.length > 0) {
            for (const item of transferList) {
                if (item instanceof MessagePort && item._closed) {
                    throw new DOMException('', 'DataCloneError');
                }
                if (item instanceof MessagePort) {
                    transferPorts.push(item);
                } else if (core.isArrayBuffer(item)) {
                    core.detachArrayBuffer(item);
                }
            }
        }
        const target = this._entangled;
        enqueuePortJob(() => {
            dispatchTrustedEvent(target,
                new MessageEvent('message', { data, source: target, ports: transferPorts }));
        });
    }

    start() {
        this._started = true;
    }

    close() {
        if (this._closed) return;
        this._closed = true;
        if (this._entangled) {
            this._entangled._entangled = null;
            this._entangled = null;
        }
    }
}

defineEventAttribute(MessagePort.prototype, 'message');

class MessageChannel {
    get [Symbol.toStringTag]() {
        return 'MessageChannel';
    }

    constructor() {
        const port1 = new MessagePort(portConstruction);
        const port2 = new MessagePort(portConstruction);
        port1._entangled = port2;
        port2._entangled = port1;
        this.port1 = port1;
        this.port2 = port2;
    }
}

Object.defineProperties(globalThis, {
    MessageChannel: {
        enumerable: true, configurable: true, writable: true,
        value: MessageChannel,
    },
    MessagePort: {
        enumerable: true, configurable: true, writable: true,
        value: MessagePort,
    },
});
