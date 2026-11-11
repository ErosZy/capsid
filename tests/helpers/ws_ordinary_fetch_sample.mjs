import fs from 'node:fs';
import {ordinaryFetchInputPaths, ordinaryFetchShards} from '../../tools/workerd-ordinary-fetch-contract.mjs';
import {readLock} from '../../tools/workerd-reference.mjs';

// Frozen real reference observations exercise validators. Product copies built
// here are synthetic controls and never qualify a live backend.
export function readOrdinaryFetchSamples(lock = readLock()) {
    const archive = JSON.parse(fs.readFileSync(new URL('../fixtures/workerd-ordinary-fetch-report.json', import.meta.url), 'utf8'));
    return ordinaryFetchShards.map(shard => {
        const report = structuredClone(archive.reports[shard]);
        report.schema = 'capsid-ordinary-fetch-v1'; report.shard = shard;
        report.referenceLock = structuredClone(lock);
        report.binaries = {host: lock.binary_sha256, worker: null};
        report.client = {node: lock.client.node_version, undici: lock.client.undici_version};
        report.inputs = Object.fromEntries(Object.keys(ordinaryFetchInputPaths).map((name, index) => [name, String(index + 1).repeat(64)]));
        // Historical view and native-body recorders omitted a held flag. The
        // formal finite-source recorder emits false; retain the raw archives.
        // This transformation creates synthetic validator controls only.
        for (const value of report.cases) if (shard === 'views' || shard === 'bodies' && value.id.startsWith('ordinary-native-shape-'))
            for (const entries of [value.beforeAction, value.upstream]) for (const entry of entries) entry.held = false;
        return report;
    });
}
