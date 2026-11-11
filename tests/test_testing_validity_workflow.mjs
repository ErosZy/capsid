import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';

// These private workflow copies exercise the actual auditor. No Hosted jobs
// are submitted or counted as executed by these controls.
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const auditor = path.join(root, 'tests/audit-testing-validity-workflow.mjs');
const rawWorkflow = fs.readFileSync(path.join(root, '.github/workflows/testing-validity.yml'), 'utf8');
// Construct faults on one representation, then serialize every actual audit
// input with the selected line ending. A CRLF checkout must not make a
// multiline mutation a no-op before the auditor is exercised.
const workflow = rawWorkflow.replace(/\r\n/g, '\n');
const release = fs.readFileSync(path.join(root, '.github/workflows/release.yml'), 'utf8').replace(/\r\n/g, '\n');
const controlEol = process.env.CAPSID_WORKFLOW_CONTROL_EOL ?? (rawWorkflow.includes('\r\n') ? 'crlf' : 'lf');
assert.ok(['lf', 'crlf'].includes(controlEol), 'known workflow control line ending');
const serialize = (value, eol) => eol === 'crlf' ? value.replace(/\r\n/g, '\n').replace(/\n/g, '\r\n') : value.replace(/\r\n/g, '\n');
function run(source, eol = controlEol) {
    const temporaryRoot = fs.realpathSync(os.tmpdir());
    const directory = fs.mkdtempSync(path.join(temporaryRoot, 'capsid-workflow-control-'));
    assert.ok(path.isAbsolute(directory) && path.basename(directory).startsWith('capsid-workflow-control-'));
    assert.equal(path.dirname(fs.realpathSync(directory)), temporaryRoot);
    try {
        fs.mkdirSync(path.join(directory, '.github/workflows'), {recursive: true});
        fs.writeFileSync(path.join(directory, '.github/workflows/testing-validity.yml'), serialize(source, eol));
        fs.writeFileSync(path.join(directory, '.github/workflows/release.yml'), serialize(release, eol));
        const result = spawnSync(process.execPath, [auditor, directory],
            {encoding: 'utf8', timeout: 10000, windowsHide: true});
        assert.ifError(result.error);
        return {status: result.status, output: `${result.stdout}\n${result.stderr}`};
    } finally {
        fs.rmSync(directory, {recursive: true, force: true});
    }
}
const baseline = run(workflow);
assert.equal(baseline.status, 0, `healthy audit must pass before any fault is credited:\n${baseline.output}`);
test('healthy complete workflow passes the actual audit', () => {
    assert.match(baseline.output, /Windows package profile required/);
});
test('the opposite line ending preserves the same complete healthy audit', () => {
    const other = run(workflow, controlEol === 'crlf' ? 'lf' : 'crlf');
    assert.equal(other.status, 0, other.output);
    assert.match(other.output, /Windows package profile required/);
});

for (const [name, before, after, diagnostic] of [
    ['ordinary raw result not deferred to strict acceptance', '        continue-on-error: true\n        run: |\n          rm -f build-validity/ordinary-host-exit-code.txt', '        run: |\n          rm -f build-validity/ordinary-host-exit-code.txt', 'only the raw ordinary step defers its conclusion'],
    ['ordinary exit capture removed', '--parallel 2 || ordinary_ctest_status=$?', '--parallel 2 || true', 'ordinary full suite failures must retain their raw exit code'],
    ['ordinary exit fabricated', 'ordinary_ctest_status=0', 'ordinary_ctest_status=8', 'ordinary raw exit is recorded and propagated'],
    ['ordinary exit reset', 'exit "${ordinary_ctest_status}"', 'ordinary_ctest_status=0\n          exit "${ordinary_ctest_status}"', 'raw exit cannot be reset or manufactured'],
    ['ordinary outcome suppressed', 'exit "${ordinary_ctest_status}"', 'exit 0', 'ordinary raw exit is recorded and propagated'],
    ['ordinary stale exit retained', 'rm -f build-validity/ordinary-host-exit-code.txt', 'echo stale-exit-allowed', 'ordinary raw exit is recorded and propagated'],
    ['ordinary registration lost', 'id: ordinary_host', 'id: missing_identity', 'ordinary full suite has a stable result identity'],
    ['ordinary suite skipped', 'id: ordinary_host', 'id: ordinary_host\n        if: false', 'ordinary full suite cannot be skipped'],
    ['profile audit skipped on ordinary failure', "if: ${{ !cancelled() && steps.ordinary_host.outcome != 'skipped' }}", 'if: success()', 'profile evidence is retained after ordinary success or failure'],
    ['full LastTest archive lost', 'cp build-validity/Testing/Temporary/LastTest.log', 'echo ignored-full-log', 'profile audit preserves and binds actual evidence'],
    ['profile command lost', 'node tools/workerd-wpt-acceptance.mjs audit', 'node tools/removed-audit.mjs audit', 'profile audit preserves and binds actual evidence'],
    ['raw exit archive lost', 'cp build-validity/ordinary-host-exit-code.txt', 'echo removed-exit-archive', 'profile audit preserves and binds actual evidence'],
    ['raw exit binding lost', '--ctest-exit-code "${evidence}/ordinary-exit-code.txt"', '--ctest-exit-code "${evidence}/wrong.txt"', 'profile audit preserves and binds actual evidence'],
    ['step conclusion substituted for outcome', "--ordinary-outcome '${{ steps.ordinary_host.outcome }}'", "--ordinary-outcome '${{ steps.ordinary_host.conclusion }}'", 'profile audit preserves and binds actual evidence'],
    ['compatibility result archive lost', '--acceptance-output "${evidence}/compatibility-acceptance.json"', '--acceptance-output "${evidence}/missing.json"', 'profile audit preserves and binds actual evidence'],
    ['compatibility gate failure ignored', "      - name: Audit the versioned workerd profile without changing standard results\n", "      - name: Audit the versioned workerd profile without changing standard results\n        continue-on-error: true\n", 'complete compatibility acceptance cannot ignore failures'],
    ['complete context execution lost', 'node tools/run_workerd_wpt_context.mjs', 'node tools/removed-context.mjs', 'profile audit preserves and binds actual evidence'],
    ['complete context archive lost', '--output "${evidence}/context"', '--output "${evidence}/lost-context"', 'profile audit preserves and binds actual evidence'],
    ['complete context binding lost', '--context "${evidence}/context/context.json"', '--context "${evidence}/wrong.json"', 'profile audit preserves and binds actual evidence'],
    ['profile upload lost', '            build-validity/workerd-wpt-profile-evidence/', '            build-validity/missing-profile/', 'profile evidence is uploaded'],
    ['ordinary WPT exclusion', '--output-junit ordinary-host-junit.xml', "-E '^worker_wpt_' --output-junit ordinary-host-junit.xml", 'ordinary full suite cannot filter standard WPT failures'],
]) fault(`profile workflow rejects ${name}`, workflow.replace(before, after), diagnostic);

for (const name of [
    'Run the privileged sandbox matrix',
    'Record package evidence (build ID, compatibility ID, package and SBOM hashes)',
    'Generate txiki.js upgrade gate report',
    'Require delegated cgroup v2 and network namespace passes',
]) {
    const heading = `      - name: ${name}\n`;
    const guard = "        if: ${{ !cancelled() && steps.ordinary_host.outcome != 'skipped' }}\n";
    const healthy = heading + guard;
    assert.ok(workflow.includes(healthy), `post-ordinary step has its execution guard: ${name}`);
    for (const [kind, replacement] of [
        ['implicit success', ''],
        ['explicit success', '        if: success()\n'],
        ['never run', '        if: false\n'],
        ['implicit status in ordinary outcome check', "        if: ${{ steps.ordinary_host.outcome != 'skipped' }}\n"],
        ['missing ordinary prerequisite', '        if: ${{ !cancelled() }}\n'],
    ]) {
        fault(`${name} rejects ${kind}`, workflow.replace(healthy, heading + replacement),
            `${name} runs after ordinary success or failure`);
    }
    fault(`${name} rejects ignored failure`, workflow.replace(healthy, healthy + '        continue-on-error: true\n'),
        `${name} cannot ignore failures`);
}

function windows(change) {
    const start = workflow.indexOf('  windows-host-library:');
    assert.ok(start > 0);
    const before = workflow.slice(start), after = change(before);
    assert.notEqual(after, before, 'fault must change the private Windows job');
    return workflow.slice(0, start) + after;
}
function fault(name, source, diagnostic) {
    test(name, () => {
        assert.notEqual(source, workflow, 'fault changes the input');
        const result = run(source);
        assert.notEqual(result.status, 0, `auditor accepted ${name}`);
        assert.ok(result.output.includes(diagnostic), `${name}: wrong failure\n${result.output}`);
    });
}
fault('macOS cannot select the retired macos-14 runner image',
    workflow.replace('runs-on: macos-15', 'runs-on: macos-14'),
    'testing-validity workflow lost required gate: runs-on: macos-15');
for (const [index, match] of [...workflow.matchAll(/--no-tests=error/g)].entries()) {
    fault(`CTest command ${index + 1} cannot omit its empty-selection guard`,
        workflow.slice(0, match.index) + workflow.slice(match.index + match[0].length),
        'must fail when no tests are selected');
}
const firstGuard = workflow.indexOf('--no-tests=error');
fault('a comment cannot replace a missing CTest guard',
    '# --no-tests=error\n' + workflow.slice(0, firstGuard) + workflow.slice(firstGuard + '--no-tests=error'.length),
    'must fail when no tests are selected');

for (const [option, correct, wrong] of [
    ['CAPSID_BUILD_HOST', 'ON', 'OFF'], ['CAPSID_BUILD_WORKER', 'ON', 'OFF'],
    ['CAPSID_STRICT_WARNINGS', 'ON', 'OFF'], ['CAPSID_USE_MIMALLOC', 'OFF', 'ON'],
    ['CMAKE_BUILD_TYPE', 'Release', 'Debug'], ['CMAKE_C_COMPILER', 'cl', 'gcc'],
    ['CMAKE_CXX_COMPILER', 'cl', 'g++'], ['VCPKG_TARGET_TRIPLET', 'x64-windows-static', 'x64-windows'],
]) fault(`Windows ${option} cannot borrow another job's setting`,
    windows(source => source.replace(`-D${option}=${correct}`, `-D${option}=${wrong}`)),
    `Windows configure must set exactly one ${option}=${correct}`);
for (const suffix of ['', ':BOOL']) fault(`Windows duplicate Host override ${suffix || 'untyped'} fails`,
    windows(source => source.replace('-DCAPSID_BUILD_HOST=ON', `-DCAPSID_BUILD_HOST${suffix}=OFF \x60\n              -DCAPSID_BUILD_HOST=ON`)),
    'Windows configure must set exactly one CAPSID_BUILD_HOST=ON');

for (const [name, old, replacement, diagnostic] of [
    ['Python version', "python-version: '3.12.3'", "python-version: '3.12.9'", 'prepares the locked Python runtime'],
    ['client setup', 'tools/setup-ws-interop-python.py', 'tools/removed-setup.py', 'tools/setup-ws-interop-python.py'],
    ['Python client path', 'CAPSID_WS_INTEROP_PYTHON', 'REMOVED_PYTHON_PATH', 'CAPSID_WS_INTEROP_PYTHON'],
    ['Node client path', 'CAPSID_WS_INTEROP_NODE_PACKAGE', 'REMOVED_NODE_PATH', 'CAPSID_WS_INTEROP_NODE_PACKAGE'],
    ['certificate tool probe', 'openssl_fixture version', 'openssl_fixture --help', 'openssl_fixture version'],
    ['certificate tool path', 'GITHUB_PATH', 'REMOVED_PATH', 'GITHUB_PATH'],
    ['CTest tree', 'ctest --test-dir build-windows ', 'ctest --test-dir build-wrong ', 'runs its configured CTest tree'],
    ['evidence root', '$evidence_root = (Resolve-Path build-windows).Path', '$evidence_root = (Resolve-Path .).Path', 'evidence root resolves the configured build tree'],
    ['JUnit path', "$junit_path = Join-Path $evidence_root 'windows-junit.xml'", "$junit_path = Join-Path $evidence_root 'lost.xml'", 'retains JUnit evidence'],
    ['JUnit output', '--output-junit $junit_path ', '--output-junit lost.xml ', 'retains JUnit evidence'],
    ['CTest log path', "$ctest_log = Join-Path $evidence_root 'ctest.log'", "$ctest_log = Join-Path $evidence_root 'lost.log'", 'retains the complete CTest log'],
    ['CTest log destination', 'Tee-Object -FilePath $ctest_log', 'Tee-Object -FilePath lost.log', 'retains the complete CTest log'],
    ['CTest failure capture', '$ctest_exit = $LASTEXITCODE', '$ctest_exit = 0', 'propagates the actual CTest failure'],
    ['CTest failure exit', 'exit $ctest_exit', 'exit 0', 'propagates the actual CTest failure'],
    ['CTest exit preserved before another process', '$ctest_exit = $LASTEXITCODE', 'node --version\n          $ctest_exit = $LASTEXITCODE', 'propagates the actual CTest failure'],
    ['reference asset path', "'capsid-workerd-reference/workerd.exe'", "'capsid-workerd-reference/workerd'", 'native reference installer requires'],
    ['reference installer', 'node tools/workerd-reference.mjs install $reference_path', 'node tools/removed-reference.mjs install $reference_path', 'native reference installer requires'],
    ['commented reference installer', 'node tools/workerd-reference.mjs install $reference_path', '# node tools/workerd-reference.mjs install $reference_path', 'native reference installer requires'],
    ['required reference', '"CAPSID_WORKERD_REQUIRED=1"', '"CAPSID_WORKERD_REQUIRED=0"', 'native reference installer requires'],
    ['reference path export', '"CAPSID_WORKERD_REF=$reference_path"', '"REMOVED_REF=$reference_path"', 'native reference installer requires'],
    ['reference gate', 'node tools/run-workerd-ws-gate.mjs build-windows build-windows/workerd-compatibility', 'node tools/removed-gate.mjs build-windows build-windows/workerd-compatibility', 'native reference gate requires'],
    ['commented reference gate', 'node tools/run-workerd-ws-gate.mjs build-windows build-windows/workerd-compatibility', '# node tools/run-workerd-ws-gate.mjs build-windows build-windows/workerd-compatibility', 'native reference gate requires'],
    ['always upload', 'name: Save Windows evidence\n        if: always()', 'name: Save Windows evidence\n        if: success()', 'Windows evidence uploads after failure'],
    ['missing upload failure', 'if-no-files-found: error', 'if-no-files-found: ignore', 'Windows missing evidence fails the upload'],
]) fault(`Windows ${name} is required within its job`,
    windows(source => source.replaceAll(old, replacement)), diagnostic);

for (const name of ['Install the locked native Windows workerd reference', 'Run the native Windows workerd compatibility gate', 'Run Windows host-side matrix']) {
    for (const bypass of ['if: false', 'continue-on-error: true']) fault(`Windows ${name} rejects ${bypass}`,
        windows(source => source.replace(`- name: ${name}`, `- name: ${name}\n        ${bypass}`)),
        'cannot skip or ignore failures');
}
for (const artifact of ['windows-junit.xml', 'ctest.log', 'Testing/Temporary/LastTest.log', 'workerd-compatibility/']) {
    fault(`Windows actual ${artifact} evidence must be uploaded`,
        windows(source => source.replace(`            build-windows/${artifact}`, '            build-windows/lost-evidence')),
        `Windows evidence upload requires: build-windows/${artifact}`);
}

const exclusion = "-E '^wpt_conformance_not_configured$'";
for (const [name, replacement, diagnostic] of [
    ['broad package exclusion', "-E '^(wpt_conformance_not_configured|worker_package_.*)$'", 'must execute package contents'],
    ['one hidden package gate', "-E '^worker_package_metadata_controls$'", 'must execute package contents'],
    ['duplicate exclusion', `${exclusion} -E '^worker_package_smoke$'`, 'must execute package contents'],
    ['unquoted exclusion', `${exclusion} --exclude-regex ^worker_package_smoke$`, 'cannot add an unreviewed or unquoted exclusion'],
    ['joined exclusion', `${exclusion} -E^worker_package_smoke$`, 'cannot add an unreviewed or unquoted exclusion'],
    ['regex selection', `${exclusion} -R websocket`, 'cannot hide required tests'],
    ['joined regex selection', `${exclusion} -Rwebsocket`, 'cannot hide required tests'],
    ['label selection', `${exclusion} --label-regex websocket`, 'cannot hide required tests'],
    ['file exclusion', `${exclusion} --exclude-from-file hidden.txt`, 'cannot hide required tests'],
]) fault(`Windows full profile rejects ${name}`,
    windows(source => source.replace(exclusion, () => replacement)), diagnostic);

function nativeJob(name, change) {
    const next = {'release-wpt-and-delegated-sandbox': 'sanitizers', sanitizers: 'fuzz', fuzz: 'posix-host-library'}[name];
    const start = workflow.indexOf(`  ${name}:\n`), end = workflow.indexOf(`\n  ${next}:\n`, start);
    assert.ok(start > 0 && end > start);
    const before = workflow.slice(start, end), after = change(before);
    assert.notEqual(before, after, 'native-provider fault changes its actual job');
    return workflow.slice(0, start) + after + workflow.slice(end);
}
for (const profile of ['asan', 'ubsan', 'tsan']) {
    for (const hidden of ['host_managed_executable_quarantine_cleared_by_deploy',
        'host_managed_http_rejects_multiple_listeners', 'host_managed_recovery_rejects_oversized_snapshot']) {
        fault(`${profile} cannot hide native ${hidden}`, nativeJob('sanitizers', source => {
            const marker = `          - name: ${profile}\n`;
            const start = source.indexOf(marker);
            assert.ok(start >= 0);
            let end = source.indexOf('\n          - name:', start + marker.length);
            if (end < 0) end = source.indexOf('\n    steps:', start);
            assert.ok(end > start);
            const before = source.slice(start, end);
            const healthy = profile !== 'ubsan' ? '^(wpt_conformance_not_configured|worker_package_.*|websocket_managed_(replacement-recursion|reclamation-idle))$' : '^(wpt_conformance_not_configured|worker_package_.*)$';
            const after = before.replace(healthy, healthy.slice(0,-2)+`|${hidden})$`);
            assert.notEqual(after, before);
            return source.slice(0, start) + after + source.slice(end);
        }), `sanitizer ${profile} exclusion must retain all native managed checks`);
    }
}
for (const job of ['release-wpt-and-delegated-sandbox', 'sanitizers']) {
    fault(`${job} requires explicit independent certificate CLI`, nativeJob(job, source =>
        source.replace('-DCAPSID_TEST_OPENSSL_EXECUTABLE=/usr/bin/openssl',
            '-DCAPSID_TEST_OPENSSL_EXECUTABLE=/opt/openssl35/bin/openssl')),
        'native certificate fixtures bind the installed system CLI independently of instrumented provider libraries');
    fault(`${job} installs the certificate CLI`, nativeJob(job, source =>
        source.replace(' openssl ', ' ')), 'native certificate fixture CLI is installed and probed');
    fault(`${job} probes the certificate CLI`, nativeJob(job, source =>
        source.replace('/usr/bin/openssl version', '/usr/bin/openssl --help')),
        'native certificate fixture CLI is installed and probed');
}
for (const job of ['release-wpt-and-delegated-sandbox', 'sanitizers', 'fuzz']) {
    for (const [name, before, after, diagnostic] of [
        ['official archive', 'https://archive.ubuntu.com/ubuntu/', 'http://azure.archive.ubuntu.com/ubuntu/', 'uses the official Ubuntu archive without changing verification settings'],
        ['bounded retries', 'Acquire::Retries=3', 'Acquire::Retries=300', 'bounds dependency network retries and timeouts'],
        ['index failure propagation', 'update --error-on=any', 'update', 'rejects incomplete apt indexes'],
        ['package network bounds', 'sudo apt-get "${apt_network[@]}" install -y ', 'sudo apt-get install -y ', 'applies network bounds to package downloads'],
    ]) fault(`${job} requires ${name}`, nativeJob(job, source => source.replace(before, after)), `${job} ${diagnostic}`);
}
const rawMemoryHeading='      - name: Record original ASan RSS results\n';
const memoryHeading='      - name: Require separate ASan memory acceptance\n';
const selectedMatrixHeading='      - name: Run ${{ matrix.name }} matrix\n';
for (const [name,before,after,diagnostic] of [
 ['raw identity','id: asan_rss_raw','id: removed_raw','stable raw RSS step identity'],
 ['raw condition',"if: matrix.name == 'asan'",'if: false','raw RSS observation runs for every ASan entry'],
 ['raw deferred conclusion',rawMemoryHeading+'        id: asan_rss_raw\n        if: matrix.name == \'asan\'\n        continue-on-error: true',rawMemoryHeading+'        id: asan_rss_raw\n        if: matrix.name == \'asan\'\n        continue-on-error: false','raw ASan RSS failure outcome is deferred only'],
 ['both raw cases',"-R '^websocket_managed_(replacement-recursion|reclamation-idle)$'","-R '^websocket_managed_replacement-recursion$'",'ASan raw RSS preserves actual complete evidence'],
 ['actual raw exit','|| asan_rss_status=$?','|| true','ASan raw RSS preserves actual complete evidence'],
 ['raw complete log','cp build-asan/Testing/Temporary/LastTest.log build-asan/asan-rss-raw-LastTest.log','cp build-asan/Testing/Temporary/LastTest.log build-asan/lost.log','ASan raw RSS preserves actual complete evidence'],
 ['no manufactured raw exit','          printf \'%s\\n\' "${asan_rss_status}"','          asan_rss_status=0\n          printf \'%s\\n\' "${asan_rss_status}"','raw ASan RSS exit cannot be reset or manufactured'],
]) fault('ASan memory requires '+name,nativeJob('sanitizers',source=>source.replace(before,()=>after)),diagnostic);
const actualMemoryGuard="        if: ${{ !cancelled() && matrix.name == 'asan' && steps.asan_rss_raw.outcome != 'skipped' }}\n";
for (const [name,before,after,diagnostic] of [
 ['post-failure acceptance',memoryHeading+actualMemoryGuard,memoryHeading+'        if: success()\n','separate ASan acceptance runs after raw success or failure'],
 ['mandatory acceptance',memoryHeading+actualMemoryGuard,memoryHeading+actualMemoryGuard+'        continue-on-error: true\n','separate ASan acceptance cannot ignore failures'],
 ['actual acceptor','python3 -B tools/asan-memory-acceptance.py','python3 -B tools/removed.py','separate ASan acceptance binds actual inputs'],
 ['raw log input','--raw-last-test build-asan/asan-rss-raw-LastTest.log','--raw-last-test missing.log','separate ASan acceptance binds actual inputs'],
 ['raw exit input','--raw-exit-code build-asan/asan-rss-raw-exit-code.txt','--raw-exit-code missing.txt','separate ASan acceptance binds actual inputs'],
 ['raw outcome',"--raw-outcome '${{ steps.asan_rss_raw.outcome }}'","--raw-outcome 'success'",'separate ASan acceptance binds actual inputs'],
 ['complete inventory','--registration build-asan/asan-memory-registration.json','--registration missing.json','separate ASan acceptance binds actual inputs'],
 ['actual selected matrix','--matrix-junit build-asan/asan-junit.xml','--matrix-junit missing.xml','separate ASan acceptance binds actual inputs'],
 ['durable acceptance','--output build-asan/asan-memory-acceptance.json','--output ignored.json','separate ASan acceptance binds actual inputs'],
]) fault('ASan memory requires '+name,nativeJob('sanitizers',source=>source.replace(before,()=>after)),diagnostic);
const tsanRawMemoryHeading='      - name: Record original TSan RSS results\n';
const tsanMemoryHeading='      - name: Require separate TSan memory acceptance\n';
function tsanMemoryChange(source, before, after) {
    const start = source.indexOf(tsanRawMemoryHeading);
    assert.ok(start >= 0);
    return source.slice(0, start) + source.slice(start).replace(before, () => after);
}
for (const [name,before,after,diagnostic] of [
 ['raw identity','id: tsan_rss_raw','id: removed_raw','stable raw RSS step identity'],
 ['raw condition',"if: matrix.name == 'tsan'",'if: false','raw RSS observation runs for every TSan entry'],
 ['raw deferred conclusion',tsanRawMemoryHeading+'        id: tsan_rss_raw\n        if: matrix.name == \'tsan\'\n        continue-on-error: true',tsanRawMemoryHeading+'        id: tsan_rss_raw\n        if: matrix.name == \'tsan\'\n        continue-on-error: false','raw TSan RSS failure outcome is deferred only'],
 ['both raw cases',"-R '^websocket_managed_(replacement-recursion|reclamation-idle)$'","-R '^websocket_managed_replacement-recursion$'",'TSan raw RSS preserves actual complete evidence'],
 ['actual raw exit','|| tsan_rss_status=$?','|| true','TSan raw RSS preserves actual complete evidence'],
 ['raw complete log','cp build-tsan/Testing/Temporary/LastTest.log build-tsan/tsan-rss-raw-LastTest.log','cp build-tsan/Testing/Temporary/LastTest.log build-tsan/lost.log','TSan raw RSS preserves actual complete evidence'],
 ['no manufactured raw exit','          printf \'%s\\n\' "${tsan_rss_status}"','          tsan_rss_status=0\n          printf \'%s\\n\' "${tsan_rss_status}"','raw TSan RSS exit cannot be reset or manufactured'],
]) fault('TSan memory requires '+name,nativeJob('sanitizers',source=>tsanMemoryChange(source,before,after)),diagnostic);
const tsanActualMemoryGuard="        if: ${{ !cancelled() && matrix.name == 'tsan' && steps.tsan_rss_raw.outcome != 'skipped' }}\n";
for (const [name,before,after,diagnostic] of [
 ['post-failure acceptance',tsanMemoryHeading+tsanActualMemoryGuard,tsanMemoryHeading+'        if: success()\n','separate TSan acceptance runs after raw success or failure'],
 ['mandatory acceptance',tsanMemoryHeading+tsanActualMemoryGuard,tsanMemoryHeading+tsanActualMemoryGuard+'        continue-on-error: true\n','separate TSan acceptance cannot ignore failures'],
 ['actual acceptor','python3 -B tools/asan-memory-acceptance.py --sanitizer tsan','python3 -B tools/removed.py','separate TSan acceptance binds actual inputs'],
 ['raw log input','--raw-last-test build-tsan/tsan-rss-raw-LastTest.log','--raw-last-test missing.log','separate TSan acceptance binds actual inputs'],
 ['raw exit input','--raw-exit-code build-tsan/tsan-rss-raw-exit-code.txt','--raw-exit-code missing.txt','separate TSan acceptance binds actual inputs'],
 ['raw outcome',"--raw-outcome '${{ steps.tsan_rss_raw.outcome }}'","--raw-outcome 'success'",'separate TSan acceptance binds actual inputs'],
 ['complete inventory','--registration build-tsan/tsan-memory-registration.json','--registration missing.json','separate TSan acceptance binds actual inputs'],
 ['actual selected matrix','--matrix-junit build-tsan/tsan-junit.xml','--matrix-junit missing.xml','separate TSan acceptance binds actual inputs'],
 ['durable acceptance','--output build-tsan/tsan-memory-acceptance.json','--output ignored.json','separate TSan acceptance binds actual inputs'],
]) fault('TSan memory requires '+name,nativeJob('sanitizers',source=>tsanMemoryChange(source,before,after)),diagnostic);
fault('TSan raw RSS keeps fatal race diagnostics',nativeJob('sanitizers',source=>source.replace('TSAN_OPTIONS: halt_on_error=1:exitcode=66','TSAN_OPTIONS: halt_on_error=0:exitcode=0')),'raw TSan RSS preserves fatal race diagnostics');
for (const path of ['build-tsan/tsan-rss-raw-junit.xml','build-tsan/tsan-rss-raw-LastTest.log','build-tsan/tsan-rss-raw-exit-code.txt','build-tsan/tsan-memory-*.json'])
 fault('TSan memory retains artifact '+path,nativeJob('sanitizers',source=>source.replace('            '+path,'            missing-artifact')),'TSan acceptance uploads '+path);
fault('sanitizer selected matrix cannot ignore failures',nativeJob('sanitizers',source=>source.replace(selectedMatrixHeading,selectedMatrixHeading+'        continue-on-error: true\n')),'cannot skip or ignore failures');
fault('sanitizer selected matrix cannot narrow other tests',nativeJob('sanitizers',source=>source.replace("-E '${{ matrix.ctest_exclude }}'","-E '${{ matrix.ctest_exclude }}' -R websocket")),'sanitizer matrix cannot hide other native checks');
for (const path of ['build-asan/asan-rss-raw-junit.xml','build-asan/asan-rss-raw-LastTest.log','build-asan/asan-memory-*.json'])
 fault('ASan memory retains artifact '+path,nativeJob('sanitizers',source=>source.replace('            '+path,'            missing-artifact')),'ASan acceptance uploads '+path);
const nativePins = JSON.parse(fs.readFileSync(new URL('../patches/native-providers/manifest.json', import.meta.url)));
for (const [job, suffix] of [['release-wpt-and-delegated-sandbox', 'Release'], ['sanitizers', 'sanitizer targets']]) {
    for (const provider of ['openssl', 'cares']) fault(`${suffix} ${provider} cannot omit reviewed corrections`,
        nativeJob(job, source => source.replace(`--provider ${provider} --source`, `--provider removed-${provider} --source`)),
        `${suffix} ${provider} applies the reviewed provider corrections`);
    fault(`${suffix} c-ares archive identity is checked`,
        nativeJob(job, source => source.replace(nativePins.providers.cares.source.sha256, 'f'.repeat(64))),
        `${suffix} c-ares requires the locked upstream archive`);
    fault(`${suffix} c-ares remains a static provider`,
        nativeJob(job, source => source.replace('-DCARES_SHARED=OFF', '-DCARES_SHARED=ON')),
        `${suffix} c-ares retains its static provider and compilation evidence`);
    fault(`${suffix} Capsid cannot use older system c-ares headers`,
        nativeJob(job, source => source.replace('-DCAPSID_CARES_PROVIDER_DIR=/opt/openssl35/include', '-DCAPSID_CARES_PROVIDER_DIR=/usr/include')),
        `${suffix} Capsid uses the qualified c-ares headers`);
}
for (const [name, before, after, diagnostic] of [
    ['diagnostic profile', '-DCAPSID_SANDBOX_PROFILE=sanitizer', '-DCAPSID_SANDBOX_PROFILE=production', 'sanitizers explicitly use the diagnostic sandbox'],
    ['c-ares instrumentation', '-DCMAKE_C_FLAGS="${sanitizer_flags}"', '-DCMAKE_C_FLAGS=""', 'c-ares compilation and link consume the selected sanitizer'],
    ['leak scanning', 'ASAN_OPTIONS: detect_leaks=1', 'ASAN_OPTIONS: detect_leaks=0', 'sanitizer qualification keeps leak scanning enabled'],
    ['native network checks', '^(wpt_conformance_not_configured|worker_package_.*)$', '^(wpt_conformance_not_configured|worker_strict_sandbox_direct_fetch|worker_package_.*)$', 'diagnostic sanitizer matrix cannot exclude the native network checks'],
]) fault(`sanitizer workflow rejects missing ${name}`,
    nativeJob('sanitizers', source => source.replace(before, after)), diagnostic);
fault('Release cannot borrow the diagnostic sandbox',
    nativeJob('release-wpt-and-delegated-sandbox', source => source.replace('-DCAPSID_SANDBOX_PROFILE=production', '-DCAPSID_SANDBOX_PROFILE=sanitizer')),
    'Release explicitly qualifies the production sandbox');
