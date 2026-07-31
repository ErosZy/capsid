#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';

const [ rootPath, outputPath, ...resourcePaths ] = process.argv.slice(2);
if (!rootPath || !outputPath || resourcePaths.length === 0) {
    throw new TypeError(
        'usage: generate-resource-map.mjs <root> <output> <resource...>');
}

const entries = resourcePaths.map(resourcePath => {
    const absolutePath = path.join(rootPath, resourcePath);
    const encoded = fs.readFileSync(absolutePath).toString('base64');
    return `    ${JSON.stringify(resourcePath)}: ${JSON.stringify(encoded)}`;
});

const source = `// Generated from the pinned WPT checkout.
const __wptResourceBase64 = Object.freeze({
${entries.join(',\n')}
});
const __wptNativeFetch = globalThis.fetch;
Object.defineProperty(globalThis, 'fetch', {
    configurable: true,
    writable: true,
    value(input, init) {
        const url = new URL(
            input instanceof Request ? input.url : String(input),
            globalThis.location.href);
        const key = url.pathname.replace(/^\\//, '');
        const encoded = __wptResourceBase64[key];
        if (url.origin === 'https://wpt.local' && encoded !== undefined) {
            const binary = atob(encoded);
            const bytes = Uint8Array.from(binary, value => value.charCodeAt(0));
            const response = new Response(bytes);
            Object.defineProperty(response, 'bytes', {
                configurable: true,
                value: async () => new Uint8Array(await response.arrayBuffer()),
            });
            return Promise.resolve(response);
        }
        return __wptNativeFetch(input, init);
    },
});
`;

fs.writeFileSync(outputPath, source);
