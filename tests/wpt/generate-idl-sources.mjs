#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';

const [ wptRoot, outputPath, ...specs ] = process.argv.slice(2);
if (!wptRoot || !outputPath || specs.length === 0) {
    throw new TypeError(
        'usage: generate-idl-sources.mjs <wpt-root> <output> <spec...>');
}

const sources = Object.fromEntries(specs.map(spec => [
    spec,
    fs.readFileSync(
        path.join(wptRoot, 'interfaces', `${spec}.idl`),
        'utf8'),
]));

fs.writeFileSync(
    outputPath,
    `const __wptIdlSources = Object.freeze(${JSON.stringify(sources)});\n` +
    'globalThis.fetch_spec = spec => {\n' +
    '    const idl = __wptIdlSources[spec];\n' +
    '    if (idl === undefined) {\n' +
    '        return Promise.reject(new TypeError(`unknown IDL: ${spec}`));\n' +
    '    }\n' +
    '    return Promise.resolve({ spec, idl });\n' +
    '};\n');
