import assert from 'node:assert/strict';

export function referencePlatform(lock) {
    assert.ok(lock && typeof lock === 'object', 'reference platform lock required');
    if (lock.release_asset === 'workerd-linux-64.gz') return 'linux';
    if (lock.release_asset === 'workerd-windows-64.gz') return 'win32';
    assert.fail('unsupported locked workerd platform asset');
}

export function referenceBackendModes(lock) {
    return referencePlatform(lock) === 'linux'
        ? ['single-worker', 'static-pool', 'managed'] : ['single-worker', 'static-pool'];
}
