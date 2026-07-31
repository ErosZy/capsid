import fs from 'node:fs';

const [ outputPath, ...inputPaths ] = process.argv.slice(2);

if (!outputPath || inputPaths.length === 0) {
    throw new Error('expected output path followed by input paths');
}

const sources = inputPaths.map(path => {
    let source = fs.readFileSync(path, 'utf8');
    if (path.endsWith('/encoding/resources/single-byte-decoder.js')) {
        const duplicateHelpers = source.indexOf(
            '\n// For TextDecoder tests');
        if (duplicateHelpers < 0) {
            throw new Error(
                `single-byte decoder resource format changed: ${path}`);
        }
        source = source.slice(0, duplicateHelpers);
    }
    if (path.endsWith(
        '/wasm/jsapi/constructor/instantiate-bad-imports.any.js')) {
        const occurrences = source.match(/\.\.\.arguments/g)?.length ?? 0;
        if (occurrences !== 4) {
            throw new Error(
                `instantiate bad-imports source format changed: ${path}`);
        }
        // The upstream .any.js is a classic script, where a rest binding named
        // "arguments" is valid. Our per-file harness bundles as strict ESM, so
        // rename only that local binding without changing any assertion.
        source = source.replaceAll(
            '...arguments',
            '...importArguments',
        );
    }
    return source;
});
fs.writeFileSync(outputPath, sources.join('\n'));
