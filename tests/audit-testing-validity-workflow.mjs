import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import path from "node:path";

const root = path.resolve(process.argv[2] ?? ".");
const workflowPath = path.join(
  root,
  ".github/workflows/testing-validity.yml",
);
const workflow = await readFile(workflowPath, "utf8");

const expectedActions = new Map([
  ["actions/checkout", "11d5960a326750d5838078e36cf38b85af677262"],
  ["actions/setup-node", "49933ea5288caeca8642d1e84afbd3f7d6820020"],
  ["actions/setup-go", "40f1582b2485089dde7abd97c1529aa768e1baff"],
  ["denoland/setup-deno", "22d081ff2d3a40755e97629de92e3bcbfa7cf2ed"],
  // v1: sets up the Visual Studio developer environment in a non-vcvars
  // shell so the Ninja generator can find `cl` (Windows host job only).
  ["ilammy/msvc-dev-cmd", "460a772e4cf7358f9f2f23773240813e40e7a894"],
  // v6.0.0 (node24 runtime): the reviewed revision after the Node 20
  // deprecation migration (upload b7c566a, download 018cc2c).
  ["actions/upload-artifact", "b7c566a772e6b6bfb58ed0dc250532a479d7789f"],
  ["actions/download-artifact", "018cc2cf5baa6db3ef3c5f8a56943fffe632ef53"],
]);

const uses = [
  ...workflow.matchAll(/^\s*uses:\s*([^@\s]+)@([^\s#]+)/gm),
].map((match) => ({
  action: match[1],
  revision: match[2],
}));
assert.ok(uses.length > 0, "workflow contains no third-party actions");
for (const { action, revision } of uses) {
  assert.match(
    revision,
    /^[0-9a-f]{40}$/,
    `${action} must be pinned to an immutable 40-character commit SHA`,
  );
  assert.equal(
    revision,
    expectedActions.get(action),
    `${action} is not pinned to the reviewed revision`,
  );
}
assert.deepEqual(
  new Set(uses.map(({ action }) => action)),
  new Set(expectedActions.keys()),
  "workflow action inventory drifted",
);

const requiredFragments = [
  "submodules: recursive",
  "-DCMAKE_BUILD_TYPE=Release",
  "-DCAPSID_ENABLE_LTO=ON",
  "Build OpenSSL 3.5 for Release",
  "Build OpenSSL 3.5 for sanitizer targets",
  "-DCMAKE_PREFIX_PATH=/opt/openssl35",
  "-DCAPSID_WPT_ROOT=\"${WPT_DIRECTORY}\"",
  "--output-junit ordinary-host-junit.xml",
  '"${{ matrix.name }}-junit.xml"',
  "--output-junit fuzz-junit.xml",
  "--output-junit macos-junit.xml",
  "tools/generate-txiki-upgrade-report.py",
  "--privileged",
  "--cgroupns=private",
  "scripts/run-delegated-sandbox-tests.sh",
  "CAPSID_ENABLE_ASAN",
  "CAPSID_ENABLE_UBSAN",
  "-DCAPSID_BUILD_FUZZERS=ON",
  "-L fuzz",
  "runs-on: macos-14",
  "-DCAPSID_BUILD_WORKER=OFF",
  "hosted-evidence/run.txt",
  "hosted-evidence/sha256sums.txt",
  "hosted-evidence/evidence-tree.sha256",
  'test "${RELEASE_RESULT}" = success',
  'test "${SANITIZER_RESULT}" = success',
  'test "${FUZZ_RESULT}" = success',
  'test "${MACOS_RESULT}" = success',
];
for (const fragment of requiredFragments) {
  assert.ok(
    workflow.includes(fragment),
    `testing-validity workflow lost required gate: ${fragment}`,
  );
}

assert.doesNotMatch(
  workflow,
  /^\s*uses:\s*[^@\s]+@(main|master|v\d+)\s*$/gm,
  "workflow contains a movable action revision",
);
assert.doesNotMatch(
  workflow,
  /--output-junit\s+(?:"?)build-/g,
  "CTest --test-dir already roots relative JUnit output inside the build tree",
);

console.log(
  `testing-validity workflow: ${uses.length} action uses and ` +
    `${requiredFragments.length} security gates verified`,
);
