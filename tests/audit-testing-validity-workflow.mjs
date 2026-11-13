import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import path from "node:path";

const root = path.resolve(process.argv[2] ?? ".");
const workflowPath = path.join(
  root,
  ".github/workflows/testing-validity.yml",
);
const workflow = await readFile(workflowPath, "utf8");
const releaseWorkflow = await readFile(
  path.join(root, ".github/workflows/release.yml"),
  "utf8",
);
assert.match(workflow, /^  workflow_call:\s*$/m, 'validity workflow is reusable by the release gate');
assert.match(releaseWorkflow, /^  validation:\r?\n    uses: \.\/\.github\/workflows\/testing-validity\.yml\s*$/m,
  'release validates the workflow from its own commit');
assert.match(releaseWorkflow, /^  package:\r?\n    needs: validation\s*$/m,
  'packaging and publication depend on successful validation');
assert.match(releaseWorkflow, /ref: \$\{\{ github\.sha \}\}/,
  'package checkout uses the tested event SHA, not a movable tag name');

const expectedActions = new Map([
  ["actions/checkout", "11d5960a326750d5838078e36cf38b85af677262"],
  ["actions/setup-node", "49933ea5288caeca8642d1e84afbd3f7d6820020"],
  ["actions/setup-python", "e797f83bcb11b83ae66e0230d6156d7c80228e7c"],
  ["actions/setup-go", "40f1582b2485089dde7abd97c1529aa768e1baff"],
  ["denoland/setup-deno", "22d081ff2d3a40755e97629de92e3bcbfa7cf2ed"],
  // v1 (v1.13.0): sets up the Visual Studio developer environment in a
  // non-vcvars shell so the Ninja generator can find `cl` (Windows host
  // job only). Must stay on the tag commit: the repo only commits
  // node_modules into release tags, so default-branch revisions fail
  // with "Cannot find module '@actions/core'".
  ["ilammy/msvc-dev-cmd", "0b201ec74fa43914dc39ae48a89fd1d8cb592756"],
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
  'tools/setup-ws-interop-python.py',
  'CAPSID_WS_INTEROP_PYTHON',
  'CAPSID_WS_INTEROP_NODE_PACKAGE',
  'python3-venv',
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
  "runs-on: macos-15",
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
for(const jobName of ['release-wpt-and-delegated-sandbox','sanitizers','posix-host-library']) {
  const job=workflow.split(`  ${jobName}:`)[1]?.split(/\n  [a-z][a-z-]+:\r?\n/)[0];
  assert.ok(job,`required job ${jobName}`);
  for(const fragment of ['tools/setup-ws-interop-python.py','CAPSID_WS_INTEROP_PYTHON',
    'CAPSID_WS_INTEROP_NODE_PACKAGE','tools/workerd-reference.mjs install'])
    assert.ok(job.includes(fragment),`${jobName} must prepare independent clients/reference: ${fragment}`);
}

const sanitizerJob = workflow.split('  sanitizers:')[1]?.split(/\n  [a-z][a-z-]+:\r?\n/)[0];
const reportArchiveStep = sanitizerJob?.split('      - name: Archive sanitizer driver reports preserving original names')[1]?.split(/\r?\n      - name:/)[0];
assert.ok(reportArchiveStep, 'sanitizer raw reports must be archived before upload');
for (const fragment of ['if: always()', 'REPORT_DIRECTORY: build-${{ matrix.name }}/matrix-case-reports',
  "tarfile.open(archive,'w:gz',dereference=False)", 'output.add(report,arcname=report.name)',
  "archive.name+'.sha256'"])
  assert.ok(reportArchiveStep.includes(fragment), `sanitizer archive preserves all raw names: ${fragment}`);
const sanitizerUpload = sanitizerJob?.split('      - name: Save sanitizer evidence')[1];
assert.ok(sanitizerUpload.includes('build-${{ matrix.name }}/matrix-case-reports.tar.gz') &&
  sanitizerUpload.includes('build-${{ matrix.name }}/matrix-case-reports.tar.gz.sha256'),
  'sanitizer upload carries the opaque archive and checksum');
assert.doesNotMatch(sanitizerUpload, /matrix-case-reports\/\s*(?:\r?\n|$)/,
  'raw runtime paths with POSIX colons cannot be submitted directly to upload-artifact');

function sanitizerStep(name) {
  const step = sanitizerJob?.split(`      - name: ${name}`)[1]?.split(/\r?\n      - name:/)[0];
  assert.ok(step, `sanitizer job requires step: ${name}`);
  assert.doesNotMatch(step, /^        (?:if:|continue-on-error:)|\|\|\s*(?:true|:|exit\s+0)/m,
    `sanitizer ${name} cannot skip or ignore failures`);
  return step.split(/\r?\n/).filter(line => !/^\s*#/.test(line)).map(line => line.trim()).join('\n');
}
const providerStep = sanitizerStep('Build OpenSSL 3.5 for sanitizer targets');
assert.match(providerStep, /^set -euo pipefail$/m, 'provider configure/build failures must propagate');
const providerModes = [...providerStep.matchAll(/^(asan|ubsan|tsan)\) sanitizer=([a-z]+) ;;$/gm)]
  .map(match => [match[1], match[2]]);
assert.deepEqual(providerModes, [['asan', 'address'], ['ubsan', 'undefined'], ['tsan', 'thread']],
  'each provider uses the sanitizer selected for its Capsid matrix entry');
assert.ok(providerStep.includes('case "${{ matrix.name }}" in\nasan) sanitizer=address ;;') &&
  providerStep.includes('*) echo "Unknown sanitizer matrix entry" >&2; exit 1 ;;\nesac'),
  'provider mode selection must reject unknown matrix entries');
assert.ok(providerStep.includes('compiler=cc\nif [ "${{ matrix.compiler }}" = "clang" ]; then\ncompiler=clang\nfi'),
  'provider compiler must match the Capsid sanitizer compiler');
assert.ok(providerStep.includes('sanitizer_flags="-fsanitize=${sanitizer} -fno-omit-frame-pointer -fno-sanitize-recover=all"'),
  'provider retains frame pointers and fatal sanitizer diagnostics');
assert.ok(providerStep.includes('CC="${compiler}" CFLAGS="${sanitizer_flags}" CXXFLAGS="${sanitizer_flags}" LDFLAGS="-fsanitize=${sanitizer}" \\\n./config --prefix=/opt/openssl35 --openssldir=/opt/openssl35 no-shared no-tests no-asm\nmake -j2\nsudo make install_sw'),
  'provider configure consumes the compiler and sanitizer flags and builds instrumentable C implementations');
const sanitizerConfigure = sanitizerStep('Configure ${{ matrix.name }}');
assert.ok(sanitizerConfigure.includes('if [ "${{ matrix.compiler }}" = "clang" ]; then\ncompiler_args=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)\nfi'),
  'Capsid and provider select the same Clang compiler');
assert.ok(sanitizerConfigure.includes('"${compiler_args[@]}" "${host_args[@]}"'),
  'Capsid configure consumes its selected compiler and provider prefix');

const releaseJob = workflow.split('  release-wpt-and-delegated-sandbox:')[1]?.split(/\n  [a-z][a-z-]+:\r?\n/)[0];
const nativeProviderPins = JSON.parse(await readFile(
  new URL('../patches/native-providers/manifest.json', import.meta.url), 'utf8'));
function nativeProviderStep(job, name) {
  const step = job?.split(`      - name: ${name}`)[1]?.split(/\r?\n      - name:/)[0];
  assert.ok(step, `native providers require ${name}`);
  assert.doesNotMatch(step, /^        (?:if:|continue-on-error:)|\|\|\s*(?:true|:|exit\s+0)/m,
    `native providers cannot skip or ignore ${name}`);
  return step.split(/\r?\n/).filter(line => !/^\s*#/.test(line)).map(line => line.trim()).join('\n');
}
for (const [job, suffix] of [[releaseJob, 'Release'], [sanitizerJob, 'sanitizer targets']]) {
  const ssl = nativeProviderStep(job, 'Build OpenSSL 3.5 for ' + suffix);
  const cares = nativeProviderStep(job, 'Build c-ares 1.34.5 for ' + suffix);
  for (const [step, provider] of [[ssl, 'openssl'], [cares, 'cares']]) {
    const call = `python3 "\${GITHUB_WORKSPACE}/tools/apply-native-provider-patches.py" \\\n--provider ${provider} --source `;
    assert.ok(step.includes(call), `${suffix} ${provider} applies the reviewed provider corrections`);
    assert.ok(step.indexOf(call) < step.indexOf(provider === 'openssl' ? './config ' : 'cmake -S '),
      `${suffix} ${provider} corrects sources before configure`);
    assert.ok(step.includes(`tee "\${evidence}/${provider}-source.json"`),
      `${suffix} ${provider} retains the exact source preparation report`);
  }
  const pin = nativeProviderPins.providers.cares.source;
  assert.ok(cares.includes('curl -sSL -o capsid-cares.tar.gz ' + pin.url) &&
    cares.includes(`echo "${pin.sha256}  capsid-cares.tar.gz" | sha256sum -c -`) &&
    cares.includes('tar xzf capsid-cares.tar.gz'), `${suffix} c-ares requires the locked upstream archive`);
  assert.ok(cares.includes(`--provider cares --source "\${RUNNER_TEMP}/${pin.root}"`) &&
    cares.includes(`cmake -S "\${RUNNER_TEMP}/${pin.root}"`), `${suffix} c-ares builds the corrected source tree`);
  for (const fragment of ['-DCMAKE_INSTALL_PREFIX=/opt/openssl35',
    '-DCARES_STATIC=ON -DCARES_SHARED=OFF -DCARES_STATIC_PIC=ON',
    'cmake --build "${RUNNER_TEMP}/capsid-cares-build" --parallel 2 --verbose',
    'sudo cmake --install "${RUNNER_TEMP}/capsid-cares-build"',
    '"${evidence}/cares-compile-commands.json"'])
    assert.ok(cares.includes(fragment), `${suffix} c-ares retains its static provider and compilation evidence`);
  assert.ok(job.includes('-DCAPSID_CARES_PROVIDER_DIR=/opt/openssl35/include'),
    `${suffix} Capsid uses the qualified c-ares headers`);
}
const sanitizerCares = nativeProviderStep(sanitizerJob, 'Build c-ares 1.34.5 for sanitizer targets');
for (const fragment of ['-DCMAKE_C_COMPILER="${compiler}"', '-DCMAKE_C_FLAGS="${sanitizer_flags}"',
  '-DCMAKE_C_FLAGS_DEBUG="-g -O1"', '-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=${sanitizer}"'])
  assert.ok(sanitizerCares.includes(fragment), 'c-ares compilation and link consume the selected sanitizer');
assert.equal([...sanitizerCares.matchAll(/-DCMAKE_C_FLAGS(?::[A-Z]+)?=/g)].length, 1,
  'c-ares cannot override its sanitizer flags');
const profileOptions = job => [...job.matchAll(/-DCAPSID_SANDBOX_PROFILE(?::[A-Z]+)?=([^\s\\'"]+)/g)].map(m => m[1]);
assert.deepEqual(profileOptions(releaseJob), ['production'], 'Release explicitly qualifies the production sandbox');
assert.deepEqual(profileOptions(sanitizerJob), ['sanitizer'], 'sanitizers explicitly use the diagnostic sandbox');
assert.deepEqual([...sanitizerJob.matchAll(/^          ASAN_OPTIONS: ([^\r\n]+)\r?$/gm)].map(match => match[1]),
  ['detect_leaks=1:abort_on_error=1:strict_string_checks=1','detect_leaks=1:abort_on_error=1:strict_string_checks=1'],
  'sanitizer qualification keeps leak scanning enabled');
assert.doesNotMatch(sanitizerJob, /detect_leaks=0/, 'sanitizer qualification cannot disable leak scanning');
assert.doesNotMatch(sanitizerJob.split('\n    steps:')[0],
  /worker_strict_sandbox_(?:direct_fetch|https_ca|https_tls12_rsa_pss)/,
  'diagnostic sanitizer matrix cannot exclude the native network checks');
const sanitizerMatrixHeader = sanitizerJob.split(/\r?\n    steps:/)[0].replace(/\r\n/g, '\n');
const sanitizerMatrixEntries = sanitizerMatrixHeader.split('          - name: ').slice(1);
assert.deepEqual(sanitizerMatrixEntries.map(entry => entry.split('\n')[0].trim()), ['asan', 'ubsan', 'tsan'],
  'sanitizer matrix retains all three native profiles');
for (const entry of sanitizerMatrixEntries) {
  const name = entry.split('\n')[0].trim();
  const lines = entry.split('\n').map(line => line.trim()).filter(line => line && !line.startsWith('#'));
  const exclusions = lines.map((line, index) => [line, index]).filter(([line]) => line.startsWith('ctest_exclude:'));
  assert.equal(exclusions.length, 1, `sanitizer ${name} has exactly one exclusion definition`);
  const [line, index] = exclusions[0];
  const value = line.slice('ctest_exclude:'.length).trim();
  const expression = value === '>-' ? lines[index + 1] : value.replace(/^['"]|['"]$/g, '');
  const expectedExclusion = name !== 'ubsan' ? '^(wpt_conformance_not_configured|worker_package_.*|websocket_managed_(replacement-recursion|reclamation-idle))$' : '^(wpt_conformance_not_configured|worker_package_.*)$';
  assert.equal(expression, expectedExclusion,
    `sanitizer ${name} exclusion must retain all native managed checks`);
}
for (const [job, configureName, installName] of [
  [releaseJob, 'Configure Release/LTO with WPT and benchmarks', 'Install build dependencies'],
  [sanitizerJob, 'Configure ${{ matrix.name }}', 'Install build and locked JavaScript dependencies'],
]) {
  const configure = nativeProviderStep(job, configureName);
  assert.ok(configure.includes('-DCAPSID_TEST_OPENSSL_EXECUTABLE=/usr/bin/openssl') &&
    [...configure.matchAll(/-DCAPSID_TEST_OPENSSL_EXECUTABLE=/g)].length === 1,
    'native certificate fixtures bind the installed system CLI independently of instrumented provider libraries');
  const install = nativeProviderStep(job, installName);
  assert.ok(install.includes(' openssl ') && install.includes('/usr/bin/openssl version'),
    'native certificate fixture CLI is installed and probed');
}
for (const jobName of ['sanitizers', 'windows-host-library', 'posix-host-library']) {
  const job = workflow.split(`  ${jobName}:`)[1]?.split(/\n  [a-z][a-z-]+:\r?\n/)[0];
  const budgets = [...job.matchAll(/^    timeout-minutes: (\d+)\r?$/gm)].map(match => Number(match[1]));
  assert.ok(budgets.length === 1 && budgets[0] >= 180 && budgets[0] <= 360,
    `${jobName} has a bounded job budget sufficient for the measured complete matrix and builds`);
}
const posixJob = workflow.split('  posix-host-library:')[1]?.split(/\n  [a-z][a-z-]+:\r?\n/)[0];
function posixStep(name) {
  const step = posixJob?.split(`      - name: ${name}`)[1]?.split(/\r?\n      - name:/)[0];
  assert.ok(step, `macOS requires step: ${name}`);
  assert.doesNotMatch(step, /^        (?:if:|continue-on-error:)|\|\|\s*(?:true|:|exit\s+0)|set\s+\+e/m,
    `macOS cannot skip or ignore ${name}`);
  return step.split(/\r?\n/).filter(line => !/^\s*#/.test(line)).map(line => line.trim()).join('\n');
}
const macNode=posixStep('Set up Node.js'),macPython=posixStep('Set up the locked Python WebSocket runtime');
assert.ok(macNode.includes('uses: actions/setup-node@49933ea5288caeca8642d1e84afbd3f7d6820020') &&
  [...macNode.matchAll(/^node-version: (.+)$/gm)].map(x=>x[1]).join(',')==='24.14.0',
  'macOS uses the locked Node runtime');
assert.ok(macPython.includes('uses: actions/setup-python@e797f83bcb11b83ae66e0230d6156d7c80228e7c') &&
  [...macPython.matchAll(/^python-version: (.+)$/gm)].map(x=>x[1]).join(',')==="'3.12.3'",
  'macOS uses the locked independent Python runtime');
for(const name of ['Set up Node.js','Set up the locked Python WebSocket runtime'])
  assert.ok(posixJob.indexOf('      - name: '+name)>posixJob.indexOf('      - name: Install macOS Host dependencies'),
    'macOS runtime setup follows Homebrew dependency changes');
const macClients=posixStep('Install locked independent WebSocket clients');
assert.ok(macClients.includes('python3 tools/setup-ws-interop-python.py "${RUNNER_TEMP}/capsid-ws-python"') &&
  macClients.includes('"${RUNNER_TEMP}/capsid-ws-python/bin/python" >> "${GITHUB_ENV}"') &&
  macClients.includes('"${RUNNER_TEMP}/capsid-ws-python/node-ws" >> "${GITHUB_ENV}"'),
  'macOS installs and exports the locked independent clients');
const macReference=posixStep('Install and verify locked workerd reference');
assert.ok(macReference.includes('node tools/workerd-reference.mjs install "${reference}"') &&
  macReference.includes('"${reference}" >> "${GITHUB_ENV}"'), 'macOS installs and exports the verified native reference');
const macConfigure=posixStep('Configure macOS host library and worker contracts');
assert.ok(macConfigure.includes("node_binary=$(node -p 'process.execPath')") &&
  macConfigure.includes('-DCAPSID_HOST_TEST_NODE="${node_binary}"') &&
  macConfigure.includes('-DCAPSID_NODE_EXECUTABLE="${node_binary}"'),
  'macOS CTest commands bind the canonical locked Node executable');
assert.ok(posixJob.indexOf('      - name: Install locked independent WebSocket clients')>
  posixJob.indexOf('      - name: Set up the locked Python WebSocket runtime') &&
  posixJob.indexOf('      - name: Install locked independent WebSocket clients')>
  posixJob.indexOf('      - name: Set up Node.js'), 'macOS installs clients after both locked runtimes');
const macUpload=posixJob?.split('      - name: Save macOS evidence')[1];
assert.match(macUpload, /^        if: always\(\)\s*$/m, 'macOS evidence uploads after failure');
assert.match(macUpload, /^          if-no-files-found: error\s*$/m, 'macOS missing evidence fails the upload');
for(const path of ['macos-junit.xml','Testing/Temporary/LastTest.log','CMakeCache.txt'])
  assert.ok(macUpload.includes('            build-macos/'+path), 'macOS evidence upload requires: build-macos/'+path);
for (const [jobName, installName] of [
  ['release-wpt-and-delegated-sandbox', 'Install build dependencies'],
  ['sanitizers', 'Install build and locked JavaScript dependencies'],
  ['fuzz', 'Install Clang and Ninja'],
]) {
  const job = workflow.split(`  ${jobName}:`)[1]?.split(/\n  [a-z][a-z-]+:\r?\n/)[0];
  const install = nativeProviderStep(job, installName);
  const source = "sudo sed -i 's|mirror+file:/etc/apt/apt-mirrors\\.txt|https://archive.ubuntu.com/ubuntu/|g' /etc/apt/sources.list.d/ubuntu.sources";
  const network = 'apt_network=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30)';
  const update = 'sudo apt-get "${apt_network[@]}" update --error-on=any';
  const packages = 'sudo apt-get "${apt_network[@]}" install -y ';
  assert.ok(install.includes(source), `${jobName} uses the observed reachable Ubuntu mirror without changing verification settings`);
  assert.ok(install.includes(network), `${jobName} bounds dependency network retries and timeouts`);
  assert.ok(install.includes(update), `${jobName} rejects incomplete apt indexes`);
  assert.ok(install.includes(packages), `${jobName} applies network bounds to package downloads`);
  assert.ok(install.indexOf(source) < install.indexOf(update) && install.indexOf(update) < install.indexOf(packages),
    `${jobName} updates the selected source before installing packages`);
}
assert.equal([...workflow.matchAll(/\$\{\{ runner\.temp \}\}\/capsid-native-provider-evidence\//g)].length, 2,
  'both Linux jobs upload native provider provenance');
const rawRssStep = sanitizerJob?.split('      - name: Record original ASan RSS results')[1]?.split(/\r?\n      - name:/)[0];
assert.ok(rawRssStep?.includes('id: asan_rss_raw'), 'ASan retains a stable raw RSS step identity');
assert.match(rawRssStep, /^        if: matrix\.name == 'asan'\r?$/m, 'raw RSS observation runs for every ASan entry');
assert.deepEqual([...rawRssStep.matchAll(/^        continue-on-error:\s*([^\r\n]*)\r?$/gm)].map(match=>match[1]), ['true'],
  'raw ASan RSS failure outcome is deferred only to strict separate acceptance');
for (const fragment of ['set -euo pipefail',
  'rm -f build-asan/asan-rss-raw-exit-code.txt build-asan/asan-rss-raw-junit.xml build-asan/asan-rss-raw-LastTest.log',
  'rm -f build-asan/asan-memory-registration.json build-asan/asan-memory-acceptance.json',
  'rm -f build-asan/asan-memory-replacement-recursion.json build-asan/asan-memory-reclamation-idle.json',
  '--show-only=json-v1 > build-asan/asan-memory-registration.json',
  '--output-on-failure','--output-junit asan-rss-raw-junit.xml --parallel 1',
  "-R '^websocket_managed_(replacement-recursion|reclamation-idle)$' || asan_rss_status=$?",
  "printf '%s\\n' \"${asan_rss_status}\" > build-asan/asan-rss-raw-exit-code.txt",
  'cp build-asan/Testing/Temporary/LastTest.log build-asan/asan-rss-raw-LastTest.log','exit "${asan_rss_status}"'])
  assert.ok(rawRssStep.includes(fragment), 'ASan raw RSS preserves actual complete evidence: '+fragment);
assert.equal([...rawRssStep.matchAll(/asan_rss_status=/g)].length,2,'raw ASan RSS exit cannot be reset or manufactured');
assert.doesNotMatch(rawRssStep,/\|\|\s*(?:true|:|exit\s+0)|set\s+\+e/,'raw ASan RSS cannot discard failures');
const memoryStep = sanitizerJob?.split('      - name: Require separate ASan memory acceptance')[1]?.split(/\r?\n      - name:/)[0];
assert.ok(memoryStep?.includes("if: ${{ !cancelled() && matrix.name == 'asan' && steps.asan_rss_raw.outcome != 'skipped' }}"),
  'separate ASan acceptance runs after raw success or failure');
assert.doesNotMatch(memoryStep,/^        continue-on-error:|\|\|\s*(?:true|:|exit\s+0)|set\s+\+e/m,
  'separate ASan acceptance cannot ignore failures');
for (const fragment of ['python3 -B tools/asan-memory-acceptance.py',
  '--registration build-asan/asan-memory-registration.json','--raw-junit build-asan/asan-rss-raw-junit.xml',
  '--raw-last-test build-asan/asan-rss-raw-LastTest.log','--raw-exit-code build-asan/asan-rss-raw-exit-code.txt',
  "--raw-outcome '${{ steps.asan_rss_raw.outcome }}'",'--matrix-junit build-asan/asan-junit.xml',
  '--resource-report-root build-asan','--output build-asan/asan-memory-acceptance.json'])
  assert.ok(memoryStep.includes(fragment),'separate ASan acceptance binds actual inputs: '+fragment);
const tsanRawRssStep = sanitizerJob?.split('      - name: Record original TSan RSS results')[1]?.split(/\r?\n      - name:/)[0];
assert.ok(tsanRawRssStep?.includes('id: tsan_rss_raw'), 'TSan retains a stable raw RSS step identity');
assert.match(tsanRawRssStep, /^        if: matrix\.name == 'tsan'\r?$/m, 'raw RSS observation runs for every TSan entry');
assert.deepEqual([...tsanRawRssStep.matchAll(/^        continue-on-error:\s*([^\r\n]*)\r?$/gm)].map(match=>match[1]), ['true'],
  'raw TSan RSS failure outcome is deferred only to strict separate acceptance');
for (const fragment of ['set -euo pipefail',
  'rm -f build-tsan/tsan-rss-raw-exit-code.txt build-tsan/tsan-rss-raw-junit.xml build-tsan/tsan-rss-raw-LastTest.log',
  'rm -f build-tsan/tsan-memory-registration.json build-tsan/tsan-memory-acceptance.json',
  'rm -f build-tsan/tsan-memory-replacement-recursion.json build-tsan/tsan-memory-reclamation-idle.json',
  '--show-only=json-v1 > build-tsan/tsan-memory-registration.json',
  '--output-on-failure','--output-junit tsan-rss-raw-junit.xml --parallel 1',
  "-R '^websocket_managed_(replacement-recursion|reclamation-idle)$' || tsan_rss_status=$?",
  "printf '%s\\n' \"${tsan_rss_status}\" > build-tsan/tsan-rss-raw-exit-code.txt",
  'cp build-tsan/Testing/Temporary/LastTest.log build-tsan/tsan-rss-raw-LastTest.log','exit "${tsan_rss_status}"'])
  assert.ok(tsanRawRssStep.includes(fragment), 'TSan raw RSS preserves actual complete evidence: '+fragment);
assert.equal([...tsanRawRssStep.matchAll(/tsan_rss_status=/g)].length,2,'raw TSan RSS exit cannot be reset or manufactured');
assert.doesNotMatch(tsanRawRssStep,/\|\|\s*(?:true|:|exit\s+0)|set\s+\+e/,'raw TSan RSS cannot discard failures');
const tsanMemoryStep = sanitizerJob?.split('      - name: Require separate TSan memory acceptance')[1]?.split(/\r?\n      - name:/)[0];
assert.ok(tsanMemoryStep?.includes("if: ${{ !cancelled() && matrix.name == 'tsan' && steps.tsan_rss_raw.outcome != 'skipped' }}"),
  'separate TSan acceptance runs after raw success or failure');
assert.doesNotMatch(tsanMemoryStep,/^        continue-on-error:|\|\|\s*(?:true|:|exit\s+0)|set\s+\+e/m,
  'separate TSan acceptance cannot ignore failures');
for (const fragment of ['python3 -B tools/asan-memory-acceptance.py --sanitizer tsan',
  '--registration build-tsan/tsan-memory-registration.json','--raw-junit build-tsan/tsan-rss-raw-junit.xml',
  '--raw-last-test build-tsan/tsan-rss-raw-LastTest.log','--raw-exit-code build-tsan/tsan-rss-raw-exit-code.txt',
  "--raw-outcome '${{ steps.tsan_rss_raw.outcome }}'",'--matrix-junit build-tsan/tsan-junit.xml',
  '--resource-report-root build-tsan','--output build-tsan/tsan-memory-acceptance.json'])
  assert.ok(tsanMemoryStep.includes(fragment),'separate TSan acceptance binds actual inputs: '+fragment);
assert.match(tsanRawRssStep, /^          TSAN_OPTIONS: halt_on_error=1:exitcode=66\r?$/m, 'raw TSan RSS preserves fatal race diagnostics');
const selectedSanitizerStep = sanitizerStep('Run ${{ matrix.name }} matrix');
assert.ok(selectedSanitizerStep.includes("-E '${{ matrix.ctest_exclude }}'"),'sanitizer matrix uses its reviewed selection');
assert.doesNotMatch(selectedSanitizerStep,/(?:^|\s)(?:-R\S*|-L\S*|-I\S*|--(?:include-regex|label-regex|tests-information|tests-from-file|exclude-from-file))(?=\s|=|$)/,
  'sanitizer matrix cannot hide other native checks');
for (const path of ['build-asan/asan-rss-raw-junit.xml','build-asan/asan-rss-raw-LastTest.log','build-asan/asan-memory-*.json'])
  assert.ok(sanitizerJob.includes('            '+path),'ASan acceptance uploads '+path);
for (const path of ['build-tsan/tsan-rss-raw-junit.xml','build-tsan/tsan-rss-raw-LastTest.log','build-tsan/tsan-rss-raw-exit-code.txt','build-tsan/tsan-memory-*.json'])
  assert.ok(sanitizerJob.includes('            '+path),'TSan acceptance uploads '+path);
const ordinaryStep = releaseJob?.match(/      - name: Run the complete ordinary-host matrix\r?\n([\s\S]*?)(?=\n      - name:|$)/)?.[1];
assert.ok(ordinaryStep?.includes('id: ordinary_host'), 'ordinary full suite has a stable result identity');
assert.doesNotMatch(ordinaryStep, /^        if:/m, 'ordinary full suite cannot be skipped');
assert.deepEqual([...ordinaryStep.matchAll(/^        continue-on-error:\s*([^\r\n]*)\r?$/gm)].map(match=>match[1]), ['true'],
  'only the raw ordinary step defers its conclusion to mandatory compatibility acceptance');
assert.doesNotMatch(ordinaryStep, /\|\|\s*(?:true|:|exit\s+0)|set\s+\+e/m,
  'ordinary full suite failures must retain their raw exit code');
for(const fragment of ['rm -f build-validity/ordinary-host-exit-code.txt','ordinary_ctest_status=0',
  '--parallel 2 || ordinary_ctest_status=$?',
  'printf \'%s\\n\' "${ordinary_ctest_status}" > build-validity/ordinary-host-exit-code.txt',
  'exit "${ordinary_ctest_status}"'])assert.ok(ordinaryStep.includes(fragment),'ordinary raw exit is recorded and propagated: '+fragment);
assert.equal([...ordinaryStep.matchAll(/ordinary_ctest_status=/g)].length,2,'raw exit cannot be reset or manufactured');
assert.doesNotMatch(ordinaryStep, /(?:^|\s)(?:-[RELI]\S*|--(?:include-regex|exclude-regex|label-regex|label-exclude|tests-information|tests-from-file|exclude-from-file))(?=\s|=|$)/,
  'ordinary full suite cannot filter standard WPT failures');
assert.ok(ordinaryStep.includes('ordinary-host-registration.json'), 'ordinary suite preserves its complete registration');
const profileStep = releaseJob?.match(/      - name: Audit the versioned workerd profile without changing standard results\r?\n([\s\S]*?)(?=\n      - name:|$)/)?.[1];
assert.ok(profileStep, 'versioned workerd profile audit is required');
assert.ok(profileStep.includes("if: ${{ !cancelled() && steps.ordinary_host.outcome != 'skipped' }}"),
  'profile evidence is retained after ordinary success or failure');
assert.doesNotMatch(profileStep, /^        continue-on-error:|\|\|\s*(?:true|:|exit\s+0)|set\s+\+e/m,
  'complete compatibility acceptance cannot ignore failures');
for (const fragment of ['cp build-validity/Testing/Temporary/LastTest.log',
  'cp build-validity/ordinary-host-junit.xml', 'cp build-validity/ordinary-host-exit-code.txt',
  'node tools/workerd-wpt-acceptance.mjs audit',
  '--ctest-exit-code "${evidence}/ordinary-exit-code.txt"',
  "--ordinary-outcome '${{ steps.ordinary_host.outcome }}'",
  'node tools/run_workerd_wpt_context.mjs', '--output "${evidence}/context"',
  '--context "${evidence}/context/context.json"',
  '--registration build-validity/ordinary-host-registration.json', '--gate build-validity/workerd-ws-evidence',
  '--output "${evidence}/profile-audit.json"',
  '--acceptance-output "${evidence}/compatibility-acceptance.json"'])
  assert.ok(profileStep.includes(fragment), `profile audit preserves and binds actual evidence: ${fragment}`);
assert.ok(workflow.includes('            build-validity/workerd-wpt-profile-evidence/'), 'profile evidence is uploaded');
const upgradeReportStep=releaseJob.split('      - name: Generate txiki.js upgrade gate report')[1]?.split(/\r?\n      - name:/)[0];
const upgradeReportBody=upgradeReportStep?.split(/        run: \|\r?\n/)[1]?.split(/\r?\n/)
  .filter(line=>line.trim()&&!/^\s*#/.test(line)).map(line=>line.trim()).join('\n');
const expectedUpgradeReportBody=[
  'set -euo pipefail',
  'evidence=build-validity/workerd-wpt-profile-evidence',
  'cmp build-validity/ordinary-host-junit.xml "${evidence}/ordinary-junit.xml"',
  'python3 tools/generate-txiki-upgrade-report.py \\',
  '--source-root . \\', '--build-dir build-validity \\',
  '--junit "${evidence}/ordinary-junit.xml" \\',
  '--baseline docs/txiki-upgrade-baseline.json \\',
  '--node-executable "$(node -p \'process.execPath\')" \\',
  '--workerd-python "${CAPSID_WS_INTEROP_PYTHON}" \\',
  '--workerd-acceptance "${evidence}/compatibility-acceptance.json" \\',
  '--workerd-last-test "${evidence}/ordinary-LastTest.log" \\',
  '--workerd-registration build-validity/ordinary-host-registration.json \\',
  '--workerd-gate build-validity/workerd-ws-evidence \\',
  '--workerd-context "${evidence}/context/context.json" \\',
  '--workerd-wpt-root "${RUNNER_TEMP}/wpt" \\',
  '--workerd-ctest-exit-code "${evidence}/ordinary-exit-code.txt" \\',
  '--output-json build-validity/txiki-upgrade-report.json \\',
  '--output-md build-validity/txiki-upgrade-report.md',
].join('\n');
assert.equal(upgradeReportBody,expectedUpgradeReportBody,
  'upgrade report replays complete compatibility evidence and retains the identical raw JUnit');

// The ordinary WPT step keeps its raw failure. All independent downstream
// checks must still execute after that failure, and must stop on cancellation
// or when prerequisites prevented the ordinary matrix from starting.
const postOrdinarySteps = [
  'Run the privileged sandbox matrix',
  'Record package evidence (build ID, compatibility ID, package and SBOM hashes)',
  'Generate txiki.js upgrade gate report',
  'Require delegated cgroup v2 and network namespace passes',
];
const postOrdinaryCondition = "${{ !cancelled() && steps.ordinary_host.outcome != 'skipped' }}";
for (const name of postOrdinarySteps) {
  const marker = `      - name: ${name}`;
  const step = releaseJob?.split(marker)[1]?.split(/\r?\n      - name:/)[0];
  assert.ok(step, `Release requires independent step: ${name}`);
  assert.ok(releaseJob.indexOf(marker) > releaseJob.indexOf('      - name: Run the complete ordinary-host matrix'),
    `${name} must follow the ordinary matrix`);
  const conditions = [...step.matchAll(/^        if:\s*([^\r\n]*)\r?$/gm)].map(match => match[1]);
  assert.deepEqual(conditions, [postOrdinaryCondition],
    `${name} runs after ordinary success or failure`);
  assert.doesNotMatch(step, /^        continue-on-error:|\|\|\s*(?:true|:|exit\s+0)|set\s+\+e/m,
    `${name} cannot ignore failures`);
}
const delegatedStep=releaseJob.split('      - name: Require delegated cgroup v2 and network namespace passes')[1]?.split(/\r?\n      - name:/)[0];
assert.ok(delegatedStep.includes('--entrypoint /bin/bash \\') &&
  delegatedStep.includes('/repo/scripts/run-delegated-sandbox-tests.sh /build'),
  'delegated container invokes the reviewed script through its shell');
const archiveStep=releaseJob.split('      - name: Archive workerd execution evidence')[1]?.split(/\r?\n      - name:/)[0];
assert.ok(archiveStep && /^        if: always\(\)\r?$/m.test(archiveStep), 'workerd evidence archive runs after failures');
assert.doesNotMatch(archiveStep,/^        continue-on-error:/m,'workerd evidence archive cannot ignore failure');
const archiveBody=archiveStep.split(/run: \|\r?\n/)[1]?.split(/\r?\n/).map(line=>line.trim()).filter(line=>line&&!line.startsWith('#')).join('\n');
assert.equal(archiveBody, 'set -euo pipefail\nif [ -d build-validity/workerd-ws-evidence ]; then\n'+
  'tar -czf build-validity/workerd-ws-evidence.tar.gz -C build-validity workerd-ws-evidence\n'+
  'sha256sum build-validity/workerd-ws-evidence.tar.gz > build-validity/workerd-ws-evidence.tar.gz.sha256\nfi',
  'workerd evidence archive preserves the complete original directory and digest');
const releaseUpload=releaseJob.split('      - name: Save hosted Release evidence')[1];
assert.ok(/^            build-validity\/workerd-ws-evidence\.tar\.gz\r?$/m.test(releaseUpload) &&
  /^            build-validity\/workerd-ws-evidence\.tar\.gz\.sha256\r?$/m.test(releaseUpload) &&
  !/^            build-validity\/workerd-ws-evidence\/\r?$/m.test(releaseUpload),
  'workerd evidence upload uses the lossless archive and digest');
assert.equal([...releaseJob.matchAll(/^        continue-on-error:/gm)].length,1,
  'no other Release gate can ignore its failure');

// CTest succeeds by default when a filter selects no tests. Check each
// continued shell command, rather than accepting a flag in some other job.
const lines = workflow.split(/\r?\n/);
const ctestCommands = [];
for (let index = 0; index < lines.length; index++) {
  if (!/^\s*(?:sudo\s+)?ctest\s/.test(lines[index])) continue;
  const lineNumber = index + 1;
  let command = lines[index];
  while (/[\\`]\s*$/.test(lines[index])) {
    assert.ok(index + 1 < lines.length, 'complete continued CTest command');
    command += '\n' + lines[++index];
  }
  assert.match(command, /(?:^|\s)--no-tests=error(?=\s|$)/,
    `CTest command at line ${lineNumber} must fail when no tests are selected`);
  ctestCommands.push(command);
}
assert.ok(ctestCommands.length > 0, 'workflow must run CTest');

const windowsJob = workflow.split('  windows-host-library:')[1]?.split(/\n  [a-z][a-z-]+:\r?\n/)[0];
assert.ok(windowsJob, 'required Windows native job');
assert.match(windowsJob, /^\s*python-version: '3\.12\.3'\s*$/m, 'Windows job prepares the locked Python runtime');
for (const fragment of ['tools/setup-ws-interop-python.py', 'CAPSID_WS_INTEROP_PYTHON', 'CAPSID_WS_INTEROP_NODE_PACKAGE',
  'openssl_fixture version', 'GITHUB_PATH'])
  assert.ok(windowsJob.includes(fragment), `Windows job prepares independent clients: ${fragment}`);
for (const [option, expected] of [
  ['CAPSID_BUILD_HOST', 'ON'], ['CAPSID_BUILD_WORKER', 'ON'],
  ['CAPSID_STRICT_WARNINGS', 'ON'], ['CAPSID_USE_MIMALLOC', 'OFF'],
  ['CMAKE_BUILD_TYPE', 'Release'], ['CMAKE_C_COMPILER', 'cl'], ['CMAKE_CXX_COMPILER', 'cl'],
  ['VCPKG_TARGET_TRIPLET', 'x64-windows-static'],
]) {
  const values = [...windowsJob.matchAll(new RegExp(`^\\s*-D${option}(?::[A-Z]+)?=([^\\s\\x60\\\\]+)`, 'gm'))]
    .map(match => match[1]);
  assert.deepEqual(values, [expected], `Windows configure must set exactly one ${option}=${expected}`);
}
assert.match(windowsJob, /^\s*ctest --test-dir build-windows /m, 'Windows native job runs its configured CTest tree');
function windowsStep(name) {
  const marker = `      - name: ${name}`;
  const step = windowsJob.split(marker)[1]?.split(/\r?\n      - name:/)[0];
  assert.ok(step, `Windows native job requires step: ${name}`);
  return step;
}
const windowsReference = windowsStep('Install the locked native Windows workerd reference');
const commandLines = step => new Set(step.split(/\r?\n/).map(line => line.trim()));
const windowsReferenceLines = commandLines(windowsReference);
for (const fragment of [
  "$reference_path = Join-Path $env:RUNNER_TEMP 'capsid-workerd-reference/workerd.exe'",
  'node tools/workerd-reference.mjs install $reference_path',
  'if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }',
  '"CAPSID_WORKERD_REF=$reference_path" |', '"CAPSID_WORKERD_REQUIRED=1" |',
  'Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append',
]) assert.ok(windowsReferenceLines.has(fragment), `Windows native reference installer requires: ${fragment}`);
const windowsGate = windowsStep('Run the native Windows workerd compatibility gate');
const windowsGateLines = commandLines(windowsGate);
for (const fragment of [
  'node tools/run-workerd-ws-gate.mjs build-windows build-windows/workerd-compatibility',
  'if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }',
]) assert.ok(windowsGateLines.has(fragment), `Windows native reference gate requires: ${fragment}`);
const windowsMatrix = windowsStep('Run Windows host-side matrix');
const windowsMatrixLines = commandLines(windowsMatrix);
for (const [step, label] of [[windowsReference, 'reference installer'], [windowsGate, 'reference gate'], [windowsMatrix, 'complete matrix']]) {
  assert.doesNotMatch(step, /^        (?:if:|continue-on-error:)|\|\|\s*(?:true|:|exit\s+0)/m,
    `Windows ${label} cannot skip or ignore failures`);
}
assert.ok(windowsJob.indexOf('Install the locked native Windows workerd reference') < windowsJob.indexOf('Run the native Windows workerd compatibility gate') &&
  windowsJob.indexOf('Run the native Windows workerd compatibility gate') < windowsJob.indexOf('Run Windows host-side matrix'),
  'Windows native reference installation, gate and complete matrix run in order');
assert.ok(windowsMatrixLines.has('$evidence_root = (Resolve-Path build-windows).Path'),
  'Windows native evidence root resolves the configured build tree');
assert.ok(windowsMatrixLines.has("$junit_path = Join-Path $evidence_root 'windows-junit.xml'"),
  'Windows native job retains JUnit evidence at the uploaded absolute path');
assert.match(windowsMatrix, /^\s*--output-junit \$junit_path\s*`$/m,
  'Windows native job retains JUnit evidence using the resolved path');
assert.ok(windowsMatrixLines.has("$ctest_log = Join-Path $evidence_root 'ctest.log'") &&
  windowsMatrixLines.has('2>&1 | Tee-Object -FilePath $ctest_log'),
  'Windows native job retains the complete CTest log at the uploaded absolute path');
assert.match(windowsMatrix, /2>&1 \| Tee-Object -FilePath \$ctest_log\r?\n\s+\$ctest_exit = \$LASTEXITCODE\r?\n\s+if \(\$ctest_exit -ne 0\) \{[\s\S]*?\r?\n\s+exit \$ctest_exit\r?\n\s+\}/,
  'Windows complete matrix propagates the actual CTest failure');
const windowsUpload = windowsStep('Save Windows evidence');
assert.match(windowsUpload, /^        if: always\(\)\s*$/m, 'Windows evidence uploads after failure');
assert.match(windowsUpload, /^          if-no-files-found: error\s*$/m, 'Windows missing evidence fails the upload');
for (const fragment of [
  '            build-windows/windows-junit.xml', '            build-windows/ctest.log',
  '            build-windows/Testing/Temporary/LastTest.log', '            build-windows/workerd-compatibility/',
]) assert.ok(windowsUpload.includes(fragment), `Windows evidence upload requires: ${fragment.trim()}`);
const windowsCommands = ctestCommands.filter(command => /^\s*ctest --test-dir build-windows(?:\s|$)/.test(command));
assert.equal(windowsCommands.length, 1, 'Windows native job runs one complete CTest profile');
const windowsCommand = windowsCommands[0];
const windowsExclusions = [...windowsCommand.matchAll(/(?:^|\s)(?:-E|--exclude-regex)(?:\s+|=)(['"])(.*?)\1/g)]
  .map(match => match[2]);
const windowsExcludeOptions = [...windowsCommand.matchAll(/(?:^|\s)(?:-E\S*|--exclude-regex(?:=\S*)?)(?=\s|$)/g)];
assert.equal(windowsExcludeOptions.length, windowsExclusions.length,
  'Windows CTest cannot add an unreviewed or unquoted exclusion');
assert.deepEqual(windowsExclusions, ['^wpt_conformance_not_configured$'],
  'Windows CTest must execute package contents, metadata, smoke and reproducibility; only unconfigured WPT may be excluded');
assert.doesNotMatch(windowsCommand,
  /(?:^|\s)(?:-[RLI]|--(?:include-regex|label-regex|label-exclude|exclude-label|tests-information|tests-from-file|exclude-from-file)(?=\s|=|$))/,
  'Windows native profile cannot hide required tests behind another selection filter');

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

assert.doesNotMatch(
  releaseWorkflow,
  /release_flags=\(\)/,
  "release workflow uses an empty array that fails under macOS Bash 3.2 nounset",
);
for (const fragment of ["create_release()", "edit_release()", "--prerelease"]) {
  assert.ok(
    releaseWorkflow.includes(fragment),
    `release workflow lost the stable/prerelease branch: ${fragment}`,
  );
}

console.log(
  `testing-validity workflow: ${uses.length} action uses and ` +
    `${requiredFragments.length} required fragments; ` +
    `${ctestCommands.length} CTest empty-selection guards verified; Windows package profile required`,
);
