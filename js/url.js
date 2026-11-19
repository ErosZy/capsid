const NativeURL = globalThis.URL;
const fallbackStates = new WeakMap();
let hasFallbackStates = false;

function toUSVStringSlow(string) {
  return string.replace(/[\uD800-\uDFFF]/g, (codeUnit, index) => {
    const code = codeUnit.charCodeAt(0);
    if (code <= 0xdbff) {
      const next = string.charCodeAt(index + 1);
      if (next >= 0xdc00 && next <= 0xdfff) {
        return codeUnit;
      }
    } else {
      const previous = string.charCodeAt(index - 1);
      if (previous >= 0xd800 && previous <= 0xdbff) {
        return codeUnit;
      }
    }
    return '\uFFFD';
  });
}

function toUSVString(value) {
  const string = typeof value === 'string' ? value : String(value);
  const length = string.length;
  for (let i = 0; i < length; i++) {
    const code = string.charCodeAt(i);
    if (code >= 0xd800 && code <= 0xdfff) {
      return toUSVStringSlow(string);
    }
  }
  return string;
}

function parseInvalidAsciiIdna(input) {
  const match = /^(https?|file):\/\/([^/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(input);
  if (!match || match[2].includes('@') || match[2].includes(':')) {
    return undefined;
  }
  const hostname = match[2].toLowerCase();
  const hasInvalidAsciiIdna = hostname
    .split('.')
    .some((label) => label === 'xn--' || label === 'xn--pokxncvks');
  if (!hasInvalidAsciiIdna) {
    return undefined;
  }

  const protocol = `${match[1].toLowerCase()}:`;
  const pathname = match[3] || '/';
  const search = match[4] || '';
  const hash = match[5] || '';
  const origin = protocol === 'file:' ? 'null' : `${protocol}//${hostname}`;
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

const nativeDescriptors = Object.getOwnPropertyDescriptors(NativeURL.prototype);
const nativeHrefGet = nativeDescriptors.href.get;
const nativeHrefSet = nativeDescriptors.href.set;
const nativeOriginGet = nativeDescriptors.origin.get;
const nativeProtocolGet = nativeDescriptors.protocol.get;
const nativeUsernameGet = nativeDescriptors.username.get;
const nativePasswordGet = nativeDescriptors.password.get;
const nativeHostGet = nativeDescriptors.host.get;
const nativeHostnameGet = nativeDescriptors.hostname.get;
const nativePortGet = nativeDescriptors.port.get;
const nativePathnameGet = nativeDescriptors.pathname.get;
const nativeSearchGet = nativeDescriptors.search.get;
const nativeSearchParamsGet = nativeDescriptors.searchParams.get;
const nativeHashGet = nativeDescriptors.hash.get;

function fallbackValue(object, name, nativeGetter) {
  const state = fallbackStates.get(object);
  return state === undefined ? nativeGetter.call(object) : state[name];
}

function installFallbackAccessors() {
  if (hasFallbackStates) {
    return;
  }
  hasFallbackStates = true;
  const getters = [
    ['href', nativeHrefGet],
    ['origin', nativeOriginGet],
    ['protocol', nativeProtocolGet],
    ['username', nativeUsernameGet],
    ['password', nativePasswordGet],
    ['host', nativeHostGet],
    ['hostname', nativeHostnameGet],
    ['port', nativePortGet],
    ['pathname', nativePathnameGet],
    ['search', nativeSearchGet],
    ['searchParams', nativeSearchParamsGet],
    ['hash', nativeHashGet],
  ];
  for (let i = 0; i < getters.length; i++) {
    const name = getters[i][0];
    const nativeGetter = getters[i][1];
    const current = Object.getOwnPropertyDescriptor(URL.prototype, name);
    Object.defineProperty(URL.prototype, name, {
      configurable: true,
      enumerable: current ? current.enumerable : true,
      get() {
        return fallbackValue(this, name, nativeGetter);
      },
      set: current ? current.set : undefined,
    });
  }
}

class URL extends NativeURL {
  constructor(input, base = undefined) {
    if (arguments.length === 0) {
      throw new TypeError("Failed to construct 'URL': 1 argument required");
    }
    input = toUSVString(input);
    const normalizedBase = base === undefined ? undefined : toUSVString(base);
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
      installFallbackAccessors();
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

  toString() {
    return this.href;
  }

  toJSON() {
    return this.href;
  }
}

Object.defineProperty(URL.prototype, 'href', {
  configurable: true,
  enumerable: true,
  get: nativeHrefGet,
  set(value) {
    nativeHrefSet.call(this, toUSVString(value));
  },
});

Object.defineProperty(URL.prototype, 'origin', {
  configurable: true,
  enumerable: true,
  get: nativeOriginGet,
});

Object.defineProperty(URL.prototype, 'searchParams', {
  configurable: true,
  enumerable: true,
  get: nativeSearchParamsGet,
});

for (const name of [
  'protocol',
  'username',
  'password',
  'host',
  'hostname',
  'port',
  'pathname',
  'search',
  'hash',
]) {
  const nativeGetter = nativeDescriptors[name].get;
  const nativeSetter = nativeDescriptors[name].set;
  Object.defineProperty(URL.prototype, name, {
    configurable: true,
    enumerable: true,
    get: nativeGetter,
    set(value) {
      if (hasFallbackStates && fallbackStates.has(this)) {
        throw new TypeError('Cannot mutate a URL with a pass-through IDNA label');
      }
      nativeSetter.call(this, toUSVString(value));
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
