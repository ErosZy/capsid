// Minimal MessageChannel/MessagePort polyfill for v26.6.0.
import core from 'tjs:internal/core';

// PortMessageEvent — extends vendor's MessageEvent with a `ports` getter.
class PortMessageEvent extends MessageEvent {
    constructor(type, data, ports) {
        super(type, data);
        this._ports = Object.freeze(ports ?? []);
    }
    get ports() {
        return this._ports;
    }
}

class MessagePort extends EventTarget {
    constructor() {
        super();
        this._entangled = null;
        this._closed = false;
        this._started = true;
    }

    get [Symbol.toStringTag]() {
        return 'MessagePort';
    }

    get onmessage() {
        return this._onmessageHandler ?? null;
    }

    set onmessage(fn) {
        if (this._onmessageHandler) {
            this.removeEventListener('message', this._onmessageHandler);
        }
        this._onmessageHandler = fn;
        if (typeof fn === 'function') {
            this.addEventListener('message', fn);
        }
        this._started = true;
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
        setTimeout(() => {
            target.dispatchEvent(
                new PortMessageEvent('message', data,
                    transferPorts.length > 0 ? transferPorts : undefined));
        }, 0);
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

class MessageChannel {
    get [Symbol.toStringTag]() {
        return 'MessageChannel';
    }

    constructor() {
        const port1 = new MessagePort();
        const port2 = new MessagePort();
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
