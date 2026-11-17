import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { access, readFile, readdir } from 'node:fs/promises';
import path from 'node:path';
import { archivedQualification } from './documentation_archive.mjs';

const root = path.resolve(process.argv[2] ?? '.');

async function markdownFiles(relativeRoot) {
  const absoluteRoot = path.join(root, relativeRoot);
  const entries = await readdir(absoluteRoot, { withFileTypes: true });
  const result = [];
  for (const entry of entries) {
    if (entry.name === 'node_modules') {
      continue;
    }
    const relativePath = path.join(relativeRoot, entry.name);
    if (entry.isDirectory()) {
      result.push(...(await markdownFiles(relativePath)));
    } else if (entry.isFile() && entry.name.endsWith('.md')) {
      result.push(relativePath);
    }
  }
  return result;
}

const documentPaths = [
  'README.md',
  'ROADMAP.md',
  'CONTRIBUTING.md',
  'SECURITY.md',
  'bench/README.md',
  'bench/SPEC.md',
  'bench/perf-evidence.md',
  'perf-site/README.md',
  ...(await markdownFiles('docs')),
  ...(await markdownFiles('examples')),
]
  .map((value) => value.split(path.sep).join('/'))
  .sort();

const documents = new Map();
let archivedSnapshots = 0;
for (const relativePath of documentPaths) {
  const bytes = await readFile(path.join(root, relativePath));
  if (await archivedQualification(root, relativePath, bytes)) {
    archivedSnapshots += 1;
    continue;
  }
  documents.set(
    relativePath,
    // Normalize CRLF checkouts (git autocrlf on Windows) so the frozen
    // fragment matching below is line-ending agnostic.
    bytes.toString('utf8').replace(/\r\n/g, '\n'),
  );
}

// Mixed current-reference/journal pages were migrated without rewriting the
// captured source. Keep the original frozen contract checks against exact bytes,
// while current pages describe the delivered release rather than old M1 status.
const sourceSnapshots = new Map();
const migration = JSON.parse(
  await readFile(path.join(root, 'docs/history/archives/2026-11-17/index.json'), 'utf8'),
);
assert.equal(migration.schema, 1, 'known historical-document migration schema');
for (const snapshot of migration.snapshots) {
  const target = path.resolve(root, snapshot.path);
  assert.ok(
    target.startsWith(path.join(root, 'docs/history/archives/2026-11-17') + path.sep),
    'historical source stays in its inventoried archive',
  );
  assert.ok(
    documents.has(snapshot.originalPath) && !sourceSnapshots.has(snapshot.originalPath),
    'one historical source per maintained entry page',
  );
  const bytes = await readFile(target);
  assert.equal(bytes.length, snapshot.bytes, 'historical source length is exact');
  assert.equal(
    createHash('sha256').update(bytes).digest('hex'),
    snapshot.sha256,
    'historical source bytes were not rewritten',
  );
  sourceSnapshots.set(snapshot.originalPath, bytes.toString('utf8').replace(/\r\n/g, '\n'));
}
assert.equal(sourceSnapshots.size, 6, 'all six mixed journals retain captured source');

function localLinks(relativePath, content) {
  const links = [];
  for (const match of content.matchAll(/\[[^\]]+\]\(([^)]+)\)/g)) {
    let target = match[1].trim();
    if (
      target.startsWith('http://') ||
      target.startsWith('https://') ||
      target.startsWith('mailto:') ||
      target.startsWith('#')
    ) {
      continue;
    }
    if (target.startsWith('<') && target.endsWith('>')) {
      target = target.slice(1, -1);
    }
    target = decodeURIComponent(target.split('#', 1)[0]);
    if (!target) {
      continue;
    }
    links.push({
      original: match[1],
      resolved: path.resolve(root, path.dirname(relativePath), target),
    });
  }
  return links;
}

for (const [relativePath, content] of documents) {
  for (const link of localLinks(relativePath, content)) {
    await assert.doesNotReject(
      access(link.resolved),
      `${relativePath} has a broken local link: ${link.original}`,
    );
  }
}

// Every maintained docs/*.md file must be reachable from the docs index. This
// permits a small hierarchy (for example the framework index) without allowing
// an unindexed orphan document to accumulate.
const docsPaths = [...documents.keys()].filter((value) => value.startsWith('docs/'));
const reachable = new Set(['docs/README.md']);
const pending = ['docs/README.md'];
while (pending.length > 0) {
  const source = pending.pop();
  for (const link of localLinks(source, documents.get(source))) {
    const relativeTarget = path.relative(root, link.resolved).split(path.sep).join('/');
    if (
      relativeTarget.startsWith('docs/') &&
      relativeTarget.endsWith('.md') &&
      documents.has(relativeTarget) &&
      !reachable.has(relativeTarget)
    ) {
      reachable.add(relativeTarget);
      pending.push(relativeTarget);
    }
  }
}
assert.deepEqual(
  docsPaths.filter((value) => !reachable.has(value)),
  [],
  'docs/README.md does not reach every maintained Markdown document',
);

const capabilityManifest = JSON.parse(
  await readFile(path.join(root, 'docs/capability-manifest.json'), 'utf8'),
);
const readme = documents.get('README.md');
const contributing = documents.get('CONTRIBUTING.md');
const securityPolicy = documents.get('SECURITY.md');
const architecture = documents.get('docs/architecture.md');
const hostDesign = documents.get('docs/host-technical-design-review.md');
const performanceGuide = documents.get('docs/performance-benchmarks.md');
const testingGuide = documents.get('docs/testing.md');
const capabilityPolicy = documents.get('docs/capability-policy.md');
const modulePermissions = documents.get('docs/module-permissions.md');
for (const moduleName of capabilityManifest.modules.built_and_available) {
  assert.ok(
    readme.includes(`\`${moduleName}\``),
    `README.md does not list current module ${moduleName}`,
  );
  assert.ok(
    capabilityPolicy.includes(`\`${moduleName}\``),
    `capability-policy.md does not explain current module ${moduleName}`,
  );
  assert.ok(
    modulePermissions.includes(`\`${moduleName}\``),
    `module-permissions.md does not explain current module ${moduleName}`,
  );
}
for (const permissionName of Object.keys(capabilityManifest.permissions)) {
  assert.ok(
    modulePermissions.includes(`\`${permissionName}\``),
    `module-permissions.md does not explain permission ${permissionName}`,
  );
}

for (const requiredFragment of [
  '> **Stable**: `0.2.2`. **Prerelease**: `0.3.0-beta.2`. Public C ABI v7.',
  'cmake --install build-release',
  '<capsid/runtime.hpp>',
  'When `egress_policy == NULL`, all egress Fetch requests are denied.',
  '`strict_sandbox` is off by default',
  '`tjs:*` modules cannot be enabled through configuration',
  '`CAPSID_EVENT_READY.flags` contains the sandbox features required by the deployment',
]) {
  assert.ok(
    readme.includes(requiredFragment),
    `README.md is missing user-facing configuration guidance: ${requiredFragment}`,
  );
}

assert.ok(
  !readme.includes('v0.2.0-rc.04'),
  'README.md still points users at a release that did not carry install.sh',
);
for (const [documentName, content] of [
  ['CONTRIBUTING.md', contributing],
  ['SECURITY.md', securityPolicy],
]) {
  assert.ok(
    content.includes('`0.2.2`') && content.includes('`0.3.0-beta.2`'),
    `${documentName} must distinguish stable from the experimental prerelease`,
  );
}
assert.ok(
  contributing.includes('examples/elysia-reference'),
  'CONTRIBUTING.md omits the Elysia example dependency install',
);
assert.ok(
  architecture.includes('Elysia 1.4.29'),
  'docs/architecture.md omits the currently validated Elysia version',
);
for (const staleClaim of [
  'The current tree has no benchmark runner',
  'The current tree has no `bench/`',
]) {
  assert.ok(
    !hostDesign.includes(staleClaim),
    `docs/host-technical-design-review.md contains stale current-state claim: ${staleClaim}`,
  );
}

for (const [documentName, content, requiredFragments] of [
  [
    'docs/architecture.md',
    architecture,
    [
      'Platform support splits into two independent commitments: native development and production isolation.',
      'The Windows native development toolchain has been delivered since v0.1.2.',
      'The host decides which capabilities it needs and verifies the actual features at READY.',
      'The runtime implements process creation, IPC, termination/reclamation, and OS sandboxing.',
    ],
  ],
  [
    'docs/host-technical-design-review.md',
    hostDesign,
    [
      'not part of the M1 release gate; M1A',
      'M1A + M1B are delivered as one implementation batch',
      'M1A: the benchmark-minimal single-worker data plane',
      'Run the first 15.7 baseline immediately after green',
      'TSan may come after that first baseline',
      'must precede M1C acceptance and the M2 multi-worker implementation',
      'M1C/M1D',
      'call `capsid_worker_fd()` directly',
      'not implemented on Windows machines/hosted runners',
      'seccomp/Landlock bits express different guarantees',
    ],
  ],
  [
    'docs/testing.md',
    documents.get('docs/testing.md'),
    [
      'TSan (M1C gate)',
      'must pass before M1C acceptance',
      'Do not mix with ASan, UBSan, LTO, fuzz, or benchmark runs',
      'Any report from first-party code fails',
      'personality(ADDR_NO_RANDOMIZE)',
      'seccomp=unconfined',
    ],
  ],
  [
    'docs/performance-benchmarks.md',
    performanceGuide,
    [
      'the first round does not wait for request body, streaming, cancel, or timeout to be implemented',
      'Once those contracts land, they must be benchmarked on the same runner',
    ],
  ],
  [
    'docs/testing.md',
    testingGuide,
    [
      'Windows native-dev uses the `windows-latest` hosted runner',
      'Windows cross-compilation, Wine, WSL2, or a Linux container cannot substitute for hosted native Windows execution evidence',
      'All five job classes produce hosted evidence',
    ],
  ],
]) {
  const contractContent = sourceSnapshots.get(documentName) ?? content;
  for (const fragment of requiredFragments) {
    assert.ok(
      contractContent.includes(fragment),
      `${documentName} is missing the frozen platform contract: ${fragment}`,
    );
  }
}

const removedDocuments = [
  'docs/host-architecture-plan.md',
  'docs/project-status.md',
  'docs/txiki-upgrade-report.md',
  'docs/txiki-upgrade-report.json',
];
for (const relativePath of removedDocuments) {
  await assert.rejects(
    access(path.join(root, relativePath)),
    `${relativePath} is a removed snapshot or superseded document`,
  );
}

const currentDocs = [...documents.values()].join('\n');
for (const staleFragment of [
  'host-architecture-plan.md',
  'project-status.md',
  'txiki-upgrade-report.md',
  'bench/README.md',
  '](../bench/results/',
  'vue-ssr-sweep-r1-20260730',
  'app-v2.ts',
  'app-v3.ts',
  'add_argument("--rounds"',
  '--rounds 3',
  'other ordinary extensions remain unavailable',
]) {
  assert.ok(
    !currentDocs.includes(staleFragment),
    `current documentation contains stale fragment: ${staleFragment}`,
  );
}

const currentStatus = JSON.parse(
  await readFile(path.join(root, 'docs/current-status.json'), 'utf8'),
);
const profileV2 = JSON.parse(
  await readFile(path.join(root, 'tests/wpt/workerd-default-profile-v2.json'), 'utf8'),
);
assert.equal(currentStatus.developmentBranch, 'dev/v0.3.0');
assert.equal(currentStatus.prerelease.tag, 'v0.3.0-beta.2');
assert.equal(currentStatus.runtime.publicAbi, 7);
assert.equal(currentStatus.runtime.defaultWireVersion, 3);
assert.equal(currentStatus.checkout.kind, 'formatting-successor');
assert.equal(
  currentStatus.checkout.releaseQualificationInherited,
  false,
  'formatting does not inherit the old binary qualification',
);
assert.match(currentStatus.runtime.sourceScope, /qualified beta\.2 interface baseline/);
assert.equal(currentStatus.conformance.referenceProfile, profileV2.id);
assert.deepEqual(
  currentStatus.conformance.rawFailingFiles.map((value) => value.path),
  profileV2.conflicts.map((value) => value.path),
);
assert.equal(currentStatus.qualification.ordinarySuitePassed, false);
assert.equal(currentStatus.qualification.standardsConformanceClaimed, false);
assert.equal(
  currentStatus.performance.publishedBetaPerformanceMeasured,
  false,
  'the older measured snapshot is not relabeled as beta.2',
);
assert.ok(documents.get('docs/conformance.md').includes('workerd-default-profile-v2.json'));
assert.ok(documents.get('docs/host-integration.md').includes('capsid_worker_channel_confirm'));

const coverage = JSON.parse(
  await readFile(path.join(root, 'docs/documentation-coverage.json'), 'utf8'),
);
assert.equal(coverage.schema, 1);
const coveredDocs = coverage.documents.filter(
  (value) => value.path.startsWith('docs/') && value.path.endsWith('.md'),
);
assert.deepEqual(
  coveredDocs.map((value) => value.path).sort(),
  documentPaths.filter((value) => value.startsWith('docs/')).sort(),
  'every docs page has a review disposition',
);
for (const document of coverage.documents) {
  assert.ok(
    [
      'current-reference',
      'historical-plan-or-journal',
      'immutable-evidence',
      'release-history',
    ].includes(document.kind),
  );
  assert.equal(document.reviewed, true, `${document.path}: review is incomplete`);
  assert.ok(
    ['exact-bytes', 'canonical-lf-text'].includes(document.hashMode),
    `${document.path}: known coverage digest semantics`,
  );
  const bytes = await readFile(path.join(root, document.path));
  const input =
    document.hashMode === 'exact-bytes'
      ? bytes
      : Buffer.from(bytes.toString('utf8').replace(/\r\n/g, '\n'), 'utf8');
  assert.equal(
    createHash('sha256').update(input).digest('hex'),
    document.sha256,
    `${document.path}: coverage data is stale`,
  );
}

console.log(
  `current docs: ${documents.size} documents, ${archivedSnapshots} archived snapshots hash-checked, ` +
    `${capabilityManifest.modules.built_and_available.length} modules verified`,
);
