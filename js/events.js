// Public event constructors for the pinned workerd default profile.
import { Event, toEventString } from 'txiki-polyfills/event-target.js';

const messagePorts = new WeakSet();
export const registerMessagePort = port => messagePorts.add(port);

export function eventInteger(value) {
    const number = value === undefined ? 0 : +value;
    if (!Number.isFinite(number)) return 0;
    // workerd's default JSG conversion checks the signed range before truncation.
    if (number < -2147483648 || number > 2147483647) {
        throw new TypeError('Event integer is outside the signed 32-bit range');
    }
    return Math.trunc(number) || 0;
}

export class ErrorEvent extends Event {
    #message;
    #filename;
    #lineno;
    #colno;
    #error;

    constructor(type, init = null) {
        if (arguments.length === 0) throw new TypeError('ErrorEvent requires a type');
        super(type, init);
        const options = init ?? {};
        const message = options.message;
        this.#message = message === undefined ? '' : toEventString(message);
        const filename = options.filename;
        this.#filename = filename === undefined ? '' : toEventString(filename);
        this.#lineno = eventInteger(options.lineno);
        this.#colno = eventInteger(options.colno);
        this.#error = options.error;
    }

    get message() { return this.#message; }
    get filename() { return this.#filename; }
    get lineno() { return this.#lineno; }
    get colno() { return this.#colno; }
    get error() { return this.#error; }
    get [Symbol.toStringTag]() { return 'ErrorEvent'; }
}

export class MessageEvent extends Event {
    #data;
    #origin;
    #lastEventId;
    #source;
    #ports;

    constructor(type, init = null) {
        if (arguments.length === 0) throw new TypeError('MessageEvent requires a type');
        super(type, init);
        const options = init ?? {};
        const data = options.data;
        this.#data = data === undefined ? null : data;
        const origin = options.origin;
        this.#origin = origin === undefined ? '' : toEventString(origin);
        const lastEventId = options.lastEventId;
        this.#lastEventId = lastEventId === undefined ? '' : toEventString(lastEventId);
        const source = options.source;
        if (source !== null && source !== undefined && !messagePorts.has(source)) {
            throw new TypeError('MessageEvent source must be a MessagePort');
        }
        this.#source = source ?? null;
        const ports = options.ports;
        if (ports !== undefined && !Array.isArray(ports)) {
            throw new TypeError('MessageEvent ports must be an array');
        }
        this.#ports = [];
        for (const port of ports ?? []) {
            if (!messagePorts.has(port)) {
                throw new TypeError('MessageEvent ports must contain MessagePorts');
            }
            this.#ports.push(port);
        }
    }

    get data() { return this.#data; }
    get origin() { return this.#origin; }
    get lastEventId() { return this.#lastEventId; }
    get source() { return this.#source; }
    get ports() { return this.#ports.slice(); }
    get [Symbol.toStringTag]() { return 'MessageEvent'; }
}

export class CloseEvent extends Event {
    #code;
    #reason;
    #wasClean;

    constructor(type, init = null) {
        if (arguments.length === 0) throw new TypeError('CloseEvent requires a type');
        super(type, init);
        const options = init ?? {};
        this.#code = eventInteger(options.code);
        const reason = options.reason;
        this.#reason = reason === undefined ? '' : toEventString(reason, '\uFFFD');
        this.#wasClean = !!options.wasClean;
    }

    get code() { return this.#code; }
    get reason() { return this.#reason; }
    get wasClean() { return this.#wasClean; }
    get [Symbol.toStringTag]() { return 'CloseEvent'; }
}

for (const Constructor of [ErrorEvent, MessageEvent, CloseEvent]) {
    for (const key of Object.getOwnPropertyNames(Constructor.prototype)) {
        if (key !== 'constructor') {
            Object.defineProperty(Constructor.prototype, key, { enumerable: true });
        }
    }
    Object.defineProperty(globalThis, Constructor.name, {
        configurable: true, enumerable: true, writable: true, value: Constructor,
    });
}
