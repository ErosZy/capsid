const NativeURL = globalThis.URL;
const fallbackStates = new WeakMap();

function toUSVString(value) {
    const string = String(value);
    return string.replace(/[\uD800-\uDFFF]/g, (codeUnit, index) => {
        const code = codeUnit.charCodeAt(0);
        if (code <= 0xDBFF) {
            const next = string.charCodeAt(index + 1);
            if (next >= 0xDC00 && next <= 0xDFFF) {
                return codeUnit;
            }
        } else {
            const previous = string.charCodeAt(index - 1);
            if (previous >= 0xD800 && previous <= 0xDBFF) {
                return codeUnit;
            }
        }
        return '\uFFFD';
    });
}

function parseInvalidAsciiIdna(input) {
    const match =
        /^(https?|file):\/\/([^/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(input);
    if (!match || match[2].includes('@') || match[2].includes(':')) {
        return undefined;
    }
    const hostname = match[2].toLowerCase();
    const hasInvalidAsciiIdna =
        hostname.split('.').some(label =>
            label === 'xn--' || label === 'xn--pokxncvks');
    if (!hasInvalidAsciiIdna) {
        return undefined;
    }

    const protocol = `${match[1].toLowerCase()}:`;
    const pathname = match[3] || '/';
    const search = match[4] || '';
    const hash = match[5] || '';
    const origin = protocol === 'file:'
        ? 'null'
        : `${protocol}//${hostname}`;
    return {
        hash,
        host: hostname,
        hostname,
        href: `${protocol}//${hostname}${pathname}${search}${hash}`,
        origin,
        password: '',
        pathname,
        port: '',
        protocol,
        search,
        searchParams: new URLSearchParams(search),
        username: '',
    };
}

function fallbackValue(object, name, nativeGetter) {
    const state = fallbackStates.get(object);
    return state === undefined ? nativeGetter.call(object) : state[name];
}

const nativeDescriptors =
    Object.getOwnPropertyDescriptors(NativeURL.prototype);

class URL extends NativeURL {
    constructor(input, base = undefined) {
        if (arguments.length === 0) {
            throw new TypeError(
                "Failed to construct 'URL': 1 argument required");
        }
        input = toUSVString(input);
        const normalizedBase =
            base === undefined ? undefined : toUSVString(base);
        try {
            if (normalizedBase === undefined) {
                super(input);
            } else {
                super(input, normalizedBase);
            }
        } catch (error) {
            if (normalizedBase !== undefined) {
                throw error;
            }
            const state = parseInvalidAsciiIdna(input);
            if (state === undefined) {
                throw error;
            }
            const object = Object.create(new.target.prototype);
            fallbackStates.set(object, state);
            return object;
        }
    }

    static canParse(input, base = undefined) {
        try {
            new URL(input, base);
            return true;
        } catch {
            return false;
        }
    }

    static parse(input, base = undefined) {
        try {
            return new URL(input, base);
        } catch {
            return null;
        }
    }

    get href() {
        return fallbackValue(this, 'href', nativeDescriptors.href.get);
    }

    set href(value) {
        nativeDescriptors.href.set.call(this, toUSVString(value));
    }

    get origin() {
        return fallbackValue(this, 'origin', nativeDescriptors.origin.get);
    }

    get protocol() {
        return fallbackValue(this, 'protocol', nativeDescriptors.protocol.get);
    }

    get username() {
        return fallbackValue(this, 'username', nativeDescriptors.username.get);
    }

    get password() {
        return fallbackValue(this, 'password', nativeDescriptors.password.get);
    }

    get host() {
        return fallbackValue(this, 'host', nativeDescriptors.host.get);
    }

    get hostname() {
        return fallbackValue(this, 'hostname', nativeDescriptors.hostname.get);
    }

    get port() {
        return fallbackValue(this, 'port', nativeDescriptors.port.get);
    }

    get pathname() {
        return fallbackValue(this, 'pathname', nativeDescriptors.pathname.get);
    }

    get search() {
        return fallbackValue(this, 'search', nativeDescriptors.search.get);
    }

    get searchParams() {
        return fallbackValue(
            this, 'searchParams', nativeDescriptors.searchParams.get);
    }

    get hash() {
        return fallbackValue(this, 'hash', nativeDescriptors.hash.get);
    }

    toString() {
        return this.href;
    }

    toJSON() {
        return this.href;
    }
}

for (const name of [
    'protocol', 'username', 'password', 'host', 'hostname', 'port',
    'pathname', 'search', 'hash',
]) {
    const descriptor = Object.getOwnPropertyDescriptor(URL.prototype, name);
    Object.defineProperty(URL.prototype, name, {
        ...descriptor,
        set(value) {
            if (fallbackStates.has(this)) {
                throw new TypeError(
                    'Cannot mutate a URL with a pass-through IDNA label');
            }
            nativeDescriptors[name].set.call(this, toUSVString(value));
        },
    });
}

Object.defineProperties(URL, {
    createObjectURL: {
        configurable: true,
        writable: true,
        value: NativeURL.createObjectURL,
    },
    revokeObjectURL: {
        configurable: true,
        writable: true,
        value: NativeURL.revokeObjectURL,
    },
});
Object.defineProperty(globalThis, 'URL', {
    configurable: true,
    enumerable: false,
    writable: true,
    value: URL,
});
