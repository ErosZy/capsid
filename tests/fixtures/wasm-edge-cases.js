/*
 * Focused WebAssembly aliasing regressions.
 *
 * These cases intentionally run in separate worker fixtures (see the small
 * wasm-*.js entry points beside this file).  A broken Instance constructor
 * must not prevent the other resource classes from reporting their own
 * failures.
 */

const memoryGrowModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 1, 13, 3, 96, 0, 1, 127, 96, 1,
    127, 0, 96, 0, 1, 127, 2, 14, 1, 1, 109, 6, 109, 101, 109, 111,
    114, 121, 2, 1, 1, 2, 3, 4, 3, 0, 1, 2, 7, 31, 3, 4, 103, 114,
    111, 119, 0, 0, 9, 115, 116, 111, 114, 101, 84, 97, 105, 108, 0,
    1, 8, 108, 111, 97, 100, 84, 97, 105, 108, 0, 2, 10, 30, 3, 6,
    0, 65, 1, 64, 0, 11, 11, 0, 65, 128, 128, 4, 32, 0, 54, 2, 0,
    11, 9, 0, 65, 128, 128, 4, 40, 2, 0, 11, 0, 35, 4, 110, 97, 109,
    101, 1, 28, 3, 0, 4, 103, 114, 111, 119, 1, 9, 115, 116, 111, 114,
    101, 84, 97, 105, 108, 2, 8, 108, 111, 97, 100, 84, 97, 105, 108,
]);

const callbackModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 1, 8, 2, 96, 0, 0, 96, 0, 1, 127,
    2, 21, 2, 1, 109, 2, 99, 98, 0, 0, 1, 109, 6, 109, 101, 109, 111,
    114, 121, 2, 1, 1, 2, 3, 2, 1, 1, 7, 7, 1, 3, 114, 117, 110, 0,
    1, 10, 18, 1, 16, 0, 65, 0, 65, 41, 54, 2, 0, 16, 0, 65, 0, 40,
    2, 0, 11, 0, 13, 4, 110, 97, 109, 101, 1, 6, 1, 1, 3, 114, 117,
    110,
]);

const mutableGlobalModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 1, 9, 2, 96, 1, 127, 0, 96, 0, 1,
    127, 2, 13, 1, 1, 109, 6, 103, 108, 111, 98, 97, 108, 3, 127, 1,
    3, 3, 2, 0, 1, 7, 22, 3, 3, 115, 101, 116, 0, 0, 3, 103, 101,
    116, 0, 1, 6, 103, 108, 111, 98, 97, 108, 3, 0, 10, 13, 2, 6,
    0, 32, 0, 36, 0, 11, 4, 0, 35, 0, 11,
]);

const immutableGlobalModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 127, 2, 13, 1,
    1, 109, 6, 103, 108, 111, 98, 97, 108, 3, 127, 0, 3, 2, 1, 0,
    7, 7, 1, 3, 103, 101, 116, 0, 0, 10, 6, 1, 4, 0, 35, 0, 11,
    0, 13, 4, 110, 97, 109, 101, 1, 6, 1, 0, 3, 103, 101, 116,
]);

const tableGrowModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 1, 10, 2, 96, 1, 111, 1, 127, 96,
    0, 1, 127, 2, 14, 1, 1, 109, 5, 116, 97, 98, 108, 101, 1, 111,
    1, 1, 2, 3, 3, 2, 0, 1, 7, 15, 2, 4, 103, 114, 111, 119, 0, 0,
    4, 115, 105, 122, 101, 0, 1, 10, 17, 2, 9, 0, 32, 0, 65, 1,
    252, 15, 0, 11, 5, 0, 252, 16, 0, 11, 0, 20, 4, 110, 97, 109,
    101, 1, 13, 2, 0, 4, 103, 114, 111, 119, 1, 4, 115, 105, 122,
    101,
]);

/* Exported funcref table initialized with add/sub/mul at indexes 0..2. */
const exportedFuncrefTableModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 1, 14, 2, 96, 2, 127, 127, 1, 127,
    96, 3, 127, 127, 127, 1, 127, 3, 5, 4, 0, 0, 0, 1, 4, 4, 1,
    112, 0, 4, 7, 35, 4, 3, 116, 98, 108, 1, 0, 13, 99, 97, 108,
    108, 95, 105, 110, 100, 105, 114, 101, 99, 116, 0, 3, 3, 97, 100,
    100, 0, 0, 3, 115, 117, 98, 0, 1, 9, 9, 1, 0, 65, 0, 11, 3,
    0, 1, 2, 10, 37, 4, 7, 0, 32, 0, 32, 1, 106, 11, 7, 0, 32, 0,
    32, 1, 107, 11, 7, 0, 32, 0, 32, 1, 108, 11, 11, 0, 32, 1, 32,
    2, 32, 0, 17, 0, 0, 11,
]);

/* Exports mutable i32=42 and immutable i32=100 globals. */
const exportedGlobalModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 1, 17, 4, 96, 0, 1, 127, 96, 1,
    127, 0, 96, 0, 1, 126, 96, 1, 126, 0, 3, 5, 4, 0, 1, 2, 3, 6,
    44, 5, 127, 1, 65, 42, 11, 127, 0, 65, 228, 0, 11, 126, 1, 66,
    129, 128, 128, 128, 128, 128, 128, 16, 11, 125, 1, 67, 0, 0, 192,
    63, 11, 124, 1, 68, 31, 133, 235, 81, 184, 30, 9, 64, 11, 7, 119,
    9, 9, 103, 95, 105, 51, 50, 95, 109, 117, 116, 3, 0, 11, 103, 95,
    105, 51, 50, 95, 99, 111, 110, 115, 116, 3, 1, 9, 103, 95, 105,
    54, 52, 95, 109, 117, 116, 3, 2, 9, 103, 95, 102, 51, 50, 95,
    109, 117, 116, 3, 3, 9, 103, 95, 102, 54, 52, 95, 109, 117, 116,
    3, 4, 11, 103, 101, 116, 95, 105, 51, 50, 95, 109, 117, 116, 0,
    0, 11, 115, 101, 116, 95, 105, 51, 50, 95, 109, 117, 116, 0, 1,
    11, 103, 101, 116, 95, 105, 54, 52, 95, 109, 117, 116, 0, 2, 11,
    115, 101, 116, 95, 105, 54, 52, 95, 109, 117, 116, 0, 3, 10, 25,
    4, 4, 0, 35, 0, 11, 6, 0, 32, 0, 36, 0, 11, 4, 0, 35, 2, 11,
    6, 0, 32, 0, 36, 2, 11,
]);

/* Defines and exports memory with min=1, max=10. */
const exportedMemoryModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 1, 15, 3, 96, 2, 127, 127, 0, 96,
    1, 127, 1, 127, 96, 0, 1, 127, 3, 7, 6, 0, 1, 0, 1, 2, 1, 5,
    4, 1, 1, 1, 10, 7, 76, 7, 6, 109, 101, 109, 111, 114, 121, 2,
    0, 9, 115, 116, 111, 114, 101, 95, 105, 51, 50, 0, 0, 8, 108,
    111, 97, 100, 95, 105, 51, 50, 0, 1, 8, 115, 116, 111, 114, 101,
    95, 105, 56, 0, 2, 7, 108, 111, 97, 100, 95, 105, 56, 0, 3, 8,
    109, 101, 109, 95, 115, 105, 122, 101, 0, 4, 8, 109, 101, 109,
    95, 103, 114, 111, 119, 0, 5, 10, 49, 6, 9, 0, 32, 0, 32, 1,
    54, 2, 0, 11, 7, 0, 32, 0, 40, 2, 0, 11, 9, 0, 32, 0, 32, 1,
    58, 0, 0, 11, 7, 0, 32, 0, 45, 0, 0, 11, 4, 0, 63, 0, 11, 6,
    0, 32, 0, 64, 0, 11,
]);

const cases = {
    'shared-memory'() {
        const failures = [];
        const module = new WebAssembly.Module(memoryGrowModuleBytes);
        const memory = new WebAssembly.Memory({ initial: 1, maximum: 2 });
        const first = new WebAssembly.Instance(module, { m: { memory } });
        const second = new WebAssembly.Instance(module, { m: { memory } });

        if (first.exports.grow() !== 1) {
            failures.push('primary-memory-grow-result');
        }
        first.exports.storeTail(101);
        if (memory.buffer.byteLength !== 2 * 65536 ||
            new Int32Array(memory.buffer)[16384] !== 101 ||
            second.exports.loadTail() !== 101) {
            failures.push('primary-memory-grow-tail-liveness');
        }
        if (first.exports.grow() !== -1 ||
            memory.buffer.byteLength !== 2 * 65536) {
            failures.push('imported-memory-native-maximum');
        }
        const source = memoryGrowModuleBytes.slice().buffer;
        const snapshottedModule = new WebAssembly.Module(source);
        new Uint8Array(source)[0] = 0xff;
        const snapshotMemory =
            new WebAssembly.Memory({ initial: 1, maximum: 2 });
        if (new WebAssembly.Instance(
            snapshottedModule,
            { m: { memory: snapshotMemory } },
        ).exports.grow() !== 1) {
            failures.push('module-buffer-source-snapshot');
        }
        const callbackModule = new WebAssembly.Module(callbackModuleBytes);
        const callbackMemory =
            new WebAssembly.Memory({ initial: 1, maximum: 2 });
        new WebAssembly.Instance(
            callbackModule,
            { m: { memory: callbackMemory, cb() {} } },
        );
        let observed = -1;
        const callbackInstance = new WebAssembly.Instance(callbackModule, {
            m: {
                memory: callbackMemory,
                cb() {
                    observed = new Int32Array(callbackMemory.buffer)[0];
                    new Int32Array(callbackMemory.buffer)[0] = 42;
                },
            },
        });
        if (callbackInstance.exports.run() !== 42 || observed !== 41) {
            failures.push('import-callback-memory-liveness');
        }
        return failures;
    },

    'shared-global'() {
        const failures = [];
        const mutableModule = new WebAssembly.Module(mutableGlobalModuleBytes);
        const global = new WebAssembly.Global(
            { value: 'i32', mutable: true },
            0,
        );
        const first =
            new WebAssembly.Instance(mutableModule, { m: { global } });
        const second =
            new WebAssembly.Instance(mutableModule, { m: { global } });
        second.exports.set(29);
        if (global.value !== 29 || first.exports.get() !== 29) {
            failures.push('secondary-global-write-liveness');
        }

        const immutableModule =
            new WebAssembly.Module(immutableGlobalModuleBytes);
        const immutable = new WebAssembly.Global(
            { value: 'i32', mutable: false },
            17,
        );
        const immutableFirst =
            new WebAssembly.Instance(immutableModule, { m: { global: immutable } });
        const immutableSecond =
            new WebAssembly.Instance(immutableModule, { m: { global: immutable } });
        if (immutableFirst.exports.get() !== 17 ||
            immutableSecond.exports.get() !== 17) {
            failures.push('immutable-global-shared-binding');
        }
        return failures;
    },

    'shared-table'() {
        const failures = [];
        const module = new WebAssembly.Module(tableGrowModuleBytes);
        const table = new WebAssembly.Table({
            element: 'externref',
            initial: 1,
            maximum: 2,
        });
        const first = new WebAssembly.Instance(module, { m: { table } });
        const second = new WebAssembly.Instance(module, { m: { table } });

        if (table.grow(1, 'tail') !== 1 ||
            table.length !== 2 ||
            table.get(1) !== 'tail' ||
            first.exports.size() !== 2 ||
            second.exports.size() !== 2) {
            failures.push('wrapper-table-grow-liveness');
        }

        const wasmTable = new WebAssembly.Table({
            element: 'externref',
            initial: 1,
            maximum: 2,
        });
        const wasmFirst =
            new WebAssembly.Instance(module, { m: { table: wasmTable } });
        const wasmSecond =
            new WebAssembly.Instance(module, { m: { table: wasmTable } });
        if (wasmSecond.exports.grow(null) !== 1 ||
            wasmTable.length !== 2 ||
            wasmFirst.exports.size() !== 2 ||
            wasmSecond.exports.size() !== 2) {
            failures.push('wasm-table-grow-liveness');
        }
        if (wasmSecond.exports.grow(null) !== -1 ||
            wasmTable.length !== 2) {
            failures.push('imported-table-native-maximum');
        }
        return failures;
    },

    'exported-funcref-table'() {
        const instance = new WebAssembly.Instance(
            new WebAssembly.Module(exportedFuncrefTableModuleBytes),
        );
        const table = instance.exports.tbl;
        const failures = [];
        if (table.length !== 4 ||
            table.get(0) !== instance.exports.add ||
            table.get(0)(20, 22) !== 42) {
            failures.push('exported-populated-funcref-table');
        }

        const other = new WebAssembly.Instance(
            new WebAssembly.Module(exportedFuncrefTableModuleBytes),
        );
        for (const [ operation, callback ] of [
            [ 'set', () => table.set(0, other.exports.add) ],
            [ 'grow', () => table.grow(1, other.exports.add) ],
        ]) {
            let rejected = false;
            try {
                callback();
            } catch (error) {
                rejected =
                    error instanceof TypeError &&
                    error.message.includes('Cross-instance');
            }
            if (!rejected) {
                failures.push(`cross-instance-funcref-${operation}`);
            }
        }
        if (table.get(0) !== instance.exports.add ||
            table.get(0)(20, 22) !== 42) {
            failures.push('cross-instance-funcref-mutated-table');
        }
        return failures;
    },

    'exported-global-reimport'() {
        const failures = [];
        const exporter = new WebAssembly.Instance(
            new WebAssembly.Module(exportedGlobalModuleBytes),
        );

        const mutable = exporter.exports.g_i32_mut;
        const mutableImporter = new WebAssembly.Instance(
            new WebAssembly.Module(mutableGlobalModuleBytes),
            { m: { global: mutable } },
        );
        if (mutable.value !== 42 || mutableImporter.exports.get() !== 42) {
            failures.push('exported-mutable-global-reimport-value');
        }
        exporter.exports.set_i32_mut(77);
        if (mutable.value !== 77 || mutableImporter.exports.get() !== 77) {
            failures.push('exported-global-to-importer-liveness');
        }
        mutableImporter.exports.set(88);
        if (mutable.value !== 88 ||
            exporter.exports.get_i32_mut() !== 88) {
            failures.push('reimported-global-to-exporter-liveness');
        }

        const immutable = exporter.exports.g_i32_const;
        const immutableImporter = new WebAssembly.Instance(
            new WebAssembly.Module(immutableGlobalModuleBytes),
            { m: { global: immutable } },
        );
        if (immutable.value !== 100 || immutableImporter.exports.get() !== 100) {
            failures.push('exported-immutable-global-reimport-value');
        }
        return failures;
    },

    'exported-memory-reimport'() {
        const exporter = new WebAssembly.Instance(
            new WebAssembly.Module(exportedMemoryModuleBytes),
        );
        const memory = exporter.exports.memory;

        /* memoryGrowModuleBytes imports min=1,max=2; change max to 10. */
        const importerBytes = memoryGrowModuleBytes.slice();
        importerBytes[38] = 10;
        const importer = new WebAssembly.Instance(
            new WebAssembly.Module(importerBytes),
            { m: { memory } },
        );
        return importer.exports.grow() === 1 &&
            memory.buffer.byteLength === 2 * 65536 &&
            exporter.exports.mem_size() === 2 &&
            importer.exports.loadTail() === 0 ?
            [] : [ 'exported-memory-maximum-reimport' ];
    },
};

export function createWasmEdgeApp(caseName) {
    return {
        async fetch() {
            const failures = [];
            try {
                const result = cases[caseName]();
                failures.push(...result);
            } catch (error) {
                failures.push(
                    `${caseName}:${error?.constructor?.name ?? 'Error'}:` +
                    `${error?.message ?? error}`,
                );
            }
            return new Response(JSON.stringify({
                profile: 'CAPSID-MIN-2025-subset-v0',
                passed: failures.length === 0,
                failures,
            }), {
                headers: { 'content-type': 'application/json' },
            });
        },
    };
}
