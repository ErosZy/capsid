import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';

const root = process.argv[2];
const manifestPath = process.argv[3];

if (!root || !manifestPath) {
    throw new Error('expected repository root and capability manifest path');
}

const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
const expected = new Map([
    [ 'capsid:assert', {
        implementation: 'tjs:assert',
        imports: [],
        exports: [ 'default' ],
    } ],
    [ 'capsid:getopts', {
        implementation: 'tjs:getopts',
        imports: [ 'getopts' ],
        exports: [ 'default' ],
    } ],
    [ 'capsid:hashing', {
        implementation: 'tjs:hashing',
        imports: [ 'tjs:internal/core' ],
        exports: [ 'SUPPORTED_TYPES', 'createHash' ],
    } ],
    [ 'capsid:ipaddr', {
        implementation: 'tjs:ipaddr',
        imports: [ 'ipaddr.js' ],
        exports: [ 'default' ],
    } ],
    [ 'capsid:utils', {
        implementation: 'tjs:utils',
        imports: [],
        exports: [ 'format', 'inspect' ],
    } ],
    [ 'capsid:uuid', {
        implementation: 'tjs:uuid',
        imports: [ 'uuid' ],
        exports: [ 'default' ],
    } ],
]);

const sorted = values => [ ...values ].sort();
const same = (left, right) =>
    JSON.stringify(sorted(left)) === JSON.stringify(sorted(right));
const fail = message => {
    throw new Error(`utility module manifest: ${message}`);
};

for (const [ specifier, expectedContract ] of expected) {
    if (!manifest.modules.built_and_available.includes(specifier)) {
        fail(`${specifier} is not built_and_available`);
    }
    if (manifest.modules.known_but_not_built.includes(specifier)) {
        fail(`${specifier} is simultaneously marked unavailable`);
    }
    const contract = manifest.module_contracts?.[specifier];
    if (!contract) {
        fail(`${specifier} has no contract`);
    }
    if (!Array.isArray(contract.ambient_authority) ||
        contract.ambient_authority.length !== 0) {
        fail(`${specifier} has ambient authority`);
    }
    if (!same(contract.source_imports, expectedContract.imports) ||
        !same(contract.exports, expectedContract.exports) ||
        contract.implementation_module !==
            expectedContract.implementation) {
        fail(`${specifier} contract differs from the reviewed surface`);
    }

    const sourcePath = path.resolve(root, contract.source);
    if (!sourcePath.startsWith(`${path.resolve(root)}${path.sep}`)) {
        fail(`${specifier} source escapes the repository`);
    }
    const source = fs.readFileSync(sourcePath, 'utf8');
    const sourceImports = [
        ...source.matchAll(
            /\bimport(?:[\s\S]*?\bfrom\s*)?['"]([^'"]+)['"]/g,
        ),
    ].map(match => match[1]);
    if (!same(sourceImports, contract.source_imports)) {
        fail(`${specifier} source imports drifted`);
    }

    const sourceExports = [];
    if (/\bexport\s+default\b/.test(source)) {
        sourceExports.push('default');
    }
    for (const match of source.matchAll(
        /\bexport\s+(?:async\s+)?(?:function|class|const|let|var)\s+(\w+)/g,
    )) {
        sourceExports.push(match[1]);
    }
    for (const match of source.matchAll(/\bexport\s*\{([^}]+)\}/g)) {
        for (const item of match[1].split(',')) {
            const parts = item.trim().split(/\s+as\s+/);
            sourceExports.push(parts[1] || parts[0]);
        }
    }
    if (!same(sourceExports, contract.exports)) {
        fail(`${specifier} source exports drifted`);
    }

    const runtimeImports = contract.runtime_imports;
    if (!Array.isArray(runtimeImports) ||
        runtimeImports.some(item =>
            item !== 'tjs:internal/core' &&
            item !== 'tjs:internal/path')) {
        fail(`${specifier} has an unreviewed runtime import`);
    }
    if (runtimeImports.some(item =>
        !contract.source_imports.includes(item))) {
        fail(`${specifier} runtime import is absent from source imports`);
    }
}

for (const specifier of Object.keys(manifest.module_contracts)) {
    if (!specifier.startsWith('capsid:')) {
        fail(`non-capsid public module contract: ${specifier}`);
    }
}

const environment = manifest.module_contracts?.['capsid:env'];
if (manifest.policy_version !== 2 ||
    !manifest.modules.built_and_available.includes('capsid:env') ||
    manifest.modules.known_but_not_built.includes('capsid:env') ||
    environment?.source !== 'src/worker_runtime.cc' ||
    !same(environment?.source_imports ?? [], []) ||
    !same(environment?.runtime_imports ?? [], []) ||
    !same(environment?.exports ?? [], [ 'env' ]) ||
    !same(
        environment?.ambient_authority ?? [],
        [ 'host-provided-environment-snapshot' ],
    ) ||
    manifest.permissions?.env?.build_state !== 'available' ||
    manifest.permissions?.env?.operation !== 'capsid:env.get') {
    fail('capsid:env snapshot contract drifted');
}

const system = manifest.module_contracts?.['capsid:system'];
if (!manifest.modules.built_and_available.includes('capsid:system') ||
    manifest.modules.known_but_not_built.includes('capsid:system') ||
    system?.source !== 'src/worker_runtime.cc' ||
    !same(system?.source_imports ?? [], []) ||
    !same(system?.runtime_imports ?? [], []) ||
    !same(system?.exports ?? [], [ 'system' ]) ||
    !same(system?.ambient_authority ?? [], []) ||
    manifest.permissions?.sys?.build_state !==
        'partially-available' ||
    manifest.permissions?.sys?.operation !==
        'capsid:system.get' ||
    !same(
        manifest.permissions?.sys?.available_resources ?? [],
        [ 'featureFlags', 'runtimeVersion' ],
    )) {
    fail('capsid:system compile-time metadata contract drifted');
}

const storage = manifest.module_contracts?.['capsid:storage'];
if (!manifest.modules.built_and_available.includes('capsid:storage') ||
    manifest.modules.known_but_not_built.includes('capsid:storage') ||
    storage?.source !== 'src/worker_runtime.cc' ||
    !same(storage?.source_imports ?? [], []) ||
    !same(storage?.runtime_imports ?? [], []) ||
    !same(storage?.exports ?? [], [ 'storage' ]) ||
    !same(storage?.ambient_authority ?? [], []) ||
    manifest.permissions?.storage?.build_state !== 'available' ||
    manifest.permissions?.storage?.operation !==
        'capsid:storage.get/set/delete/clear/keys' ||
    manifest.permissions?.storage?.lifetime !== 'worker' ||
    manifest.permissions?.storage?.namespace_quota_bytes !== 65536 ||
    manifest.permissions?.storage?.namespace_entry_limit !== 256 ||
    manifest.permissions?.storage?.value_limit_bytes !== 16384) {
    fail('capsid:storage worker-local contract drifted');
}

const stdio = manifest.module_contracts?.['capsid:stdio'];
if (!manifest.modules.built_and_available.includes('capsid:stdio') ||
    manifest.modules.known_but_not_built.includes('capsid:stdio') ||
    stdio?.source !== 'src/worker_runtime.cc' ||
    !same(stdio?.source_imports ?? [], []) ||
    !same(stdio?.runtime_imports ?? [], []) ||
    !same(stdio?.exports ?? [], [ 'stdio' ]) ||
    !same(stdio?.ambient_authority ?? [], [ 'host-log-event' ]) ||
    manifest.permissions?.stdio?.build_state !==
        'partially-available' ||
    manifest.permissions?.stdio?.operation !==
        'capsid:stdio.write' ||
    !same(
        manifest.permissions?.stdio?.available_resources ?? [],
        [ 'stderr', 'stdout' ],
    ) ||
    manifest.permissions?.stdio?.message_limit_bytes !== 16384 ||
    manifest.permissions?.stdio?.transport !==
        'bounded-host-log-event') {
    fail('capsid:stdio bounded host-log contract drifted');
}

const filesystem = manifest.module_contracts?.['capsid:fs'];
if (!manifest.modules.built_and_available.includes('capsid:fs') ||
    manifest.modules.known_but_not_built.includes('capsid:fs') ||
    filesystem?.source !== 'src/worker_runtime.cc' ||
    !same(filesystem?.source_imports ?? [], []) ||
    !same(filesystem?.runtime_imports ?? [], []) ||
    !same(filesystem?.exports ?? [], [ 'fs' ]) ||
    !same(
        filesystem?.ambient_authority ?? [],
        [ 'authorized-read-only-host-paths' ],
    ) ||
    manifest.permissions?.read?.build_state !== 'available' ||
    manifest.permissions?.read?.operation !==
        'capsid:fs.readText/stat/list' ||
    manifest.permissions?.read?.file_limit_bytes !== 1048576 ||
    manifest.permissions?.read?.directory_entry_limit !== 1024 ||
    manifest.permissions?.read?.symlink_policy !==
        'deny-all-components' ||
    manifest.permissions?.write?.build_state !== 'unavailable') {
    fail('capsid:fs read-only path contract drifted');
}

const deferredPath = manifest.deferred_modules?.['capsid:path'];
if (!manifest.modules.known_but_not_built.includes('capsid:path') ||
    !deferredPath?.reason.includes('tjs.cwd') ||
    !deferredPath?.required_design.includes('virtual working directory')) {
    fail('capsid:path must remain unavailable until cwd is capability-scoped');
}

const deferredRequirements = new Map([
    [ 'capsid:net', [
        'resolved-address egress hook',
        'hostname and every resolved address',
    ] ],
    [ 'capsid:websocket', [
        'resolved-address egress hook',
        'DNS and redirect',
    ] ],
    [ 'capsid:sqlite', [
        'load extension',
        'extension loading disabled',
    ] ],
    [ 'capsid:readline', [
        'stdin',
        'host-provided input',
    ] ],
]);
for (const [ specifier, fragments ] of deferredRequirements) {
    const deferred = manifest.deferred_modules?.[specifier];
    if (!manifest.modules.known_but_not_built.includes(specifier) ||
        !deferred?.reason.includes(fragments[0]) ||
        !deferred?.required_design.includes(fragments[1])) {
        fail(`${specifier} deferred security design drifted`);
    }
}

const deferredFsWrite =
    manifest.deferred_operations?.['capsid:fs.write'];
const deferredFsWatch =
    manifest.deferred_operations?.['capsid:fs.watch'];
if (!deferredFsWrite?.reason.includes('seccomp') ||
    !deferredFsWrite?.required_design.includes('write-specific Landlock') ||
    !deferredFsWatch?.reason.includes('cross-request') ||
    !deferredFsWatch?.required_design.includes('request ownership')) {
    fail('capsid:fs deferred write/watch design drifted');
}

console.log(`utility module manifest: ${expected.size} contracts verified`);
