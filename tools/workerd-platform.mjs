import assert from 'node:assert/strict';

export function referencePlatform(lock) {
    assert.ok(lock && typeof lock === 'object', 'reference platform lock required');
    if (lock.release_asset === 'workerd-linux-64.gz') return 'linux';
    if (lock.release_asset === 'workerd-windows-64.gz') return 'win32';
    if (lock.release_asset === 'workerd-darwin-64.gz' || lock.release_asset === 'workerd-darwin-arm64.gz') return 'darwin';
    assert.fail('unsupported locked workerd platform asset');
}

export function referenceArchitecture(lock) {
    referencePlatform(lock);
    return lock.release_asset === 'workerd-darwin-arm64.gz' ? 'arm64' : 'x64';
}

export function referenceLockName(platform, architecture) {
    if (platform === 'darwin') return architecture === 'arm64'
        ? 'reference-lock-darwin-arm64.json' : 'reference-lock-darwin-x64.json';
    return platform === 'win32' ? 'reference-lock-windows.json' : 'reference-lock.json';
}

export function referenceBackendModes(lock) {
    return referencePlatform(lock) === 'linux'
        ? ['single-worker', 'static-pool', 'managed'] : ['single-worker', 'static-pool'];
}
