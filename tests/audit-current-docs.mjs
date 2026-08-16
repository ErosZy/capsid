import assert from "node:assert/strict";
import { access, readFile, readdir } from "node:fs/promises";
import path from "node:path";

const root = path.resolve(process.argv[2] ?? ".");

async function markdownFiles(relativeRoot) {
  const absoluteRoot = path.join(root, relativeRoot);
  const entries = await readdir(absoluteRoot, { withFileTypes: true });
  const result = [];
  for (const entry of entries) {
    if (entry.name === "node_modules") {
      continue;
    }
    const relativePath = path.join(relativeRoot, entry.name);
    if (entry.isDirectory()) {
      result.push(...await markdownFiles(relativePath));
    } else if (entry.isFile() && entry.name.endsWith(".md")) {
      result.push(relativePath);
    }
  }
  return result;
}

const documentPaths = [
  "README.md",
  "CONTRIBUTING.md",
  "SECURITY.md",
  ...await markdownFiles("docs"),
  ...await markdownFiles("examples"),
].map((value) => value.split(path.sep).join("/")).sort();

const documents = new Map();
for (const relativePath of documentPaths) {
  documents.set(
    relativePath,
    // Normalize CRLF checkouts (git autocrlf on Windows) so the frozen
    // fragment matching below is line-ending agnostic.
    (await readFile(path.join(root, relativePath), "utf8")).replace(
      /\r\n/g,
      "\n",
    ),
  );
}

function localLinks(relativePath, content) {
  const links = [];
  for (const match of content.matchAll(/\[[^\]]+\]\(([^)]+)\)/g)) {
    let target = match[1].trim();
    if (
      target.startsWith("http://") ||
      target.startsWith("https://") ||
      target.startsWith("mailto:") ||
      target.startsWith("#")
    ) {
      continue;
    }
    if (target.startsWith("<") && target.endsWith(">")) {
      target = target.slice(1, -1);
    }
    target = decodeURIComponent(target.split("#", 1)[0]);
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
const docsPaths = documentPaths.filter((value) => value.startsWith("docs/"));
const reachable = new Set(["docs/README.md"]);
const pending = ["docs/README.md"];
while (pending.length > 0) {
  const source = pending.pop();
  for (const link of localLinks(source, documents.get(source))) {
    const relativeTarget = path.relative(root, link.resolved)
      .split(path.sep).join("/");
    if (
      relativeTarget.startsWith("docs/") &&
      relativeTarget.endsWith(".md") &&
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
  "docs/README.md does not reach every maintained Markdown document",
);

const capabilityManifest = JSON.parse(
  await readFile(path.join(root, "docs/capability-manifest.json"), "utf8"),
);
const readme = documents.get("README.md");
const architecture = documents.get("docs/architecture.md");
const hostDesign = documents.get("docs/host-technical-design-review.md");
const performanceGuide = documents.get("docs/performance-benchmarks.md");
const testingGuide = documents.get("docs/testing.md");
const capabilityPolicy = documents.get("docs/capability-policy.md");
const modulePermissions = documents.get("docs/module-permissions.md");
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
  "cmake --install build-release",
  "<capsid/runtime.hpp>",
  "When `egress_policy == NULL`, all egress Fetch requests are denied.",
  "`strict_sandbox` is off by default",
  "`tjs:*` modules cannot be enabled through configuration",
  "`CAPSID_EVENT_READY.flags` contains the sandbox features required by the deployment",
]) {
  assert.ok(
    readme.includes(requiredFragment),
    `README.md is missing user-facing configuration guidance: ${requiredFragment}`,
  );
}

for (const [documentName, content, requiredFragments] of [
  [
    "docs/architecture.md",
    architecture,
    [
      "Platform support splits into two independent commitments: native development and production isolation.",
      "The Windows native development toolchain has been delivered since v0.1.2.",
      "The host decides which capabilities it needs and verifies the actual features at READY.",
      "The runtime implements process creation, IPC, termination/reclamation, and OS sandboxing.",
    ],
  ],
  [
    "docs/host-technical-design-review.md",
    hostDesign,
    [
      "not part of the M1 release gate; M1A",
      "M1A + M1B are delivered as one implementation batch",
      "M1A: the benchmark-minimal single-worker data plane",
      "Run the first 15.7 baseline immediately after green",
      "TSan may come after that first baseline",
      "must precede M1C acceptance and the M2 multi-worker implementation",
      "M1C/M1D",
      "call `capsid_worker_fd()` directly",
      "not implemented on Windows machines/hosted runners",
      "seccomp/Landlock bits express different guarantees",
    ],
  ],
  [
    "docs/testing.md",
    documents.get("docs/testing.md"),
    [
      "TSan (M1C gate)",
      "must pass before M1C acceptance",
      "Do not mix with ASan, UBSan, LTO, fuzz, or benchmark runs",
      "Any report from first-party code fails",
      "personality(ADDR_NO_RANDOMIZE)",
      "seccomp=unconfined",
    ],
  ],
  [
    "docs/performance-benchmarks.md",
    performanceGuide,
    [
      "the first round does not wait for request body, streaming, cancel, or timeout to be implemented",
      "Once those contracts land, they must be benchmarked on the same runner",
    ],
  ],
  [
    "docs/testing.md",
    testingGuide,
    [
      "Windows native-dev uses the `windows-latest` hosted runner",
      "Windows cross-compilation, Wine, WSL2, or a Linux container cannot substitute for hosted native Windows execution evidence",
      "All five job classes produce hosted evidence",
    ],
  ],
]) {
  for (const fragment of requiredFragments) {
    assert.ok(
      content.includes(fragment),
      `${documentName} is missing the frozen platform contract: ${fragment}`,
    );
  }
}

const removedDocuments = [
  "docs/host-architecture-plan.md",
  "docs/project-status.md",
  "docs/txiki-upgrade-report.md",
  "docs/txiki-upgrade-report.json",
];
for (const relativePath of removedDocuments) {
  await assert.rejects(
    access(path.join(root, relativePath)),
    `${relativePath} is a removed snapshot or superseded document`,
  );
}

const currentDocs = [...documents.values()].join("\n");
for (const staleFragment of [
  "host-architecture-plan.md",
  "project-status.md",
  "txiki-upgrade-report.md",
  "bench/README.md",
  "](../bench/results/",
  "vue-ssr-sweep-r1-20260730",
  "app-v2.ts",
  "app-v3.ts",
  'add_argument("--rounds"',
  "--rounds 3",
  "其余普通扩展仍 unavailable",
]) {
  assert.ok(
    !currentDocs.includes(staleFragment),
    `current documentation contains stale fragment: ${staleFragment}`,
  );
}

console.log(
  `current docs: ${documentPaths.length} documents, ` +
    `${capabilityManifest.modules.built_and_available.length} modules verified`,
);
