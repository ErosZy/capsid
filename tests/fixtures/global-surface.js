import { profileGlobalNames, profileId } from '../../js/profile-manifest.js';

export default {
    fetch() {
        const actual = Object.getOwnPropertyNames(globalThis).sort();
        const expected = [ ...profileGlobalNames ];
        const expectedSet = new Set(expected);
        const actualSet = new Set(actual);
        const missing = expected.filter(name => !actualSet.has(name));
        const unexpected = actual.filter(name => !expectedSet.has(name));
        return new Response(JSON.stringify({
            profile: profileId,
            matches: missing.length === 0 && unexpected.length === 0,
            missing,
            unexpected,
            userAgent: navigator.userAgent,
            required: {
                fetch: typeof fetch,
                Request: typeof Request,
                Response: typeof Response,
                ReadableStream: typeof ReadableStream,
                crypto: typeof crypto,
                Crypto: typeof Crypto,
                CryptoKey: typeof CryptoKey,
                SubtleCrypto: typeof SubtleCrypto,
                performance: typeof performance,
                Performance: typeof Performance,
                reportError: typeof reportError,
                WebAssembly: typeof WebAssembly,
            },
        }), {
            headers: { 'content-type': 'application/json' },
        });
    },
};
