import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

export const guardedRejectionTests = Object.freeze([
    'delayed handling: delaying handling by setTimeout(,10) will cause both events to fire',
    'delayed handling: delaying handling rejected promise created from createImageBitmap will cause both events to fire',
]);

// Add observation checks to two locked upstream bodies without changing or
// removing their existing statements. The original reason assertions remain
// authoritative, including their disagreement with the default workerd profile.
export function guardRejectionEvents(source) {
    if (source.includes('__capsidHandledCallbacks')) {
        throw new Error('rejection source is already guarded');
    }
    const opening = 'async_test(function(t) {\n';
    const callback = '  var handled = function(ev) {\n    if (ev.promise === p) {\n';
    for (const name of guardedRejectionTests) {
        const ending = `}, '${name}');`;
        const end = source.indexOf(ending);
        if (end < 0 || end !== source.lastIndexOf(ending)) {
            throw new Error(`guard target must occur exactly once: ${name}`);
        }
        const start = source.lastIndexOf(opening, end);
        const body = source.slice(start, end);
        if (start < 0 || !body.includes(callback) ||
                body.indexOf(callback) !== body.lastIndexOf(callback)) {
            throw new Error(`guard target callback layout changed: ${name}`);
        }
        const guarded = body.replace(opening, opening +
            '  var __capsidHandledCallbacks = 0;\n' +
            '  t.add_cleanup(function() {\n' +
            "    assert_equals(__capsidHandledCallbacks, 1, 'required rejectionhandled callback count');\n" +
            '  });\n').replace(callback, callback + '      __capsidHandledCallbacks++;\n');
        source = source.slice(0, start) + guarded + source.slice(end);
    }
    return source;
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    if (!process.argv[2] || !process.argv[3]) {
        throw new Error('usage: guard-rejection-events.mjs SOURCE OUTPUT');
    }
    const guarded = guardRejectionEvents(fs.readFileSync(process.argv[2], 'utf8'));
    fs.writeFileSync(process.argv[3], guarded);
}
