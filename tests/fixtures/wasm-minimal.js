const addModuleBytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01,
    0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01,
    0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09,
    0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a,
    0x0b,
]);

const importedResourcesModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 2, 48, 3, 4, 104, 111, 115, 116,
    6, 109, 101, 109, 111, 114, 121, 2, 1, 1, 2, 4, 104, 111, 115,
    116, 5, 116, 97, 98, 108, 101, 1, 112, 1, 1, 2, 4, 104, 111,
    115, 116, 6, 103, 108, 111, 98, 97, 108, 3, 127, 1, 7, 27, 3,
    6, 109, 101, 109, 111, 114, 121, 2, 0, 5, 116, 97, 98, 108,
    101, 1, 0, 6, 103, 108, 111, 98, 97, 108, 3, 0,
]);

/*
 * Imports a function as m.f and exports call(), which returns m.f(). Reusing
 * one compiled Module with two import objects must keep the instances
 * independent.
 */
const importedFunctionModuleBytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x02, 0x07, 0x01, 0x01, 0x6d, 0x01, 0x66, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 0x63, 0x61, 0x6c, 0x6c, 0x00, 0x01,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x10, 0x00, 0x0b,
]);

/*
 * Imports m.memory, exports it, and exposes i32 store/load operations at byte
 * offset zero. Two live instances importing the same Memory must observe one
 * shared backing store, not snapshots captured at instantiation time.
 */
const sharedImportedMemoryModuleBytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x09, 0x02,
    0x60, 0x01, 0x7f, 0x00,
    0x60, 0x00, 0x01, 0x7f,
    0x02, 0x0e, 0x01,
    0x01, 0x6d,
    0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79,
    0x02, 0x01, 0x01, 0x02,
    0x03, 0x03, 0x02, 0x00, 0x01,
    0x07, 0x19, 0x03,
    0x05, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x00, 0x00,
    0x04, 0x6c, 0x6f, 0x61, 0x64, 0x00, 0x01,
    0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00,
    0x0a, 0x13, 0x02,
    0x09, 0x00, 0x41, 0x00, 0x20, 0x00, 0x36, 0x02, 0x00, 0x0b,
    0x07, 0x00, 0x41, 0x00, 0x28, 0x02, 0x00, 0x0b,
]);

/*
 * Imports a mutable i32 Global and exposes set/get operations. As with Memory,
 * importing the same object into two live instances must retain live aliasing.
 */
const sharedImportedGlobalModuleBytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x09, 0x02,
    0x60, 0x01, 0x7f, 0x00,
    0x60, 0x00, 0x01, 0x7f,
    0x02, 0x0d, 0x01,
    0x01, 0x6d,
    0x06, 0x67, 0x6c, 0x6f, 0x62, 0x61, 0x6c,
    0x03, 0x7f, 0x01,
    0x03, 0x03, 0x02, 0x00, 0x01,
    0x07, 0x16, 0x03,
    0x03, 0x73, 0x65, 0x74, 0x00, 0x00,
    0x03, 0x67, 0x65, 0x74, 0x00, 0x01,
    0x06, 0x67, 0x6c, 0x6f, 0x62, 0x61, 0x6c, 0x03, 0x00,
    0x0a, 0x0d, 0x02,
    0x06, 0x00, 0x20, 0x00, 0x24, 0x00, 0x0b,
    0x04, 0x00, 0x23, 0x00, 0x0b,
]);

/*
 * Imports an externref Table and exposes indexed set/get operations. The same
 * Table object imported by multiple live instances must remain one table.
 */
const sharedImportedTableModuleBytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0b, 0x02,
    0x60, 0x02, 0x7f, 0x6f, 0x00,
    0x60, 0x01, 0x7f, 0x01, 0x6f,
    0x02, 0x0e, 0x01,
    0x01, 0x6d,
    0x05, 0x74, 0x61, 0x62, 0x6c, 0x65,
    0x01, 0x6f, 0x01, 0x01, 0x02,
    0x03, 0x03, 0x02, 0x00, 0x01,
    0x07, 0x15, 0x03,
    0x03, 0x73, 0x65, 0x74, 0x00, 0x00,
    0x03, 0x67, 0x65, 0x74, 0x00, 0x01,
    0x05, 0x74, 0x61, 0x62, 0x6c, 0x65, 0x01, 0x00,
    0x0a, 0x11, 0x02,
    0x08, 0x00, 0x20, 0x00, 0x20, 0x01, 0x26, 0x00, 0x0b,
    0x06, 0x00, 0x20, 0x00, 0x25, 0x00, 0x0b,
]);

/* Imports host.memory with limits min=1, max=2 and re-exports it. */
const importedMemoryMaxTwoModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0,
    2, 17, 1, 4, 104, 111, 115, 116, 6, 109, 101, 109, 111, 114, 121,
    2, 1, 1, 2,
    7, 10, 1, 6, 109, 101, 109, 111, 114, 121, 2, 0,
]);

/* Imports host.memory with min=1 and no declared maximum, then re-exports it. */
const importedMemoryNoMaximumModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0,
    2, 16, 1, 4, 104, 111, 115, 116, 6, 109, 101, 109, 111, 114, 121,
    2, 0, 1,
    7, 10, 1, 6, 109, 101, 109, 111, 114, 121, 2, 0,
]);

const trapModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0,
    1, 4, 1, 96, 0, 0,
    3, 2, 1, 0,
    7, 8, 1, 4, 116, 114, 97, 112, 0, 0,
    10, 5, 1, 3, 0, 0, 11,
]);

const simdTypeModuleBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0,
    1, 5, 1, 96, 0, 1, 123,
]);

/*
 * Embedder resource caps, mirrored from CAPSID_WASM_MAX_MEMORY_PAGES and
 * CAPSID_WASM_MAX_TABLE_ELEMENTS in the wasm polyfill. The
 * `wasm_resource_limit_constants` CTest case fails the build if these drift
 * apart, so the boundary assertions below cannot silently stop testing the
 * boundary.
 */
const MAX_MEMORY_PAGES = 256;
const MAX_TABLE_ELEMENTS = 1024;

async function run() {
    const failures = [];
    const check = (condition, name) => {
        if (!condition) {
            failures.push(name);
        }
    };
    const checkThrows = (constructor, fn, name) => {
        try {
            fn();
            failures.push(`${name}:did-not-throw`);
        } catch (error) {
            if (!(error instanceof constructor)) {
                failures.push(`${name}:${error?.constructor?.name ?? error}`);
            }
        }
    };
    const checkRejects = async (constructor, promise, name) => {
        try {
            await promise;
            failures.push(`${name}:did-not-reject`);
        } catch (error) {
            if (!(error instanceof constructor)) {
                failures.push(`${name}:${error?.constructor?.name ?? error}`);
            }
        }
    };

    check(WebAssembly.validate(addModuleBytes), 'validate-valid');
    check(!WebAssembly.validate(new Uint8Array([ 0x00, 0x61 ])),
        'validate-invalid');

    const compiled = await WebAssembly.compile(addModuleBytes);
    check(compiled instanceof WebAssembly.Module, 'compile-module');
    check(WebAssembly.Module.imports(compiled).length === 0,
        'module-imports');
    check(WebAssembly.Module.exports(compiled).some(
        entry => entry.name === 'add' && entry.kind === 'function'),
    'module-exports');

    const instantiated = await WebAssembly.instantiate(addModuleBytes);
    check(instantiated.module instanceof WebAssembly.Module,
        'instantiate-module');
    check(instantiated.instance instanceof WebAssembly.Instance,
        'instantiate-instance');
    check(instantiated.instance.exports.add(20, 22) === 42,
        'exported-function');

    const streamedResponse = () => new Response(
        new ReadableStream({
            start(controller) {
                controller.enqueue(addModuleBytes.slice(0, 20));
                controller.enqueue(addModuleBytes.slice(20));
                controller.close();
            },
        }),
        { headers: { 'content-type': 'application/wasm' } },
    );
    const streamedModule =
        await WebAssembly.compileStreaming(streamedResponse());
    check(streamedModule instanceof WebAssembly.Module,
        'compile-streaming-module');
    const streamedInstance =
        await WebAssembly.instantiateStreaming(streamedResponse());
    check(streamedInstance.instance.exports.add(19, 23) === 42,
        'instantiate-streaming-function');
    await checkRejects(
        TypeError,
        WebAssembly.compileStreaming(new Response(addModuleBytes)),
        'compile-streaming-requires-wasm-mime',
    );

    const memory = new WebAssembly.Memory({ initial: 1, maximum: 2 });
    check(memory.buffer.byteLength === 65536, 'memory-initial');
    check(memory.grow(1) === 1, 'memory-grow-result');
    check(memory.buffer.byteLength === 131072, 'memory-grown-size');

    const global = new WebAssembly.Global(
        { value: 'i32', mutable: true },
        7,
    );
    check(global.value === 7, 'global-initial');
    global.value = 9;
    check(global.value === 9 && global.valueOf() === 9, 'global-mutable');

    const table = new WebAssembly.Table({
        element: 'externref',
        initial: 2,
        maximum: 3,
    });
    check(table.length === 2, 'table-initial');
    const marker = { ok: true };
    table.set(0, marker);
    check(table.get(0) === marker, 'table-get-set');
    check(table.grow(1, 'tail') === 2, 'table-grow-result');
    check(table.length === 3 && table.get(2) === 'tail',
        'table-grown-value');
    checkThrows(RangeError, () => memory.grow(1),
        'memory-grow-maximum');
    checkThrows(RangeError, () => table.grow(1),
        'table-grow-maximum');

    const importedMemory =
        new WebAssembly.Memory({ initial: 1, maximum: 2 });
    new Uint8Array(importedMemory.buffer)[0] = 42;
    const importedTable = new WebAssembly.Table({
        element: 'anyfunc',
        initial: 1,
        maximum: 2,
    });
    const importedGlobal = new WebAssembly.Global(
        { value: 'i32', mutable: true },
        7,
    );
    const imported = await WebAssembly.instantiate(
        importedResourcesModuleBytes,
        {
            host: {
                memory: importedMemory,
                table: importedTable,
                global: importedGlobal,
            },
        },
    );
    check(imported.instance.exports.memory === importedMemory,
        'imported-memory-identity');
    check(imported.instance.exports.table === importedTable,
        'imported-table-identity');
    check(imported.instance.exports.global === importedGlobal,
        'imported-global-identity');
    importedGlobal.value = 11;
    check(imported.instance.exports.global.value === 11,
        'imported-global-live-value');
    check(new Uint8Array(imported.instance.exports.memory.buffer)[0] === 42,
        'imported-memory-content');

    const reusableModule =
        new WebAssembly.Module(importedFunctionModuleBytes);
    const firstImportedFunctionInstance = new WebAssembly.Instance(
        reusableModule,
        { m: { f: () => 1 } },
    );
    const secondImportedFunctionInstance = new WebAssembly.Instance(
        reusableModule,
        { m: { f: () => 2 } },
    );
    check(firstImportedFunctionInstance.exports.call() === 1,
        'module-first-instance-import');
    check(secondImportedFunctionInstance.exports.call() === 2,
        'module-imports-must-be-instance-local');

    const sharedMemoryModule =
        new WebAssembly.Module(sharedImportedMemoryModuleBytes);
    const sharedMemory =
        new WebAssembly.Memory({ initial: 1, maximum: 2 });
    const firstSharedMemoryInstance = new WebAssembly.Instance(
        sharedMemoryModule,
        { m: { memory: sharedMemory } },
    );
    const secondSharedMemoryInstance = new WebAssembly.Instance(
        sharedMemoryModule,
        { m: { memory: sharedMemory } },
    );
    firstSharedMemoryInstance.exports.store(37);
    check(secondSharedMemoryInstance.exports.load() === 37,
        'shared imported Memory is live in the second instance');
    check(new Uint8Array(sharedMemory.buffer)[0] === 37,
        'shared imported Memory wrapper observes first-instance writes');
    secondSharedMemoryInstance.exports.store(73);
    check(firstSharedMemoryInstance.exports.load() === 73,
        'shared imported Memory is live in the first instance');

    const sharedGlobalModule =
        new WebAssembly.Module(sharedImportedGlobalModuleBytes);
    const sharedGlobal = new WebAssembly.Global(
        { value: 'i32', mutable: true },
        0,
    );
    const firstSharedGlobalInstance = new WebAssembly.Instance(
        sharedGlobalModule,
        { m: { global: sharedGlobal } },
    );
    const secondSharedGlobalInstance = new WebAssembly.Instance(
        sharedGlobalModule,
        { m: { global: sharedGlobal } },
    );
    firstSharedGlobalInstance.exports.set(11);
    check(secondSharedGlobalInstance.exports.get() === 11,
        'shared imported Global is live in the second instance');
    check(sharedGlobal.value === 11,
        'shared imported Global wrapper observes first-instance writes');
    secondSharedGlobalInstance.exports.set(29);
    check(firstSharedGlobalInstance.exports.get() === 29,
        'shared imported Global is live in the first instance');

    const sharedTableModule =
        new WebAssembly.Module(sharedImportedTableModuleBytes);
    const sharedTable = new WebAssembly.Table({
        element: 'externref',
        initial: 1,
        maximum: 2,
    });
    const firstSharedTableInstance = new WebAssembly.Instance(
        sharedTableModule,
        { m: { table: sharedTable } },
    );
    const secondSharedTableInstance = new WebAssembly.Instance(
        sharedTableModule,
        { m: { table: sharedTable } },
    );
    const firstTableMarker = { instance: 1 };
    firstSharedTableInstance.exports.set(0, firstTableMarker);
    check(secondSharedTableInstance.exports.get(0) === firstTableMarker,
        'shared imported Table is live in the second instance');
    check(sharedTable.get(0) === firstTableMarker,
        'shared imported Table wrapper observes first-instance writes');
    const secondTableMarker = { instance: 2 };
    secondSharedTableInstance.exports.set(0, secondTableMarker);
    check(firstSharedTableInstance.exports.get(0) === secondTableMarker,
        'shared imported Table is live in the first instance');

    checkThrows(
        WebAssembly.LinkError,
        () => new WebAssembly.Instance(
            new WebAssembly.Module(importedMemoryMaxTwoModuleBytes),
            {
                host: {
                    memory: new WebAssembly.Memory({
                        initial: 1,
                        maximum: 3,
                    }),
                },
            },
        ),
        'imported-memory-maximum-must-not-exceed-module-maximum',
    );

    const noMaximumImportedMemory =
        new WebAssembly.Memory({ initial: 1 });
    try {
        const noMaximumImportedInstance = new WebAssembly.Instance(
            new WebAssembly.Module(importedMemoryNoMaximumModuleBytes),
            { host: { memory: noMaximumImportedMemory } },
        );
        check(
            noMaximumImportedInstance.exports.memory ===
                noMaximumImportedMemory,
            'imported-memory-without-maximum-identity',
        );
    } catch (error) {
        failures.push(
            'imported-memory-without-maximum-must-link:' +
            `${error?.constructor?.name ?? error}`,
        );
    }

    await checkRejects(
        WebAssembly.CompileError,
        WebAssembly.compile(new Uint8Array([ 0, 97 ])),
        'compile-error',
    );
    checkThrows(
        WebAssembly.LinkError,
        () => new WebAssembly.Instance(
            new WebAssembly.Module(importedResourcesModuleBytes),
            {
                host: {
                    memory: {},
                    table: importedTable,
                    global: importedGlobal,
                },
            },
        ),
        'link-error',
    );
    const trapInstance =
        (await WebAssembly.instantiate(trapModuleBytes)).instance;
    checkThrows(WebAssembly.RuntimeError, () => trapInstance.exports.trap(),
        'runtime-error');

    checkThrows(
        RangeError,
        () => new WebAssembly.Memory({ initial: MAX_MEMORY_PAGES + 1 }),
        'memory-resource-limit',
    );
    checkThrows(
        RangeError,
        () => new WebAssembly.Table({
            element: 'externref',
            initial: MAX_TABLE_ELEMENTS + 1,
        }),
        'table-resource-limit',
    );

    /*
     * The runtime cap must also be enforced through `maximum` and through
     * grow(), not only through `initial`. Without these, a module can declare a
     * conforming initial size and then exceed the embedder's memory budget at
     * run time, which is the case the cap exists to prevent.
     */
    checkThrows(
        RangeError,
        () => new WebAssembly.Memory({ initial: 1, maximum: MAX_MEMORY_PAGES + 1 }),
        'memory-resource-limit-maximum',
    );
    checkThrows(
        RangeError,
        () => new WebAssembly.Table({
            element: 'externref',
            initial: 1,
            maximum: MAX_TABLE_ELEMENTS + 1,
        }),
        'table-resource-limit-maximum',
    );

    /*
     * grow() must refuse to cross the runtime page cap even when the declared
     * maximum would allow it. Uses a small initial size so the test stays well
     * inside the worker JS heap limit: what matters is that the requested total
     * exceeds the cap, not that the memory is actually near it.
     */
    const cappedMemory = new WebAssembly.Memory({ initial: 1, maximum: MAX_MEMORY_PAGES });
    check(cappedMemory.grow(1) === 1, 'memory-grow-within-cap');
    checkThrows(
        RangeError,
        () => cappedMemory.grow(MAX_MEMORY_PAGES),
        'memory-grow-past-cap',
    );

    const cappedTable = new WebAssembly.Table({
        element: 'externref',
        initial: 1023,
        maximum: 1024,
    });
    check(cappedTable.grow(1) === 1023, 'table-grow-to-cap');
    checkThrows(
        RangeError,
        () => cappedTable.grow(1),
        'table-grow-past-cap',
    );

    check(typeof WebAssembly.Tag === 'undefined' &&
          typeof WebAssembly.Exception === 'undefined' &&
          typeof WebAssembly.JSTag === 'undefined',
    'capsid-d001-exception-api-absent');
    check(!WebAssembly.validate(simdTypeModuleBytes),
        'capsid-d002-simd-rejected');

    return {
        profile: 'CAPSID-MIN-2025-subset-v0',
        passed: failures.length === 0,
        failures,
    };
}

export default {
    async fetch() {
        try {
            return new Response(JSON.stringify(await run()), {
                headers: { 'content-type': 'application/json' },
            });
        } catch (error) {
            return new Response(JSON.stringify({
                profile: 'CAPSID-MIN-2025-subset-v0',
                passed: false,
                failures: [ error?.stack ?? String(error) ],
            }), {
                headers: { 'content-type': 'application/json' },
            });
        }
    },
};
