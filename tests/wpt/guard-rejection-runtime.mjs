import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { parseArgs } from 'node:util';
import { fileURLToPath } from 'node:url';
import { guardRejectionEvents, guardedRejectionTests } from './guard-rejection-events.mjs';

const digest = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const relative = 'html/webappapis/scripting/processing-model-2/unhandled-promise-rejections/support/promise-rejection-events.js';

export async function runRejectionRuntimeControls(options) {
    for (const name of ['source', 'wpt-root', 'integration', 'worker', 'esbuild']) {
        assert.ok(options[name], `missing --${name}`);
    }
    const root = path.resolve(options.source);
    const upstream = path.join(options['wpt-root'], relative);
    const original = fs.readFileSync(upstream, 'utf8');
    // Use the same CLI selected by CMake. ESBUILD_BINARY_PATH is the JS
    // API's native-binary override; a Unix CLI wrapper is not that binary.
    const runBundler = args => {
        const child = spawnSync(options.esbuild, args, {encoding:'utf8', timeout:20000});
        assert.equal(child.error, undefined, 'configured bundler launch or timeout failure');
        assert.equal(child.signal, null, 'configured bundler terminated by a signal');
        assert.equal(child.status, 0, `configured bundler failed: ${child.stdout}${child.stderr}`);
        return child.stdout.trim();
    };
    const bundlerVersion = runBundler(['--version']);
    const lock = JSON.parse(fs.readFileSync(path.join(root, 'vendor/txiki.js/package-lock.json'), 'utf8'));
    assert.equal(bundlerVersion, lock.packages['node_modules/esbuild'].version,
        'configured fixture bundler must match its source lock');
    const work = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-wpt-presence-'));
    const results = [];
    try {
        const environment = path.join(work, 'environment.mjs');
        fs.writeFileSync(environment,
            `Object.defineProperty(globalThis, 'location', {value:{href:${JSON.stringify('https://wpt.local/' + relative)}}});\n` +
            "globalThis.createImageBitmap = () => Promise.reject(new DOMException('', 'InvalidStateError'));\n");
        // First force the registered 10ms timers to become due during module
        // evaluation. A missing startup checkpoint then fails deterministically
        // instead of occasionally hiding the callbacks on a slower build.
        const variants = ['original-normal', 'original-missing', 'guarded-normal', 'guarded-missing'];
        for (const [evaluationDelayMs, name] of [100, 0].flatMap(delay => variants.map(name => [delay, name]))) {
            const key = `${name}-load-${evaluationDelayMs}`;
            const source = path.join(work, `${key}-source.mjs`);
            fs.writeFileSync(source, name.startsWith('guarded') ? guardRejectionEvents(original) : original);
            const selection = path.join(work, `${key}-selection.mjs`);
            fs.writeFileSync(selection,
                `const names = ${JSON.stringify(guardedRejectionTests)};\n` +
                'const originalAsyncTest = globalThis.async_test;\n' +
                'globalThis.async_test = (callback, name) => names.includes(name) ? originalAsyncTest(callback, name) : undefined;\n' +
                (name.endsWith('missing') ?
                    'const originalAdd = globalThis.addEventListener;\n' +
                    'globalThis.addEventListener = function(type, ...args) { if(type === "rejectionhandled") return; return originalAdd.call(this, type, ...args); };\n' : ''));
            const entry = path.join(work, `${key}-entry.mjs`);
            fs.writeFileSync(entry, [environment, path.join(root, 'tests/wpt/adapter.js'), selection, source]
                .map(file => `import ${JSON.stringify(file)};\n`).join('') +
                `import app from ${JSON.stringify(path.join(root, 'tests/wpt/app.js'))};\n` +
                'globalThis.__wptSeal();\n' +
                (evaluationDelayMs ? `const until = performance.now() + ${evaluationDelayMs}; while (performance.now() < until) {}\n` : '') +
                'export default app;\n');
            const bundle = path.join(work, `${key}.mjs`);
            runBundler([entry, '--bundle', '--target=esnext', '--platform=neutral',
                '--format=esm', '--log-level=silent', `--outfile=${bundle}`]);
            const command = [options.integration, options.worker, bundle, 'wpt', 'https://wpt.local/' + relative];
            const child = spawnSync(command[0], command.slice(1), {encoding:'utf8', timeout:20000});
            assert.equal(child.error, undefined, `${name}: launch or timeout failure`);
            assert.equal(child.signal, null, `${name}: unexpected signal`);
            const output = child.stdout + child.stderr;
            const match = output.match(/(\{"profile":"CAPSID-MIN-2025-subset-v0"[^\n]*\})/);
            assert.ok(match, `${name}: real WPT report is required: ${output}`);
            const report = JSON.parse(match[1]);
            const record = {name, evaluationDelayMs, command, exit:child.status, report, output,
                bundleSHA256:digest(bundle)};
            results.push(record);
            assert.equal(report.total, 2, `${name}: both named upstream bodies executed`);
            assert.deepEqual(report.expectedFailures, [], `${name}: no expected-failure waiver`);
            assert.deepEqual(report.missingExpectedFailures, []);
            if (name === 'original-missing') {
                // Counterfactual: without the added observation checks this
                // exact missing-listener mutation was accepted by the real driver.
                assert.equal(child.status, 0);
                assert.equal(report.passed, true);
                assert.deepEqual(report.failures, []);
            } else {
                assert.equal(child.status, 1);
                assert.equal(report.passed, false);
                assert.deepEqual(report.failures.map(x => x.name), guardedRejectionTests);
                if (name === 'guarded-missing') {
                    for (const failure of report.failures) {
                        assert.equal(failure.phase, 'cleanup');
                        assert.equal(failure.errorName, 'AssertionError');
                        assert.ok(failure.message.includes('required rejectionhandled callback count'));
                    }
                } else {
                    assert.deepEqual(report.failures.map(x => x.errorName), ['AssertionError', 'TypeError']);
                    assert.ok(report.failures.every(x => x.phase === 'test'),
                        `${name}: normal delivery must reach the original reason assertions`);
                }
            }
        }
        return {schema:'capsid-wpt-rejection-presence-controls-v2', complete:true,
            scope:'two named upstream cases, normal default-profile diagnostics and a missing-listener fault, with 100ms and ordinary module evaluation',
            workerSHA256:digest(options.worker), integrationSHA256:digest(options.integration),
            upstreamSHA256:digest(upstream), guardSHA256:digest(fileURLToPath(new URL('./guard-rejection-events.mjs', import.meta.url))),
            esbuild:bundlerVersion, esbuildExecutable:fs.realpathSync(options.esbuild), results};
    } catch (error) {
        // Preserve the actual earlier child output if a setup or assertion fails.
        console.error('partial runtime controls: ' + JSON.stringify(results));
        throw error;
    } finally {
        fs.rmSync(work, {recursive:true, force:true});
    }
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    const {values} = parseArgs({options:Object.fromEntries(
        ['source', 'wpt-root', 'integration', 'worker', 'esbuild'].map(name => [name, {type:'string'}]))});
    console.log(JSON.stringify(await runRejectionRuntimeControls(values), null, 2));
}
