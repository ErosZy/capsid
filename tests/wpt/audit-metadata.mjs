import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const deviationPattern = /^\|\s*(CAPSID-D\d+)\s*\|/gm;

const requireString = (value, label) => {
    if (typeof value !== 'string' || value.trim() === '') {
        throw new Error(`${label} must be a non-empty string`);
    }
    return value;
};

export const deviationIds = markdown => {
    const ids = new Set();
    for (const match of markdown.matchAll(deviationPattern)) {
        if (ids.has(match[1])) {
            throw new Error(`duplicate deviation id: ${match[1]}`);
        }
        ids.add(match[1]);
    }
    if (ids.size === 0) {
        throw new Error('no CAPSID-D deviation ids were found');
    }
    return ids;
};

export const auditMetadata = (manifest, deviationsMarkdown) => {
    const deviations = deviationIds(deviationsMarkdown);
    if (!Array.isArray(manifest.executedProfile) ||
        manifest.executedProfile.length === 0) {
        throw new Error('executedProfile must be a non-empty array');
    }
    const executed = new Set(manifest.executedProfile);
    if (executed.size !== manifest.executedProfile.length) {
        throw new Error('executedProfile contains duplicate paths');
    }

    if (!Array.isArray(manifest.expectedFailures) ||
        manifest.expectedFailures.length === 0) {
        throw new Error('expectedFailures must be a non-empty array');
    }
    const expectedKeys = new Set();
    for (const [ index, expected ] of manifest.expectedFailures.entries()) {
        const prefix = `expectedFailures[${index}]`;
        const testPath = requireString(expected.path, `${prefix}.path`);
        const subtest = requireString(expected.subtest, `${prefix}.subtest`);
        const deviation = requireString(
            expected.deviation,
            `${prefix}.deviation`,
        );
        if (!executed.has(testPath)) {
            throw new Error(`${prefix}.path is not in executedProfile: ${testPath}`);
        }
        if (!deviations.has(deviation)) {
            throw new Error(`${prefix} uses unknown deviation id: ${deviation}`);
        }
        const key = `${testPath}\0${subtest}`;
        if (expectedKeys.has(key)) {
            throw new Error(
                `duplicate expected failure for ${testPath}: ${subtest}`,
            );
        }
        expectedKeys.add(key);
    }

    const notExecuted = new Set();
    for (const [ index, group ] of (manifest.tests ?? []).entries()) {
        const excluded = group.notExecuted ?? [];
        if (!Array.isArray(excluded)) {
            throw new Error(`tests[${index}].notExecuted must be an array`);
        }
        if (excluded.length === 0) {
            continue;
        }
        requireString(
            group.notExecutedReason,
            `tests[${index}].notExecutedReason`,
        );
        const gap = requireString(group.gap, `tests[${index}].gap`);
        if (!deviations.has(gap)) {
            throw new Error(`tests[${index}] uses unknown gap id: ${gap}`);
        }
        for (const testPath of excluded) {
            requireString(testPath, `tests[${index}].notExecuted path`);
            if (executed.has(testPath)) {
                throw new Error(
                    `path is both executed and notExecuted: ${testPath}`,
                );
            }
            if (notExecuted.has(testPath)) {
                throw new Error(`duplicate notExecuted path: ${testPath}`);
            }
            notExecuted.add(testPath);
        }
    }

    const auditDeviationMap = (value, label) => {
        if (value === undefined) {
            return;
        }
        if (!value || typeof value !== 'object' || Array.isArray(value)) {
            throw new Error(`${label} must be an object`);
        }
        for (const [ name, deviation ] of Object.entries(value)) {
            requireString(name, `${label} key`);
            requireString(deviation, `${label}.${name}`);
            if (!deviations.has(deviation)) {
                throw new Error(
                    `${label}.${name} uses unknown deviation id: ${deviation}`,
                );
            }
        }
    };
    for (const [ index, group ] of (manifest.tests ?? []).entries()) {
        auditDeviationMap(group.excludedFormats, `tests[${index}].excludedFormats`);
        auditDeviationMap(
            group.excludedFeatures,
            `tests[${index}].excludedFeatures`,
        );
        if (group.gap !== undefined &&
            !deviations.has(requireString(group.gap, `tests[${index}].gap`))) {
            throw new Error(`tests[${index}] uses unknown gap id: ${group.gap}`);
        }
    }

    return {
        deviations: deviations.size,
        expectedFailures: expectedKeys.size,
        notExecuted: notExecuted.size,
    };
};

const main = () => {
    const manifestPath = process.argv[2];
    const deviationsPath = process.argv[3];
    if (!manifestPath || !deviationsPath) {
        throw new Error(
            'usage: audit-metadata.mjs MANIFEST CONFORMANCE_DEVIATIONS',
        );
    }
    const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
    const deviations = fs.readFileSync(deviationsPath, 'utf8');
    const result = auditMetadata(manifest, deviations);
    console.log(
        `WPT metadata valid: ${result.expectedFailures} expected failures, ` +
        `${result.notExecuted} not-executed paths, ` +
        `${result.deviations} registered deviations`,
    );
};

if (process.argv[1] &&
    path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    main();
}
