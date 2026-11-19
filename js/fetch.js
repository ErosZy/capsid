import {
  setResponseWebSocket,
  getResponseWebSocket,
  isWebSocketEndpoint,
  captureNativeException,
  openFetchUpgrade,
  configureFetchChannelStreams,
  inheritFetchChannelStream,
  isFetchChannelStream,
  createOrdinaryFetchBody,
  isOrdinaryFetchBody,
  teeOrdinaryFetchBody,
} from './websocket.js';
import { configureAbortExceptions } from 'txiki-polyfills/abort-controller.js';
import { configureResponseBodyFactory } from 'txiki-polyfills/fetch/fetch.js';
import { configureOwnedHeadersCloner } from 'txiki-polyfills/fetch/headers.js';

configureAbortExceptions(captureNativeException);

const NativeHeaders = globalThis.Headers;
const NativeRequest = globalThis.Request;
const nativeFetch = globalThis.fetch;

const bodyTypeError = globalThis.TypeError;
const bodyRangeError = globalThis.RangeError;
const bodyProperty = Object.getOwnPropertyDescriptor;
const objectPrototype = Object.prototype;
const hasOwn = objectPrototype.hasOwnProperty;
const objectHasOwn = Object.hasOwn;
const getPrototypeOf = Object.getPrototypeOf;
const objectKeys = Object.keys;
const applyIntrinsic = Reflect.apply;
const headerBuiltinString = String;
const headerByteArray = Uint8Array;
const headerBigInt = BigInt;
const headerChar = String.fromCharCode;
const headerPrimitiveSymbol = Symbol.toPrimitive;
const stringPrototype = String.prototype;
const strToLowerCase = stringPrototype.toLowerCase;
const strToUpperCase = stringPrototype.toUpperCase;
const strCharCodeAt = stringPrototype.charCodeAt;
const strSlice = stringPrototype.slice;
const u8Subarray = Uint8Array.prototype.subarray;
const u8Slice = Uint8Array.prototype.slice;
const abSlice = ArrayBuffer.prototype.slice;
const arraySlice = Array.prototype.slice;
const arrayPush = Array.prototype.push;
const weakMapPrototype = WeakMap.prototype;
const weakMapGet = weakMapPrototype.get;
const weakMapSet = weakMapPrototype.set;
const weakSetHas = WeakSet.prototype.has;
const weakSetAdd = WeakSet.prototype.add;
const mapGet = Map.prototype.get;
const mapSet = Map.prototype.set;
const mapHas = Map.prototype.has;
const mapDelete = Map.prototype.delete;

const sharedTextEncoder = new TextEncoder();
const sharedTextDecoder = new TextDecoder();
const headerTextDecoder = new TextDecoder('utf-8', { ignoreBOM: true });
const textDecoderDecode = TextDecoder.prototype.decode;
const textEncoderEncode = TextEncoder.prototype.encode;

const emptyBodyIntrinsics = {
  bufferLength: Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get,
  typedLength: Object.getOwnPropertyDescriptor(
    Object.getPrototypeOf(Uint8Array.prototype),
    'byteLength',
  ).get,
  viewLength: Object.getOwnPropertyDescriptor(DataView.prototype, 'byteLength').get,
  viewBuffer: Object.getOwnPropertyDescriptor(DataView.prototype, 'buffer').get,
  blobSize: Object.getOwnPropertyDescriptor(Blob.prototype, 'size').get,
  paramsString: URLSearchParams.prototype.toString,
  isView: ArrayBuffer.isView,
};

let maxFetchRequestBodyBytes = 0;
let maxFetchResponseBodyBytes = 0;

export function configureFetchLimits(requestBytes, responseBytes) {
  const requestLimit = Number(requestBytes);
  const responseLimit = Number(responseBytes);

  if (
    !Number.isSafeInteger(requestLimit) ||
    requestLimit < 0 ||
    !Number.isSafeInteger(responseLimit) ||
    responseLimit < 0
  ) {
    throw new TypeError('invalid native Fetch body limits');
  }
  maxFetchRequestBodyBytes = requestLimit;
  maxFetchResponseBodyBytes = responseLimit;
  configureFetchChannelStreams(globalThis.ReadableStream);
  configureResponseBodyFactory((client) =>
    createOrdinaryFetchBody(client, maxFetchResponseBodyBytes),
  );
}

// 128-byte ASCII classification tables for RFC/workerd header name and value rules.
// HEADER_NAME_TABLE: 0 = invalid, 1 = valid lowercase/token char, 2 = valid ASCII A-Z.
const HEADER_NAME_TABLE = new Uint8Array(128);
{
  const separators = '()<>@,;:\\"/[]?={}';
  for (let code = 33; code < 127; code++) {
    if (separators.indexOf(headerChar(code)) === -1) {
      HEADER_NAME_TABLE[code] = code >= 65 && code <= 90 ? 2 : 1;
    }
  }
}

// HEADER_VALUE_TABLE bitmask: bit 1 = trimmable whitespace (\t, \n, \r, space),
// bit 2 = invalid interior character (\0, \n, \r).
const HEADER_VALUE_TABLE = new Uint8Array(128);
HEADER_VALUE_TABLE[0] = 2;
HEADER_VALUE_TABLE[9] = 1;
HEADER_VALUE_TABLE[10] = 3;
HEADER_VALUE_TABLE[13] = 3;
HEADER_VALUE_TABLE[32] = 1;

const HEADER_MAP_THRESHOLD = 32; // 16 pairs in flat array before promoting to Map
const headerStates = new WeakMap();
const iteratorStates = new WeakMap();
const ordinaryFetchStreams = new WeakSet();
const readableLocked = Object.getOwnPropertyDescriptor(ReadableStream.prototype, 'locked').get;
const headerInvocationMessage =
  'Illegal invocation: function called with incorrect `this` reference. See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details.';
const isObjectValue = (value) =>
  value !== null && (typeof value === 'object' || typeof value === 'function');
const arrayIteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));

// kj::String conversion preserves unpaired UTF-16 code units as WTF-8.
// The reference decodes their invalid byte sequence at the JS return boundary.
function headerEncode(value) {
  const len = value.length;
  const bytes = new headerByteArray(len * 3);
  let at = 0;
  for (let index = 0; index < len; ++index) {
    let code = strCharCodeAt.call(value, index);
    if (code >= 0xd800 && code <= 0xdbff && index + 1 < len) {
      const low = strCharCodeAt.call(value, index + 1);
      if (low >= 0xdc00 && low <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
        ++index;
      }
    }
    if (code < 0x80) bytes[at++] = code;
    else if (code < 0x800) {
      bytes[at++] = 0xc0 | (code >> 6);
      bytes[at++] = 0x80 | (code & 63);
    } else if (code < 0x10000) {
      bytes[at++] = 0xe0 | (code >> 12);
      bytes[at++] = 0x80 | ((code >> 6) & 63);
      bytes[at++] = 0x80 | (code & 63);
    } else {
      bytes[at++] = 0xf0 | (code >> 18);
      bytes[at++] = 0x80 | ((code >> 12) & 63);
      bytes[at++] = 0x80 | ((code >> 6) & 63);
      bytes[at++] = 0x80 | (code & 63);
    }
  }
  return u8Subarray.call(bytes, 0, at);
}

// Keep uncommon names as canonical bytes. The pinned 64-bit SWAR conversion
// also has observable non-ASCII results; decoding only at display preserves
// distinct raw keys that happen to render to the same replacement string.
function lowerHeaderKeyNonAscii(name) {
  const bytes = headerEncode(name);
  for (let offset = 0; offset < bytes.length; offset += 8) {
    const count = Math.min(8, bytes.length - offset);
    let word = 0n;
    for (let index = 0; index < count; ++index) {
      word |= headerBigInt(bytes[offset + index]) << headerBigInt(index * 8);
    }
    word ^=
      (((word + 0x3f3f3f3f3f3f3f3fn) ^ (word + 0x2525252525252525n)) & 0x8080808080808080n) >> 2n;
    for (let index = 0; index < count; ++index) {
      bytes[offset + index] = Number((word >> headerBigInt(index * 8)) & 255n);
    }
  }
  let raw = '';
  for (let i = 0; i < bytes.length; i++) raw += headerChar(bytes[i]);
  return raw;
}

function displayHeaderKey(raw) {
  const len = raw.length;
  for (let i = 0; i < len; i++) {
    if (strCharCodeAt.call(raw, i) >= 128) {
      const bytes = new headerByteArray(len);
      for (let index = 0; index < len; ++index) {
        bytes[index] = strCharCodeAt.call(raw, index);
      }
      return textDecoderDecode.call(headerTextDecoder, bytes);
    }
  }
  return raw;
}

function headerMutationString(value) {
  if (typeof value === 'string') return value;
  if (isObjectValue(value)) {
    const exotic = value[headerPrimitiveSymbol];
    if (exotic !== undefined && exotic !== null) {
      if (typeof exotic !== 'function') {
        const label = isObjectValue(exotic) ? '#<Object>' : headerBuiltinString(exotic);
        throw new bodyTypeError(
          `'${label}' returned for property 'Symbol(Symbol.toPrimitive)' of object '#<Object>' is not a function`,
        );
      }
      value = applyIntrinsic(exotic, value, ['string']);
      if (isObjectValue(value)) throw new bodyTypeError('Cannot convert object to primitive value');
    } else {
      let primitive = false;
      for (const name of ['toString', 'valueOf']) {
        const method = value[name];
        if (typeof method !== 'function') continue;
        const result = applyIntrinsic(method, value, []);
        if (!isObjectValue(result)) {
          value = result;
          primitive = true;
          break;
        }
      }
      if (!primitive) throw new bodyTypeError('Cannot convert object to primitive value');
    }
  }
  if (typeof value === 'symbol') {
    throw new bodyTypeError('Cannot convert a Symbol value to a string');
  }
  return headerBuiltinString(value);
}

function getHeaderState(obj) {
  return weakMapPrototype.get === weakMapGet
    ? headerStates.get(obj)
    : weakMapGet.call(headerStates, obj);
}

function setHeaderState(obj, state) {
  if (weakMapPrototype.set === weakMapSet) {
    headerStates.set(obj, state);
  } else {
    weakMapSet.call(headerStates, obj, state);
  }
}

function normalizeNameString(name) {
  const len = name.length;
  const direct = stringPrototype.charCodeAt === strCharCodeAt;
  let hasUpper = false;
  for (let i = 0; i < len; i++) {
    const c = direct ? name.charCodeAt(i) : strCharCodeAt.call(name, i);
    if (c < 128) {
      const cls = HEADER_NAME_TABLE[c];
      if (cls === 0) throw new bodyTypeError('Invalid header name.');
      if (cls === 2) hasUpper = true;
    } else {
      for (let j = i + 1; j < len; j++) {
        const c2 = direct ? name.charCodeAt(j) : strCharCodeAt.call(name, j);
        if (c2 < 128 && HEADER_NAME_TABLE[c2] === 0) {
          throw new bodyTypeError('Invalid header name.');
        }
      }
      return lowerHeaderKeyNonAscii(name);
    }
  }
  if (!hasUpper) return name;
  return stringPrototype.toLowerCase === strToLowerCase
    ? name.toLowerCase()
    : strToLowerCase.call(name);
}

function normalizeName(value) {
  return normalizeNameString(typeof value === 'string' ? value : headerMutationString(value));
}

function normalizeValueString(string) {
  const len = string.length;
  const direct = stringPrototype.charCodeAt === strCharCodeAt;
  let start = 0;
  while (start < len) {
    const c = direct ? string.charCodeAt(start) : strCharCodeAt.call(string, start);
    if (c > 32 || (HEADER_VALUE_TABLE[c] & 1) === 0) break;
    start++;
  }
  let end = len;
  while (end > start) {
    const c = direct ? string.charCodeAt(end - 1) : strCharCodeAt.call(string, end - 1);
    if (c > 32 || (HEADER_VALUE_TABLE[c] & 1) === 0) break;
    end--;
  }
  let hasNonAscii = false;
  for (let i = start; i < end; i++) {
    const c = direct ? string.charCodeAt(i) : strCharCodeAt.call(string, i);
    if (c > 13 && c < 128) continue;
    if (c >= 128) {
      hasNonAscii = true;
    } else if (c === 0 || c === 10 || c === 13) {
      throw new bodyTypeError('Invalid header value.');
    }
  }
  const trimmed = start === 0 && end === len ? string : strSlice.call(string, start, end);
  return hasNonAscii ? textDecoderDecode.call(headerTextDecoder, headerEncode(trimmed)) : trimmed;
}

function normalizeValue(value) {
  return normalizeValueString(typeof value === 'string' ? value : headerMutationString(value));
}

function createHeaderState() {
  return {
    k0: null,
    v0: null,
    pairs: null,
    map: null,
    cookies: null,
    immutable: false,
  };
}

function createHeaderStateWithOne(k0, v0) {
  return {
    k0,
    v0,
    pairs: null,
    map: null,
    cookies: k0 === 'set-cookie' ? [v0] : null,
    immutable: false,
  };
}

function cloneHeaderState(src) {
  return {
    k0: src.k0,
    v0: src.v0,
    pairs: src.pairs === null ? null : arraySlice.call(src.pairs),
    map: src.map === null ? null : new Map(src.map),
    cookies: src.cookies === null ? null : arraySlice.call(src.cookies),
    immutable: false,
  };
}

function headerStateGet(state, name) {
  if (state.k0 !== null) {
    return state.k0 === name ? state.v0 : null;
  }
  const p = state.pairs;
  if (p !== null) {
    for (let i = 0, n = p.length; i < n; i += 2) {
      if (p[i] === name) return p[i + 1];
    }
    return null;
  }
  const m = state.map;
  if (m !== null) {
    const v = mapGet.call(m, name);
    return v === undefined ? null : v;
  }
  return null;
}

function headerStateHas(state, name) {
  if (state.k0 !== null) {
    return state.k0 === name;
  }
  const p = state.pairs;
  if (p !== null) {
    for (let i = 0, n = p.length; i < n; i += 2) {
      if (p[i] === name) return true;
    }
    return false;
  }
  const m = state.map;
  if (m !== null) {
    return mapHas.call(m, name);
  }
  return false;
}

function headerStateAppendNormalized(state, name, value) {
  if (name === 'set-cookie') {
    if (state.cookies === null) state.cookies = [value];
    else arrayPush.call(state.cookies, value);
  }
  const m = state.map;
  if (m !== null) {
    const existing = mapGet.call(m, name);
    mapSet.call(m, name, existing === undefined ? value : `${existing}, ${value}`);
    return;
  }
  const p = state.pairs;
  if (p !== null) {
    const n = p.length;
    for (let i = 0; i < n; i += 2) {
      if (p[i] === name) {
        p[i + 1] = `${p[i + 1]}, ${value}`;
        return;
      }
    }
    if (n < HEADER_MAP_THRESHOLD) {
      p[n] = name;
      p[n + 1] = value;
      return;
    }
    const nextMap = new Map();
    for (let i = 0; i < n; i += 2) {
      mapSet.call(nextMap, p[i], p[i + 1]);
    }
    mapSet.call(nextMap, name, value);
    state.pairs = null;
    state.map = nextMap;
    return;
  }
  if (state.k0 !== null) {
    if (state.k0 === name) {
      state.v0 = `${state.v0}, ${value}`;
      return;
    }
    state.pairs = [state.k0, state.v0, name, value];
    state.k0 = null;
    state.v0 = null;
    return;
  }
  state.k0 = name;
  state.v0 = value;
}

function headerStateSetNormalized(state, name, value) {
  if (name === 'set-cookie') {
    state.cookies = [value];
  }
  const m = state.map;
  if (m !== null) {
    mapSet.call(m, name, value);
    return;
  }
  const p = state.pairs;
  if (p !== null) {
    const n = p.length;
    for (let i = 0; i < n; i += 2) {
      if (p[i] === name) {
        p[i + 1] = value;
        return;
      }
    }
    if (n < HEADER_MAP_THRESHOLD) {
      p[n] = name;
      p[n + 1] = value;
      return;
    }
    const nextMap = new Map();
    for (let i = 0; i < n; i += 2) {
      mapSet.call(nextMap, p[i], p[i + 1]);
    }
    mapSet.call(nextMap, name, value);
    state.pairs = null;
    state.map = nextMap;
    return;
  }
  if (state.k0 !== null) {
    if (state.k0 === name) {
      state.v0 = value;
      return;
    }
    state.pairs = [state.k0, state.v0, name, value];
    state.k0 = null;
    state.v0 = null;
    return;
  }
  state.k0 = name;
  state.v0 = value;
}

function headerStateDeleteNormalized(state, name) {
  if (name === 'set-cookie') {
    state.cookies = null;
  }
  const m = state.map;
  if (m !== null) {
    mapDelete.call(m, name);
    return;
  }
  const p = state.pairs;
  if (p !== null) {
    const n = p.length;
    for (let i = 0; i < n; i += 2) {
      if (p[i] === name) {
        p.splice(i, 2);
        if (p.length === 2) {
          state.k0 = p[0];
          state.v0 = p[1];
          state.pairs = null;
        } else if (p.length === 0) {
          state.pairs = null;
        }
        return;
      }
    }
    return;
  }
  if (state.k0 === name) {
    state.k0 = null;
    state.v0 = null;
  }
}

function requireHeaderState(value) {
  const state = getHeaderState(value);
  if (state === undefined) throw new bodyTypeError(headerInvocationMessage);
  return state;
}

function sortedEntriesFromState(state) {
  if (state.k0 !== null) {
    if (state.k0 === 'set-cookie') {
      const entries = [];
      const cookies = state.cookies;
      for (let i = 0; i < cookies.length; i++) {
        entries.push(['set-cookie', cookies[i]]);
      }
      return entries;
    }
    return [[displayHeaderKey(state.k0), state.v0]];
  }
  const p = state.pairs;
  if (p !== null) {
    const count = p.length >> 1;
    const keys = new Array(count);
    for (let i = 0; i < count; i++) keys[i] = p[i << 1];
    keys.sort();
    const entries = [];
    for (let i = 0; i < count; i++) {
      const name = keys[i];
      if (name === 'set-cookie') {
        const cookies = state.cookies;
        for (let j = 0; j < cookies.length; j++) entries.push(['set-cookie', cookies[j]]);
      } else {
        for (let j = 0; j < p.length; j += 2) {
          if (p[j] === name) {
            entries.push([displayHeaderKey(name), p[j + 1]]);
            break;
          }
        }
      }
    }
    return entries;
  }
  const m = state.map;
  if (m !== null) {
    const entries = [];
    for (const name of Array.from(m.keys()).sort()) {
      if (name === 'set-cookie') {
        const cookies = state.cookies;
        for (let j = 0; j < cookies.length; j++) entries.push(['set-cookie', cookies[j]]);
      } else {
        entries.push([displayHeaderKey(name), mapGet.call(m, name)]);
      }
    }
    return entries;
  }
  return [];
}

function sortedEntries(headers) {
  return sortedEntriesFromState(requireHeaderState(headers));
}

const headersIteratorPrototypes = Object.create(null);
for (const [kind, label] of [
  ['entry', 'EntryIterator'],
  ['key', 'KeyIterator'],
  ['value', 'ValueIterator'],
]) {
  const prototype = Object.create(arrayIteratorPrototype);
  Object.defineProperties(prototype, {
    next: {
      configurable: true,
      enumerable: true,
      writable: true,
      value: function next() {
        const state = weakMapGet.call(iteratorStates, this);
        if (!state || state.kind !== kind) throw new bodyTypeError(headerInvocationMessage);
        if (state.index >= state.entries.length) return { done: true, value: undefined };
        const [name, value] = state.entries[state.index++];
        return {
          done: false,
          value: kind === 'key' ? name : kind === 'value' ? value : [name, value],
        };
      },
    },
    [Symbol.iterator]: {
      configurable: true,
      writable: true,
      // The pinned iterator method is anonymous and checks its kind.
      value: function () {
        const state = weakMapGet.call(iteratorStates, this);
        if (!state || state.kind !== kind) throw new bodyTypeError(headerInvocationMessage);
        return this;
      },
    },
    [Symbol.toStringTag]: { configurable: true, value: label },
  });
  Object.defineProperty(prototype[Symbol.iterator], 'name', { value: '' });
  headersIteratorPrototypes[kind] = prototype;
}

function createHeadersIterator(headers, kind) {
  const iterator = Object.create(headersIteratorPrototypes[kind]);
  weakMapSet.call(iteratorStates, iterator, { entries: sortedEntries(headers), kind, index: 0 });
  return iterator;
}

// Cache the supplied iterator method and let for-of cache next and perform
// IteratorClose if converting an element throws. Conversion happens while
// consuming the sequence, before Headers validates pair lengths/names/values.
function convertHeaderSequence(value, method, convert) {
  const converted = [];
  const iterable = {
    [Symbol.iterator]() {
      return applyIntrinsic(method, value, []);
    },
  };
  for (const item of iterable) converted.push(convert(item));
  return converted;
}

function initHeadersIntoState(state, init) {
  if (init === undefined) return;
  if (!isObjectValue(init)) {
    throw new bodyTypeError(
      "Failed to construct 'Headers': constructor parameter 1 is not of type 'Headers or Sequence or object'.",
    );
  }
  const proto = getPrototypeOf(init);
  if (proto === headersPrototype) {
    const owned = getHeaderState(init);
    if (owned !== undefined) {
      state.k0 = owned.k0;
      state.v0 = owned.v0;
      state.pairs = owned.pairs === null ? null : arraySlice.call(owned.pairs);
      state.map = owned.map === null ? null : new Map(owned.map);
      state.cookies = owned.cookies === null ? null : arraySlice.call(owned.cookies);
      return;
    }
  } else if (proto !== objectPrototype) {
    const owned = getHeaderState(init);
    if (owned !== undefined) {
      state.k0 = owned.k0;
      state.v0 = owned.v0;
      state.pairs = owned.pairs === null ? null : arraySlice.call(owned.pairs);
      state.map = owned.map === null ? null : new Map(owned.map);
      state.cookies = owned.cookies === null ? null : arraySlice.call(owned.cookies);
      return;
    }
    if (
      (init instanceof NativeHeaders || init instanceof Headers) &&
      !objectHasOwn(init, Symbol.iterator) &&
      typeof init.getSetCookie === 'function'
    ) {
      for (const [name, value] of init) {
        if (name !== 'set-cookie') {
          const nStr = headerMutationString(name);
          const vStr = headerMutationString(value);
          const normVal = normalizeValueString(vStr);
          const normName = normalizeNameString(nStr);
          headerStateAppendNormalized(state, normName, normVal);
        }
      }
      for (const value of init.getSetCookie()) {
        const vStr = headerMutationString(value);
        const normVal = normalizeValueString(vStr);
        headerStateAppendNormalized(state, 'set-cookie', normVal);
      }
      return;
    }
  }

  const method = init[Symbol.iterator];
  if (typeof method === 'function') {
    const entries = convertHeaderSequence(init, method, (pair) => {
      const inner = isObjectValue(pair) ? pair[Symbol.iterator] : undefined;
      if (typeof inner !== 'function') {
        throw new bodyTypeError("Incorrect type: the provided value is not of type 'Sequence'.");
      }
      return convertHeaderSequence(pair, inner, headerMutationString);
    });
    for (let i = 0; i < entries.length; i++) {
      const pair = entries[i];
      if (pair.length !== 2) {
        throw new bodyTypeError(
          'To initialize a Headers object from a sequence, each inner sequence must have exactly two elements.',
        );
      }
      const normVal = normalizeValueString(pair[1]);
      const normName = normalizeNameString(pair[0]);
      headerStateAppendNormalized(state, normName, normVal);
    }
    return;
  }

  // Record path: read and coerce all values before validation to preserve
  // Web IDL getter/proxy/error ordering, while avoiding intermediate pair arrays.
  const keys = objectKeys(init);
  const count = keys.length;
  if (count === 0) return;
  if (count === 1) {
    const k0 = keys[0];
    const raw0 = init[k0];
    const v0 = typeof raw0 === 'string' ? raw0 : headerMutationString(raw0);
    const normVal = normalizeValueString(v0);
    const normName = normalizeNameString(k0);
    headerStateAppendNormalized(state, normName, normVal);
    return;
  }
  const values = new Array(count);
  for (let i = 0; i < count; i++) {
    const raw = init[keys[i]];
    values[i] = typeof raw === 'string' ? raw : headerMutationString(raw);
  }
  for (let i = 0; i < count; i++) {
    const normVal = normalizeValueString(values[i]);
    const normName = normalizeNameString(keys[i]);
    headerStateAppendNormalized(state, normName, normVal);
  }
}

class Headers {
  constructor(init = undefined) {
    const state = createHeaderState();
    setHeaderState(this, state);
    if (init !== undefined) {
      initHeadersIntoState(state, init);
    }
  }

  append(name, value) {
    const state = requireHeaderState(this);
    if (arguments.length < 1) {
      throw new bodyTypeError(
        "Failed to execute 'append' on 'Headers': parameter 1 is not of type 'string'.",
      );
    }
    const nStr = typeof name === 'string' ? name : headerMutationString(name);
    if (arguments.length < 2) {
      throw new bodyTypeError(
        "Failed to execute 'append' on 'Headers': parameter 2 is not of type 'string'.",
      );
    }
    const vStr = typeof value === 'string' ? value : headerMutationString(value);
    if (state.immutable) throw new bodyTypeError("Can't modify immutable headers.");
    const normVal = normalizeValueString(vStr);
    const normName = normalizeNameString(nStr);
    headerStateAppendNormalized(state, normName, normVal);
  }

  delete(name) {
    const state = requireHeaderState(this);
    if (arguments.length < 1) {
      throw new bodyTypeError(
        "Failed to execute 'delete' on 'Headers': parameter 1 is not of type 'string'.",
      );
    }
    const nStr = typeof name === 'string' ? name : headerMutationString(name);
    if (state.immutable) throw new bodyTypeError("Can't modify immutable headers.");
    headerStateDeleteNormalized(state, normalizeNameString(nStr));
  }

  get(name) {
    const state = requireHeaderState(this);
    if (arguments.length < 1) {
      throw new bodyTypeError(
        "Failed to execute 'get' on 'Headers': parameter 1 is not of type 'string'.",
      );
    }
    return headerStateGet(
      state,
      normalizeNameString(typeof name === 'string' ? name : headerMutationString(name)),
    );
  }

  getSetCookie() {
    const state = requireHeaderState(this);
    return state.cookies === null ? [] : arraySlice.call(state.cookies);
  }

  has(name) {
    const state = requireHeaderState(this);
    if (arguments.length < 1) {
      throw new bodyTypeError(
        "Failed to execute 'has' on 'Headers': parameter 1 is not of type 'string'.",
      );
    }
    return headerStateHas(
      state,
      normalizeNameString(typeof name === 'string' ? name : headerMutationString(name)),
    );
  }

  set(name, value) {
    const state = requireHeaderState(this);
    if (arguments.length < 1) {
      throw new bodyTypeError(
        "Failed to execute 'set' on 'Headers': parameter 1 is not of type 'string'.",
      );
    }
    const nStr = typeof name === 'string' ? name : headerMutationString(name);
    if (arguments.length < 2) {
      throw new bodyTypeError(
        "Failed to execute 'set' on 'Headers': parameter 2 is not of type 'string'.",
      );
    }
    const vStr = typeof value === 'string' ? value : headerMutationString(value);
    if (state.immutable) throw new bodyTypeError("Can't modify immutable headers.");
    const normVal = normalizeValueString(vStr);
    const normName = normalizeNameString(nStr);
    headerStateSetNormalized(state, normName, normVal);
  }

  forEach(callback, thisArg = undefined) {
    const state = requireHeaderState(this);
    if (typeof callback !== 'function') {
      throw new bodyTypeError(
        "Failed to execute 'forEach' on 'Headers': parameter 1 is not of type 'function'.",
      );
    }
    // Pinned workerd uses a complete entry snapshot here, and invokes the
    // callback directly without reading this.entries or callback.call.
    const entries = sortedEntriesFromState(state);
    for (let i = 0; i < entries.length; i++) {
      const entry = entries[i];
      applyIntrinsic(callback, thisArg, [entry[1], entry[0], this]);
    }
  }

  keys() {
    return createHeadersIterator(this, 'key');
  }

  values() {
    return createHeadersIterator(this, 'value');
  }

  entries() {
    return createHeadersIterator(this, 'entry');
  }

  [Symbol.iterator]() {
    return this.entries();
  }

  get [Symbol.toStringTag]() {
    return 'Headers';
  }
}

const headersPrototype = Headers.prototype;

function createEmptyHeaders() {
  const headers = Object.create(headersPrototype);
  setHeaderState(headers, createHeaderState());
  return headers;
}

function createHeadersFromIncomingPairs(rawPairs) {
  const headers = Object.create(headersPrototype);
  const state = createHeaderState();
  setHeaderState(headers, state);
  if (rawPairs !== undefined && rawPairs !== null) {
    for (let i = 0; i < rawPairs.length; i++) {
      const pair = rawPairs[i];
      const normVal = normalizeValueString(
        typeof pair[1] === 'string' ? pair[1] : headerMutationString(pair[1]),
      );
      const normName = normalizeNameString(
        typeof pair[0] === 'string' ? pair[0] : headerMutationString(pair[0]),
      );
      headerStateAppendNormalized(state, normName, normVal);
    }
  }
  return headers;
}

function cloneHeadersFromOwnedState(srcState) {
  const headers = Object.create(headersPrototype);
  setHeaderState(headers, cloneHeaderState(srcState));
  return headers;
}

function wrapNormalizedNativeHeaders(nativeHeaders) {
  const owned = getHeaderState(nativeHeaders);
  if (owned !== undefined) {
    return cloneHeadersFromOwnedState(owned);
  }
  const headers = Object.create(headersPrototype);
  const state = createHeaderState();
  setHeaderState(headers, state);
  for (const [name, value] of nativeHeaders) {
    if (name !== 'set-cookie') {
      headerStateSetNormalized(state, name, value);
    }
  }
  for (const value of nativeHeaders.getSetCookie()) {
    headerStateAppendNormalized(state, 'set-cookie', value);
  }
  return headers;
}

function finishFetchedResponse(response, url, redirected) {
  response.url = url;
  response.redirected = Boolean(redirected);
  const state =
    response._headersState !== null ? response._headersState : getHeaderState(response.headers);
  if (state !== undefined && state !== null) {
    state.immutable = true;
  }
  if (response.body !== null) weakSetAdd.call(ordinaryFetchStreams, response.body);
  return response;
}

const streamTee = ReadableStream.prototype.tee;
Object.defineProperty(ReadableStream.prototype, 'tee', {
  ...Object.getOwnPropertyDescriptor(ReadableStream.prototype, 'tee'),
  value: function tee() {
    if (
      (weakSetHas.call(ordinaryFetchStreams, this) || isOrdinaryFetchBody(this)) &&
      applyIntrinsic(readableLocked, this, [])
    ) {
      throw new bodyTypeError('This ReadableStream is currently locked to a reader,');
    }
    const nativeBranches = teeOrdinaryFetchBody(this);
    const branches = nativeBranches ?? applyIntrinsic(streamTee, this, []);
    if (!nativeBranches) for (const branch of branches) inheritFetchChannelStream(this, branch);
    if (weakSetHas.call(ordinaryFetchStreams, this)) this._disturbed = true;
    return branches;
  },
});

function normalizeRequestInfo(input) {
  // Preserve Request and NativeRequest brands. Other RequestInfo values use
  // one Web IDL string conversion, before RequestInit getters are observed.
  return input instanceof Request || input instanceof NativeRequest ? input : `${input}`;
}

function normalizeBody(body) {
  if (
    body === undefined ||
    body === null ||
    typeof body === 'string' ||
    body instanceof ReadableStream ||
    body instanceof Blob ||
    body instanceof FormData ||
    body instanceof URLSearchParams ||
    body instanceof ArrayBuffer ||
    ArrayBuffer.isView(body)
  ) {
    return body;
  }

  return String(body);
}

function isEmptyFixedResponseBody(body) {
  if (typeof body === 'string') return body.length === 0;
  if (body instanceof ArrayBuffer) return emptyBodyIntrinsics.bufferLength.call(body) === 0;
  if (emptyBodyIntrinsics.isView(body)) {
    try {
      return emptyBodyIntrinsics.typedLength.call(body) === 0;
    } catch (error) {
      if (!(error instanceof bodyTypeError)) throw error;
      // A DataView's JS byteLength getter throws after detachment, while
      // the reference's native BufferSource conversion sees zero bytes.
      emptyBodyIntrinsics.viewBuffer.call(body); // native brand check
      try {
        return emptyBodyIntrinsics.viewLength.call(body) === 0;
      } catch (viewError) {
        if (viewError instanceof bodyTypeError) return true;
        throw viewError;
      }
    }
  }
  if (body instanceof Blob) return emptyBodyIntrinsics.blobSize.call(body) === 0;
  if (body instanceof URLSearchParams) return emptyBodyIntrinsics.paramsString.call(body) === '';
  return false;
}

// Bridge-only header extraction: one pass over the internal representation, no
// sort, no iterator objects, no forEach callback, no getSetCookie slice.
// Order is insertion order with set-cookie emitted last — the same observable
// sequence frozen by test_host_single_worker.mjs.
let bridgeLastHasContentLength = false;

function bridgePairsFromState(state) {
  if (state.k0 !== null) {
    const k0 = state.k0;
    if (k0 === 'content-length') {
      bridgeLastHasContentLength = true;
    }
    if (k0 !== 'set-cookie') {
      return [[k0, state.v0]];
    }
    const cookies = state.cookies;
    const pairs = new Array(cookies.length);
    for (let i = 0; i < cookies.length; i++) {
      pairs[i] = ['set-cookie', cookies[i]];
    }
    return pairs;
  }
  const p = state.pairs;
  if (p !== null) {
    const pairs = [];
    for (let i = 0, n = p.length; i < n; i += 2) {
      const name = p[i];
      if (name === 'content-length') {
        bridgeLastHasContentLength = true;
      }
      if (name !== 'set-cookie') {
        pairs.push([name, p[i + 1]]);
      }
    }
    const cookies = state.cookies;
    if (cookies !== null) {
      for (let i = 0; i < cookies.length; i++) {
        pairs.push(['set-cookie', cookies[i]]);
      }
    }
    return pairs;
  }
  const m = state.map;
  if (m !== null) {
    const pairs = [];
    for (const name of m.keys()) {
      if (name === 'content-length') {
        bridgeLastHasContentLength = true;
      }
      if (name !== 'set-cookie') {
        pairs.push([name, mapGet.call(m, name)]);
      }
    }
    const cookies = state.cookies;
    if (cookies !== null) {
      for (let i = 0; i < cookies.length; i++) {
        pairs.push(['set-cookie', cookies[i]]);
      }
    }
    return pairs;
  }
  return [];
}

export function bridgeHeaderPairs(headers) {
  bridgeLastHasContentLength = false;
  const state = getHeaderState(headers);
  if (state !== undefined) {
    return bridgePairsFromState(state);
  }
  const pairs = [];
  headers.forEach((value, name) => {
    if (name === 'content-length') {
      bridgeLastHasContentLength = true;
    }
    if (name !== 'set-cookie') {
      pairs.push([name, value]);
    }
  });
  for (const value of headers.getSetCookie()) {
    pairs.push(['set-cookie', value]);
  }
  return pairs;
}

export function bridgeResponseHeaders(response) {
  bridgeLastHasContentLength = false;
  const state = response._headersState;
  if (state !== null && !objectHasOwn(response, 'headers')) {
    return bridgePairsFromState(state);
  }
  return bridgeHeaderPairs(response.headers);
}

export function bridgeResponseHadContentLength() {
  return bridgeLastHasContentLength;
}

export function hasHeader(headers, normalizedName) {
  const state = getHeaderState(headers);
  if (state === undefined) {
    return headers.has(normalizedName);
  }
  return headerStateHas(state, normalizedName);
}

export function getFastResponseBody(response) {
  if (
    response._bodyFastKind === 0 ||
    response._bodyExposed ||
    response._bodyConsumed ||
    response._bodyStream !== null ||
    objectHasOwn(response, 'body')
  ) {
    return null;
  }
  return response._bodyFastData;
}

export function getFastStreamResponseBody(response) {
  if (
    response._bodyFastKind !== 0 ||
    response._bodyStream === null ||
    objectHasOwn(response, 'body')
  ) {
    return null;
  }
  return response._bodyStream instanceof ReadableStream ? response._bodyStream : null;
}

export function isNullFastResponseBody(response) {
  return (
    response._bodyFastKind === 0 &&
    !response._bodyExposed &&
    response._bodyStream === null &&
    !objectHasOwn(response, 'body')
  );
}

function materializeFastBody(target, exposed) {
  if (exposed) {
    target._bodyExposed = true;
  }
  if (target._bodyStream === null && target._bodyFastKind !== 0) {
    const bytes =
      target._bodyFastKind === 1
        ? textEncoderEncode.call(sharedTextEncoder, target._bodyFastData)
        : target._bodyFastData;
    const stream = new ReadableStream({
      start(controller) {
        controller.enqueue(bytes);
        controller.close();
      },
    });
    target._noBody = false;
    target._bodySize = bytes.byteLength;
    target._bodyStream = stream;
    target._bodyFastKind = 0;
    target._bodyFastData = null;
  }
  return target._bodyStream;
}

function cloneBufferSource(body) {
  if (body instanceof Uint8Array) {
    return u8Slice.call(body);
  }
  if (body instanceof ArrayBuffer) {
    return new Uint8Array(abSlice.call(body, 0));
  }
  const view = new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
  return u8Slice.call(view);
}

function initBodyFields(target, body, hState) {
  target._bodyInit = body;
  if (body === null || body === undefined || body === '') {
    target._noBody = true;
    target._bodySize = 0;
    target._bodyStream = null;
    target._bodyFastKind = 0;
    target._bodyFastData = null;
    if (typeof body === 'string' && hState !== null && !headerStateHas(hState, 'content-type')) {
      headerStateSetNormalized(hState, 'content-type', 'text/plain;charset=UTF-8');
    }
    return;
  }
  if (typeof body === 'string') {
    target._noBody = false;
    target._bodySize = -1;
    target._bodyStream = null;
    target._bodyFastKind = 1;
    target._bodyFastData = body;
    if (hState !== null && !headerStateHas(hState, 'content-type')) {
      headerStateSetNormalized(hState, 'content-type', 'text/plain;charset=UTF-8');
    }
    return;
  }
  if (body instanceof ReadableStream) {
    target._noBody = false;
    target._bodySize = -1;
    target._bodyStream = body;
    target._bodyFastKind = 0;
    target._bodyFastData = null;
    return;
  }
  if (body instanceof Uint8Array || body instanceof ArrayBuffer || ArrayBuffer.isView(body)) {
    const bytes = cloneBufferSource(body);
    target._noBody = false;
    target._bodySize = bytes.byteLength;
    target._bodyStream = null;
    target._bodyFastKind = 2;
    target._bodyFastData = bytes;
    return;
  }
  if (body instanceof Blob) {
    target._noBody = false;
    target._bodySize = body.size;
    target._bodyStream = body.stream();
    target._bodyFastKind = 0;
    target._bodyFastData = null;
    if (body.type && hState !== null && !headerStateHas(hState, 'content-type')) {
      headerStateSetNormalized(hState, 'content-type', normalizeValueString(String(body.type)));
    }
    return;
  }
  if (body instanceof FormData) {
    const blob = body._blob();
    target._noBody = false;
    target._bodySize = blob.size;
    target._bodyStream = blob.stream();
    target._bodyFastKind = 0;
    target._bodyFastData = null;
    if (hState !== null && !headerStateHas(hState, 'content-type')) {
      headerStateSetNormalized(hState, 'content-type', normalizeValueString(String(blob.type)));
    }
    return;
  }
  if (body instanceof URLSearchParams) {
    const encoded = textEncoderEncode.call(sharedTextEncoder, body.toString());
    target._noBody = false;
    target._bodySize = encoded.byteLength;
    target._bodyStream = null;
    target._bodyFastKind = 2;
    target._bodyFastData = encoded;
    if (hState !== null && !headerStateHas(hState, 'content-type')) {
      headerStateSetNormalized(
        hState,
        'content-type',
        'application/x-www-form-urlencoded;charset=UTF-8',
      );
    }
    return;
  }
  const str = String(body);
  target._noBody = false;
  target._bodySize = -1;
  target._bodyStream = null;
  target._bodyFastKind = 1;
  target._bodyFastData = str;
}

function consumeBodyGuard(target) {
  if (target._noBody) {
    return null;
  }
  if (target.bodyUsed) {
    return Promise.reject(new TypeError('Already read'));
  }
  target._bodyConsumed = true;
  return null;
}

async function readAllChunks(stream) {
  const reader = stream.getReader();
  const chunks = [];
  let totalLength = 0;

  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    totalLength += value.byteLength;
  }

  if (chunks.length === 1) {
    const only = chunks[0];
    if (only.byteOffset === 0 && only.byteLength === only.buffer.byteLength) {
      return only.buffer;
    }
  }
  const result = new Uint8Array(totalLength);
  let offset = 0;
  for (let i = 0; i < chunks.length; i++) {
    result.set(chunks[i], offset);
    offset += chunks[i].byteLength;
  }
  return result.buffer;
}

function decodeUrlEncodedFormData(body) {
  const form = new FormData();
  const trimmed = body.trim();
  if (!trimmed) return form;
  const parts = trimmed.split('&');
  for (let i = 0; i < parts.length; i++) {
    const entry = parts[i];
    if (!entry) continue;
    const eq = entry.indexOf('=');
    const rawName = eq < 0 ? entry : entry.slice(0, eq);
    const rawValue = eq < 0 ? '' : entry.slice(eq + 1);
    form.append(
      decodeURIComponent(rawName.replace(/\+/g, ' ')),
      decodeURIComponent(rawValue.replace(/\+/g, ' ')),
    );
  }
  return form;
}

function consumeArrayBuffer(target) {
  const stream = materializeFastBody(target, true);
  const rejected = consumeBodyGuard(target);
  if (rejected !== null) return rejected;
  if (!stream) return Promise.resolve(new ArrayBuffer(0));
  return readAllChunks(stream);
}

function consumeBlob(target) {
  const stream = materializeFastBody(target, true);
  const rejected = consumeBodyGuard(target);
  if (rejected !== null) return rejected;
  const contentType = target.headers.get('content-type') ?? '';
  if (!stream) return Promise.resolve(new Blob([], { type: contentType }));
  return readAllChunks(stream).then((buffer) => new Blob([buffer], { type: contentType }));
}

function consumeText(target) {
  const stream = materializeFastBody(target, true);
  const rejected = consumeBodyGuard(target);
  if (rejected !== null) return rejected;
  if (!stream) return Promise.resolve('');
  return readAllChunks(stream).then((buffer) => textDecoderDecode.call(sharedTextDecoder, buffer));
}

function consumeJson(target) {
  return consumeText(target).then(JSON.parse);
}

function consumeFormData(target) {
  materializeFastBody(target, true);
  const contentType = target.headers.get('content-type') || '';
  if (/^multipart\/form-data/i.test(contentType.trim())) {
    return parseMultipartFormData(target);
  }
  if (!/^application\/x-www-form-urlencoded(?:\s*;|$)/i.test(contentType)) {
    return consumeText(target).then(() => {
      throw new TypeError(
        'Content-Type was not multipart/form-data or application/x-www-form-urlencoded',
      );
    });
  }
  return consumeText(target).then(decodeUrlEncodedFormData);
}

function toNativeHeaderEntries(init) {
  const headers = init instanceof Headers ? init : new Headers(init);
  const entries = sortedEntries(headers).filter(([name]) => name !== 'set-cookie');

  for (const value of headers.getSetCookie()) {
    entries.push(['set-cookie', value]);
  }
  return entries;
}

const redirectValueError =
  'Invalid redirect value, must be one of "follow" or "manual" ("error" won\'t be implemented since it does not make sense at the edge; use "manual" and check the response status code).';
function normalizeRedirect(value) {
  if (value === undefined) return undefined;
  if (typeof value === 'symbol') {
    throw new bodyTypeError('Cannot convert a Symbol value to a string');
  }
  const redirect = `${value}`;
  if (redirect !== 'follow' && redirect !== 'manual') throw new bodyTypeError(redirectValueError);
  return redirect;
}

function normalizeRequestInit(init) {
  if (init === undefined || init === null) {
    return {};
  }
  const normalized = {};
  for (const key of objectKeys(init)) {
    normalized[key] = init[key];
  }
  if (normalized.headers !== undefined) {
    normalized.headers = new Headers(normalized.headers);
  }
  if (hasOwn.call(normalized, 'body')) {
    normalized.body = normalizeBody(normalized.body);
  }
  if (normalized.redirect !== undefined) {
    normalized.redirect = normalizeRedirect(normalized.redirect);
  }
  return normalized;
}

function normalizeMethod(method) {
  if (
    method === 'GET' ||
    method === 'POST' ||
    method === 'HEAD' ||
    method === 'PUT' ||
    method === 'DELETE' ||
    method === 'OPTIONS' ||
    method === 'PATCH' ||
    method === 'CONNECT' ||
    method === 'TRACE'
  ) {
    return method;
  }
  const upcased = strToUpperCase.call(method);
  if (
    upcased === 'GET' ||
    upcased === 'POST' ||
    upcased === 'HEAD' ||
    upcased === 'PUT' ||
    upcased === 'DELETE' ||
    upcased === 'OPTIONS' ||
    upcased === 'PATCH' ||
    upcased === 'CONNECT' ||
    upcased === 'TRACE'
  ) {
    return upcased;
  }
  return method;
}

function isNullBodyStatus(status) {
  return status === 101 || status === 204 || status === 205 || status === 304;
}

function bodyLimitError(kind, limit) {
  return new TypeError(`fetch ${kind} body exceeded configured limit of ${limit} bytes`);
}

function bodyChunkSize(chunk) {
  if (typeof chunk === 'string') {
    return textEncoderEncode.call(sharedTextEncoder, chunk).byteLength;
  }
  if (chunk instanceof ArrayBuffer) {
    return chunk.byteLength;
  }
  if (ArrayBuffer.isView(chunk)) {
    return chunk.byteLength;
  }
  throw new TypeError('Fetch body stream chunk must be bytes');
}

function knownBodySize(body) {
  if (body === undefined || body === null) {
    return 0;
  }
  if (typeof body === 'string') {
    return textEncoderEncode.call(sharedTextEncoder, body).byteLength;
  }
  if (body instanceof Blob) {
    return body.size;
  }
  if (body instanceof URLSearchParams) {
    return textEncoderEncode.call(sharedTextEncoder, String(body)).byteLength;
  }
  if (body instanceof ArrayBuffer) {
    return body.byteLength;
  }
  if (ArrayBuffer.isView(body)) {
    return body.byteLength;
  }
  return null;
}

function rejectedBodyStream(stream, error) {
  const channel = isFetchChannelStream(stream);
  const source = channel
    ? {
        type: 'bytes',
        start(controller) {
          Promise.resolve(stream.cancel(error)).catch(() => {});
          controller.error(error);
        },
      }
    : {
        start(controller) {
          Promise.resolve(stream.cancel(error)).catch(() => {});
          controller.error(error);
        },
      };
  const rejected = new ReadableStream(source);
  // A configured limit errors the native body without changing its reader
  // interface or losing the request context carried by the original stream.
  return channel ? inheritFetchChannelStream(stream, rejected) : rejected;
}

function limitedBodyStream(stream, limit, kind, state = undefined) {
  const reader = stream.getReader();
  let total = 0;
  let finished = false;
  const source = {
    async pull(controller) {
      const result = await reader.read();
      if (result.done) {
        finished = true;
        reader.releaseLock();
        controller.close();
        controller.byobRequest?.respond(0);
        return;
      }

      total += bodyChunkSize(result.value);
      if (total > limit) {
        const error = bodyLimitError(kind, limit);
        if (state !== undefined) {
          state.error = error;
        }
        finished = true;
        try {
          await reader.cancel(error);
        } catch {
          // The configured limit remains the observable failure.
        }
        reader.releaseLock();
        controller.error(error);
        return;
      }
      controller.enqueue(result.value);
    },
    async cancel(reason) {
      if (finished) {
        return;
      }
      finished = true;
      try {
        await reader.cancel(reason);
      } finally {
        reader.releaseLock();
      }
    },
  };
  if (kind === 'response' && isFetchChannelStream(stream)) {
    source.type = 'bytes';
  }
  const limited = new ReadableStream(source);
  return kind === 'response' ? inheritFetchChannelStream(stream, limited) : limited;
}

function limitedNativeRequest(input, init, limit) {
  if (init !== undefined && init !== null && hasOwn.call(init, 'body')) {
    const size = knownBodySize(init.body);
    if (size !== null && size > limit) {
      throw bodyLimitError('request', limit);
    }
    if (size !== null) {
      return { input, init, state: null };
    }
  }

  const request = new NativeRequest(input, init);
  if (request.body === null) {
    return { input, init, state: null };
  }
  const state = { error: null };
  return {
    input: new NativeRequest(request, {
      body: limitedBodyStream(request.body, limit, 'request', state),
      duplex: 'half',
    }),
    init: undefined,
    state,
  };
}

function limitedResponseBody(response, limit) {
  if (response.body === null) {
    return null;
  }
  const contentLength = response.headers.get('content-length');
  const ordinary = isOrdinaryFetchBody(response.body);
  const encodedGzip =
    (ordinary || isFetchChannelStream(response.body)) &&
    strToLowerCase.call(response.headers.get('content-encoding') ?? '') === 'gzip';
  if (
    !encodedGzip &&
    contentLength !== null &&
    /^[0-9]+$/.test(contentLength) &&
    Number(contentLength) > limit
  ) {
    return rejectedBodyStream(response.body, bodyLimitError('response', limit));
  }
  // Ordinary native bodies count decoded producer bytes directly, retaining
  // their native reader and demand geometry under an enabled limit.
  if (ordinary) return response.body;
  return limitedBodyStream(response.body, limit, 'response');
}

function assignRequestSlots(
  req,
  url,
  method,
  credentials,
  redirect,
  mode,
  headers,
  rawHeaders,
  signal,
  signalState,
  noBody,
  bodyInit,
  bodySize,
  bodyStream,
) {
  req.url = url;
  req.method = method;
  req.credentials = credentials;
  req.redirect = redirect;
  req.mode = mode;
  req.referrer = null;
  req._headers = headers;
  req._rawHeaders = rawHeaders;
  req._signal = signal;
  req._signalState = signalState;
  req._noBody = noBody;
  req._bodyInit = bodyInit;
  req._bodySize = bodySize;
  req._bodyStream = bodyStream;
  req._bodyConsumed = false;
  req._bodyFastKind = 0;
  req._bodyFastData = null;
  req._bodyExposed = false;
}

class Request {
  constructor(input, init = undefined) {
    const normalizedInput = normalizeRequestInfo(input);
    const options = normalizeRequestInit(init);

    let url;
    let credentials;
    let redirect;
    let headers = null;
    let method;
    let mode;
    let signal;
    let body = options.body;
    let bodyFromOptions = body !== undefined && body !== null;

    if (normalizedInput instanceof Request || normalizedInput instanceof NativeRequest) {
      if (!bodyFromOptions && normalizedInput.bodyUsed) {
        throw new TypeError('Already read');
      }
      url = normalizedInput.url;
      credentials = normalizedInput.credentials;
      redirect = normalizedInput.redirect;
      if (!options.headers) {
        headers = new Headers(normalizedInput.headers);
      }
      method = normalizedInput.method;
      mode = normalizedInput.mode;
      signal = normalizedInput.signal;
      if (!bodyFromOptions && normalizedInput.body !== null) {
        body = normalizedInput.body;
        normalizedInput.bodyUsed = true;
      }
    } else {
      url = String(normalizedInput);
    }

    credentials = options.credentials || credentials || 'same-origin';
    redirect = options.redirect || redirect || 'follow';
    if (options.headers) {
      headers = options.headers instanceof Headers ? options.headers : new Headers(options.headers);
    } else if (headers === null) {
      headers = createEmptyHeaders();
    }
    method = normalizeMethod(options.method || method || 'GET');
    mode = options.mode || mode || null;
    signal = options.signal || signal || null;

    if ((method === 'GET' || method === 'HEAD') && body) {
      throw new TypeError('Body not allowed for GET or HEAD requests');
    }
    if (bodyFromOptions && body instanceof ReadableStream) {
      if (options.duplex !== 'half') {
        throw new TypeError('ReadableStream body requires duplex: "half" option');
      }
    }

    if (
      (method === 'GET' || method === 'HEAD') &&
      (options.cache === 'no-store' || options.cache === 'no-cache')
    ) {
      const reParamSearch = /([?&])_=[^&]*/;
      if (reParamSearch.test(url)) {
        url = url.replace(reParamSearch, `$1_=${Date.now()}`);
      } else {
        url += (url.indexOf('?') >= 0 ? '&' : '?') + `_=${Date.now()}`;
      }
    }

    assignRequestSlots(
      this,
      url,
      method,
      credentials,
      redirect,
      mode,
      headers,
      null,
      signal,
      null,
      true,
      null,
      0,
      null,
    );
    const hState = getHeaderState(headers) ?? null;
    initBodyFields(this, body === undefined ? null : body, hState);
  }

  get headers() {
    if (this._headers === null && this._rawHeaders !== null) {
      const raw = this._rawHeaders;
      this._rawHeaders = null;
      this._headers = createHeadersFromIncomingPairs(raw);
    }
    return this._headers;
  }

  set headers(value) {
    this._rawHeaders = null;
    this._headers = value;
  }

  get signal() {
    if (this._signal !== null) return this._signal;
    const state = this._signalState;
    if (state !== null) {
      this._signalState = null;
      if (state.abortController === null) {
        state.abortController = new AbortController();
        if (state.cancelled) {
          state.abortController.abort(state.cancelReason);
        }
      }
      this._signal = state.abortController.signal;
      return this._signal;
    }
    return null;
  }

  set signal(value) {
    this._signalState = null;
    this._signal = value;
  }

  get body() {
    if (this._bodyFastKind !== 0) {
      return materializeFastBody(this, true);
    }
    return this._bodyStream;
  }

  set body(value) {
    this._bodyExposed = true;
    this._bodyFastKind = 0;
    this._bodyFastData = null;
    this._bodyStream = value;
  }

  get bodyUsed() {
    if (this._bodyConsumed) return true;
    const stream = this._bodyStream;
    return (
      stream !== null && stream !== undefined && bodyProperty(stream, '_disturbed')?.value === true
    );
  }

  set bodyUsed(value) {
    this._bodyConsumed = Boolean(value);
  }

  arrayBuffer() {
    return consumeArrayBuffer(this);
  }

  blob() {
    return consumeBlob(this);
  }

  formData() {
    return consumeFormData(this);
  }

  json() {
    return consumeJson(this);
  }

  text() {
    return consumeText(this);
  }

  clone() {
    if (this.bodyUsed) {
      throw new TypeError('Already read');
    }
    let body2 = null;
    if (this._bodyFastKind !== 0) {
      body2 = this._bodyFastKind === 1 ? this._bodyFastData : u8Slice.call(this._bodyFastData);
    } else if (this._bodyStream instanceof ReadableStream) {
      const [body1, teeBody] = this._bodyStream.tee();
      this._bodyStream = body1;
      body2 = teeBody;
    } else {
      body2 = this._bodyStream;
    }
    const cloned = new Request(this.url, {
      body: body2,
      credentials: this.credentials,
      duplex: body2 instanceof ReadableStream ? 'half' : undefined,
      headers: new Headers(this.headers),
      method: this.method,
      mode: this.mode,
      redirect: this.redirect,
      signal: this.signal,
    });
    if (this.cache !== undefined) cloned.cache = this.cache;
    if (this.destination !== undefined) cloned.destination = this.destination;
    if (this.integrity !== undefined) cloned.integrity = this.integrity;
    if (this.isHistoryNavigation !== undefined) {
      cloned.isHistoryNavigation = this.isHistoryNavigation;
    }
    if (this.isReloadNavigation !== undefined) cloned.isReloadNavigation = this.isReloadNavigation;
    if (this.keepalive !== undefined) cloned.keepalive = this.keepalive;
    if (this.referrerPolicy !== undefined) cloned.referrerPolicy = this.referrerPolicy;
    return cloned;
  }
}

function validateIncomingHeaderPairs(rawPairs) {
  if (rawPairs === undefined || rawPairs === null) return;
  const direct = stringPrototype.charCodeAt === strCharCodeAt;
  for (let i = 0; i < rawPairs.length; i++) {
    const pair = rawPairs[i];
    const name = typeof pair[0] === 'string' ? pair[0] : headerMutationString(pair[0]);
    const nLen = name.length;
    for (let j = 0; j < nLen; j++) {
      const c = direct ? name.charCodeAt(j) : strCharCodeAt.call(name, j);
      if (c < 128 && HEADER_NAME_TABLE[c] === 0) {
        throw new bodyTypeError('Invalid header name.');
      }
    }
    const value = typeof pair[1] === 'string' ? pair[1] : headerMutationString(pair[1]);
    const vLen = value.length;
    let start = 0;
    while (start < vLen) {
      const c = direct ? value.charCodeAt(start) : strCharCodeAt.call(value, start);
      if (c > 32 || (HEADER_VALUE_TABLE[c] & 1) === 0) break;
      start++;
    }
    let end = vLen;
    while (end > start) {
      const c = direct ? value.charCodeAt(end - 1) : strCharCodeAt.call(value, end - 1);
      if (c > 32 || (HEADER_VALUE_TABLE[c] & 1) === 0) break;
      end--;
    }
    for (let j = start; j < end; j++) {
      const c = direct ? value.charCodeAt(j) : strCharCodeAt.call(value, j);
      if (c > 13) continue;
      if (c === 0 || c === 10 || c === 13) {
        throw new bodyTypeError('Invalid header value.');
      }
    }
  }
}

export function createIncomingRequest(url, method, headerEntries, body, hasBody, abortState) {
  validateIncomingHeaderPairs(headerEntries);
  const req = Object.create(Request.prototype);
  assignRequestSlots(
    req,
    url,
    normalizeMethod(method),
    'same-origin',
    'follow',
    null,
    null,
    headerEntries,
    null,
    abortState,
    !hasBody,
    hasBody ? body : null,
    hasBody ? -1 : 0,
    hasBody ? body : null,
  );
  return req;
}

/*
 * Shared byte-level multipart/form-data parser used by both Request and
 * Response. Must consume the body via arrayBuffer() (which sets bodyUsed)
 * rather than reading the body stream directly.
 */
const crlfBytes = new Uint8Array([0x0d, 0x0a]);
const doubleCrlfBytes = new Uint8Array([0x0d, 0x0a, 0x0d, 0x0a]);

function extractBoundary(contentType) {
  const match = /boundary\s*=\s*(?:"([^"]+)"|'([^']+)'|([^;]+))/i.exec(contentType);
  if (!match) {
    throw new TypeError('multipart/form-data missing boundary');
  }
  return match[1] || match[2] || match[3];
}

function buildDelimiter(boundary) {
  return textEncoderEncode.call(sharedTextEncoder, `--${boundary}`);
}

function isCrlfBefore(bytes, index) {
  return (
    index >= crlfBytes.byteLength &&
    bytes[index - 2] === crlfBytes[0] &&
    bytes[index - 1] === crlfBytes[1]
  );
}

async function parseMultipartFormData(request) {
  const contentType = request.headers.get('content-type');
  const boundary = extractBoundary(contentType);
  const delimiter = buildDelimiter(boundary);

  const bodyBuffer = await request.arrayBuffer();
  const bytes = new Uint8Array(bodyBuffer);

  const form = new FormData();
  let offset = 0;

  while (offset < bytes.byteLength) {
    const delimiterIndex = indexOfBytes(bytes, delimiter, offset);
    if (delimiterIndex < 0) {
      break;
    }
    if (delimiterIndex !== 0 && !isCrlfBefore(bytes, delimiterIndex)) {
      offset = delimiterIndex + delimiter.byteLength;
      continue;
    }
    offset = delimiterIndex + delimiter.byteLength;

    if (offset + 1 < bytes.byteLength && bytes[offset] === 0x2d && bytes[offset + 1] === 0x2d) {
      break;
    }

    if (
      offset + crlfBytes.byteLength <= bytes.byteLength &&
      bytes[offset] === crlfBytes[0] &&
      bytes[offset + 1] === crlfBytes[1]
    ) {
      offset += crlfBytes.byteLength;
    }

    const headerEndIndex = indexOfBytes(bytes, doubleCrlfBytes, offset);
    if (headerEndIndex < 0) {
      break;
    }

    const headerBytes = bytes.subarray(offset, headerEndIndex);
    const headerText = textDecoderDecode.call(sharedTextDecoder, headerBytes);
    const partHeaders = parsePartHeaders(headerText);

    offset = headerEndIndex + doubleCrlfBytes.byteLength;

    let nextDelimiterIndex = -1;
    let bodyEnd = bytes.byteLength;
    let searchOffset = offset;
    while (searchOffset < bytes.byteLength) {
      const candidateIndex = indexOfBytes(bytes, delimiter, searchOffset);
      if (candidateIndex < 0) {
        break;
      }
      if (candidateIndex === 0 || isCrlfBefore(bytes, candidateIndex)) {
        nextDelimiterIndex = candidateIndex;
        break;
      }
      searchOffset = candidateIndex + delimiter.byteLength;
    }

    if (nextDelimiterIndex >= 0) {
      bodyEnd = nextDelimiterIndex;
      if (isCrlfBefore(bytes, bodyEnd)) {
        bodyEnd -= crlfBytes.byteLength;
      }
    }

    const partBody = bytes.subarray(offset, bodyEnd);

    const disposition = partHeaders['content-disposition'] || '';
    const nameMatch = /name="([^"]+)"/.exec(disposition);
    const filenameMatch = /filename="([^"]+)"/.exec(disposition);
    const fieldName = nameMatch ? nameMatch[1] : '';

    if (filenameMatch) {
      const filename = filenameMatch[1];
      const partContentType = partHeaders['content-type'] || 'application/octet-stream';
      const file = new File([partBody.slice()], filename, {
        type: partContentType,
      });
      form.append(fieldName, file);
    } else {
      const value = textDecoderDecode.call(sharedTextDecoder, partBody);
      form.append(fieldName, value);
    }

    offset = nextDelimiterIndex >= 0 ? nextDelimiterIndex : bytes.byteLength;
  }

  return form;
}

function indexOfBytes(haystack, needle, start) {
  const limit = haystack.byteLength - needle.byteLength;
  for (let i = start; i <= limit; i++) {
    let match = true;
    for (let j = 0; j < needle.byteLength; j++) {
      if (haystack[i + j] !== needle[j]) {
        match = false;
        break;
      }
    }
    if (match) {
      return i;
    }
  }
  return -1;
}

function parsePartHeaders(text) {
  const headers = {};
  const lines = text.split('\r\n');
  let currentName = '';
  for (const line of lines) {
    if (line.startsWith(' ') || line.startsWith('\t')) {
      if (currentName) {
        headers[currentName] += ` ${line.trim()}`;
      }
    } else {
      const colonIndex = line.indexOf(':');
      if (colonIndex > 0) {
        currentName = line.substring(0, colonIndex).trim().toLowerCase();
        headers[currentName] = line.substring(colonIndex + 1).trim();
      }
    }
  }
  return headers;
}

function assignResponseSlots(
  res,
  type,
  status,
  ok,
  statusText,
  headers,
  headersState,
  url,
  redirected,
  noBody,
  bodyInit,
  bodySize,
  bodyStream,
  bodyFastKind,
  bodyFastData,
) {
  res.type = type;
  res.status = status;
  res.ok = ok;
  res.statusText = statusText;
  res._headers = headers;
  res._headersState = headersState;
  res.url = url;
  res.redirected = redirected;
  res._noBody = noBody;
  res._bodyInit = bodyInit;
  res._bodySize = bodySize;
  res._bodyStream = bodyStream;
  res._bodyConsumed = false;
  res._bodyFastKind = bodyFastKind;
  res._bodyFastData = bodyFastData;
  res._bodyExposed = false;
}

function constructResponseWithHeaders(
  target,
  body,
  hState,
  initStatus,
  initStatusText,
  initWebSocket,
) {
  const status = initStatus === undefined ? 200 : Number(initStatus);
  let normalizedBody = normalizeBody(body);

  if (
    initWebSocket !== undefined &&
    initWebSocket !== null &&
    !isWebSocketEndpoint(initWebSocket)
  ) {
    throw new bodyTypeError(
      "Incorrect type for the 'webSocket' field on 'InitializerDict': the provided value is not of type 'WebSocket'.",
    );
  }
  if (status === 101) {
    if (initWebSocket === undefined || initWebSocket === null) {
      throw new bodyRangeError(
        'Responses may only be constructed with status codes in the range 200 to 599, inclusive.',
      );
    }
    if (isEmptyFixedResponseBody(normalizedBody)) normalizedBody = null;
  } else {
    if (initWebSocket !== undefined && initWebSocket !== null) {
      throw new bodyRangeError('Responses with a WebSocket must have status code 101.');
    }
    if (!(status >= 200 && status <= 599)) {
      throw new bodyRangeError(`The status provided (${status}) is outside the range [200, 599].`);
    }
  }

  if (normalizedBody !== null && normalizedBody !== undefined && isNullBodyStatus(status)) {
    throw new bodyTypeError(
      status === 101
        ? 'Response with null body status (101, 204, 205, or 304) cannot have a body.'
        : 'Response with a null-body status cannot have a body',
    );
  }

  const statusText =
    status === 101
      ? initStatusText === undefined
        ? 'Switching Protocols'
        : String(initStatusText)
      : initStatusText === undefined
        ? ''
        : `${initStatusText}`;
  const ok = status >= 200 && status < 300;

  assignResponseSlots(
    target,
    'default',
    status,
    ok,
    statusText,
    null,
    hState,
    '',
    false,
    true,
    null,
    0,
    null,
    0,
    null,
  );
  if (status === 101) {
    setResponseWebSocket(target, initWebSocket);
  }
  initBodyFields(target, normalizedBody, hState);
  return target;
}

class Response {
  constructor(body = null, init = undefined) {
    let initHeaders;
    let initStatus;
    let initStatusText;
    let initWebSocket;
    if (init !== undefined && init !== null) {
      // Response's dictionary has no redirect/body member. Read only the
      // ResponseInit members in canonical order.
      initHeaders = init.headers;
      initStatus = init.status;
      initStatusText = init.statusText;
      initWebSocket = init.webSocket;
    }
    const hState = createHeaderState();
    if (initHeaders !== undefined) {
      initHeadersIntoState(hState, initHeaders);
    }
    constructResponseWithHeaders(this, body, hState, initStatus, initStatusText, initWebSocket);
  }

  get headers() {
    if (this._headers === null && this._headersState !== null) {
      const headers = Object.create(headersPrototype);
      setHeaderState(headers, this._headersState);
      this._headers = headers;
    }
    return this._headers;
  }

  set headers(value) {
    this._headers = value;
    this._headersState = null;
  }

  get body() {
    if (this._bodyFastKind !== 0) {
      return materializeFastBody(this, true);
    }
    return this._bodyStream;
  }

  set body(value) {
    this._bodyExposed = true;
    this._bodyFastKind = 0;
    this._bodyFastData = null;
    this._bodyStream = value;
  }

  get bodyUsed() {
    if (this._bodyConsumed) return true;
    const stream = this._bodyStream;
    return (
      stream !== null && stream !== undefined && bodyProperty(stream, '_disturbed')?.value === true
    );
  }

  set bodyUsed(value) {
    this._bodyConsumed = Boolean(value);
  }

  get webSocket() {
    return getResponseWebSocket(this);
  }

  arrayBuffer() {
    return consumeArrayBuffer(this);
  }

  blob() {
    return consumeBlob(this);
  }

  formData() {
    return consumeFormData(this);
  }

  json() {
    return consumeJson(this);
  }

  text() {
    return consumeText(this);
  }

  clone() {
    if (getResponseWebSocket(this)) {
      throw new TypeError('Cannot clone a response to a WebSocket handshake.');
    }
    if (this.type === 'error') {
      const wrapped = Object.create(Response.prototype);
      const srcState =
        this._headersState !== null && !objectHasOwn(this, 'headers')
          ? this._headersState
          : getHeaderState(this.headers);
      let clonedState;
      if (srcState !== undefined && srcState !== null) {
        clonedState = cloneHeaderState(srcState);
      } else {
        clonedState = createHeaderState();
        initHeadersIntoState(clonedState, this.headers);
      }
      assignResponseSlots(
        wrapped,
        this.type,
        this.status,
        this.ok,
        this.statusText,
        null,
        clonedState,
        this.url,
        this.redirected,
        true,
        null,
        0,
        null,
        0,
        null,
      );
      return wrapped;
    }
    let body2 = null;
    if (this._bodyFastKind !== 0) {
      body2 = this._bodyFastKind === 1 ? this._bodyFastData : u8Slice.call(this._bodyFastData);
    } else if (this._bodyStream !== null) {
      if (this._bodyStream.locked) {
        throw new bodyTypeError('This ReadableStream is currently locked to a reader,');
      }
      if (this._bodyStream instanceof ReadableStream) {
        const [body1, teeBody] = this._bodyStream.tee();
        this._bodyStream = body1;
        body2 = teeBody;
      } else {
        body2 = this._bodyStream;
      }
    }
    const cloned = new Response(body2, {
      status: this.status,
      statusText: this.statusText,
      headers: this.headers,
    });
    cloned.ok = this.ok;
    cloned.redirected = this.redirected;
    cloned.type = this.type;
    cloned.url = this.url;
    const hState = this._headersState !== null ? this._headersState : getHeaderState(this.headers);
    if (hState !== undefined && hState !== null && hState.immutable) {
      finishFetchedResponse(cloned, this.url, this.redirected);
      if (this._bodyStream !== null) weakSetAdd.call(ordinaryFetchStreams, this._bodyStream);
    }
    return cloned;
  }

  static error() {
    const response = Object.create(Response.prototype);
    assignResponseSlots(
      response,
      'error',
      0,
      false,
      '',
      null,
      createHeaderState(),
      '',
      false,
      true,
      null,
      0,
      null,
      0,
      null,
    );
    return response;
  }

  static json(data, init = undefined) {
    const body = JSON.stringify(data);
    if (body === undefined) {
      throw new bodyTypeError('The data is not JSON serializable');
    }
    if (init === undefined || init === null) {
      const response = Object.create(Response.prototype);
      assignResponseSlots(
        response,
        'default',
        200,
        true,
        '',
        null,
        createHeaderStateWithOne('content-type', 'application/json'),
        '',
        false,
        false,
        body,
        -1,
        null,
        1,
        body,
      );
      return response;
    }
    const initHeaders = init.headers;
    const initStatus = init.status;
    const initStatusText = init.statusText;
    const initWebSocket = init.webSocket;
    const hState = createHeaderState();
    if (initHeaders !== undefined) {
      initHeadersIntoState(hState, initHeaders);
    }
    if (!headerStateHas(hState, 'content-type')) {
      headerStateSetNormalized(hState, 'content-type', 'application/json');
    }
    const response = Object.create(Response.prototype);
    return constructResponseWithHeaders(
      response,
      body,
      hState,
      initStatus,
      initStatusText,
      initWebSocket,
    );
  }

  static redirect(url, status = 302) {
    const normalizedStatus = Number(status);
    if (
      normalizedStatus !== 301 &&
      normalizedStatus !== 302 &&
      normalizedStatus !== 303 &&
      normalizedStatus !== 307 &&
      normalizedStatus !== 308
    ) {
      throw new bodyRangeError('Invalid status code');
    }
    const hState = createHeaderStateWithOne('location', normalizeValue(String(url)));
    const response = Object.create(Response.prototype);
    return constructResponseWithHeaders(response, null, hState, normalizedStatus, '', undefined);
  }
}

async function fetchUpgradeRequest(request) {
  let redirected = false;
  const visited = [];
  for (let count = 0; ; count++) {
    const target = new URL(request.url);
    target.hash = '';
    visited.push(target.href);
    const result = await openFetchUpgrade(
      target.href,
      toNativeHeaderEntries(request.headers),
      request.redirect === 'manual' ? 1 : request.redirect === 'error' ? 2 : 0,
      request.signal,
    );
    const metadata = result.metadata;
    const responseHeaders = new Headers(metadata.headers);
    const location = responseHeaders.get('location');
    if (
      [301, 302, 303, 307, 308].includes(metadata.status) &&
      location !== null &&
      request.redirect !== 'manual'
    ) {
      if (count >= 20) {
        await result.body?.cancel();
        throw new bodyTypeError(`Too many redirects.${visited.join(', ')}`);
      }
      if (result.body) {
        const reader = result.body.getReader();
        try {
          while (!(await reader.read()).done) {}
        } finally {
          reader.releaseLock();
        }
      }
      const next = new URL(location, target);
      const headers = new Headers(request.headers);
      if (next.origin !== target.origin) headers.delete('authorization');
      request = new NativeRequest(next.href, {
        method: 'GET',
        headers: toNativeHeaderEntries(headers),
        redirect: request.redirect,
        signal: request.signal,
      });
      redirected = true;
      continue;
    }
    const body =
      result.body && maxFetchResponseBodyBytes !== 0
        ? limitedResponseBody(
            { body: result.body, headers: responseHeaders },
            maxFetchResponseBodyBytes,
          )
        : result.body;
    const responseInit = {
      status: metadata.status,
      statusText: metadata.statusText,
      headers: metadata.headers,
    };
    if (result.webSocket) {
      responseInit.webSocket = result.webSocket;
    }
    const response = new Response(body, responseInit);
    response.url = metadata.url;
    response.redirected = redirected || metadata.redirected;
    return response;
  }
}

function fetch(input, init = undefined) {
  let nativeInput;
  try {
    nativeInput = normalizeRequestInfo(input);
  } catch (error) {
    return Promise.reject(error);
  }
  let nativeInit = init;
  let requestLimitState = null;

  try {
    if (input instanceof Request) {
      nativeInput = input.url;
      nativeInit = {
        method: input.method,
        headers: new Headers(input.headers),
        credentials: input.credentials,
        redirect: input.redirect,
        mode: input.mode,
        signal: input.signal,
      };
      if (input.body !== null) {
        nativeInit.body = input.body;
        nativeInit.duplex = 'half';
      }
      if (init !== undefined && init !== null) {
        Object.assign(nativeInit, normalizeRequestInit(init));
      }
    } else if (init !== undefined) {
      nativeInit = normalizeRequestInit(init);
    }
  } catch (error) {
    return Promise.reject(error);
  }

  // Prepared Headers retain their raw owned keys for copy operations.
  // Read the private state, without application getters or public iteration,
  // when selecting the existing Upgrade transport.
  const fields = nativeInit?.headers;
  const ownedState = fields !== undefined && fields !== null ? getHeaderState(fields) : undefined;
  const upgradeValue = ownedState === undefined ? null : headerStateGet(ownedState, 'upgrade');
  let upgrade = upgradeValue !== null && strToLowerCase.call(upgradeValue) === 'websocket';
  if (Array.isArray(fields)) {
    for (let index = 0; index < fields.length; index++) {
      if (fields[index][0] === 'upgrade' && strToLowerCase.call(fields[index][1]) === 'websocket') {
        upgrade = true;
      }
    }
  }
  if (upgrade) {
    try {
      return fetchUpgradeRequest(new NativeRequest(nativeInput, nativeInit));
    } catch (error) {
      return Promise.reject(error);
    }
  }

  if (maxFetchRequestBodyBytes !== 0) {
    try {
      const limited = limitedNativeRequest(nativeInput, nativeInit, maxFetchRequestBodyBytes);
      nativeInput = limited.input;
      nativeInit = limited.init;
      requestLimitState = limited.state;
    } catch (error) {
      return Promise.reject(error);
    }
  }

  return nativeFetch(nativeInput, nativeInit)
    .catch((error) => {
      if (requestLimitState?.error) {
        throw requestLimitState.error;
      }
      throw error;
    })
    .then((response) => {
      if (response instanceof Response) {
        return finishFetchedResponse(response, response.url, response.redirected);
      }
      const nullBody = isNullBodyStatus(response.status);
      if (nullBody && response.body !== null) {
        response.body.cancel().catch(() => {});
      }
      const body = nullBody
        ? null
        : maxFetchResponseBodyBytes === 0
          ? response.body
          : limitedResponseBody(response, maxFetchResponseBodyBytes);
      const wrapped = new Response(nullBody ? null : body, {
        status: response.status,
        statusText: response.statusText,
        headers: wrapNormalizedNativeHeaders(response.headers),
      });
      return finishFetchedResponse(wrapped, response.url, response.redirected);
    });
}

// Preserve owned raw keys through vendor Request/Response copies without
// invoking an application replacement of Headers[Symbol.iterator].
configureOwnedHeadersCloner((init) => {
  const owned = weakMapGet.call(headerStates, init);
  return owned === undefined ? undefined : cloneHeadersFromOwnedState(owned);
});

Object.defineProperties(globalThis, {
  Headers: { configurable: true, writable: true, value: Headers },
  Request: { configurable: true, writable: true, value: Request },
  Response: { configurable: true, writable: true, value: Response },
  fetch: { configurable: true, writable: true, value: fetch },
});
