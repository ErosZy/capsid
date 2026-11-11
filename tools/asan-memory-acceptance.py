"""Audit preserved instrumented RSS results and separate resource checks.

Production RSS remains mandatory in the independent ordinary Release matrix.
Complete ASan/LSan is an independent mandatory gate for TSan resource coverage.
This entry point never changes a raw CTest result or claims production coverage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

SCENARIOS = ('replacement-recursion', 'reclamation-idle')
RAW_NAMES = tuple('websocket_managed_' + name for name in SCENARIOS)
RESOURCE_NAMES = tuple('websocket_managed_asan_resources_' + name for name in SCENARIOS)
ASAN_OPTIONS = {'detect_leaks=1', 'abort_on_error=1', 'strict_string_checks=1'}


def check(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        check(key not in result, 'duplicate JSON key: ' + key)
        result[key] = value
    return result


def read_json(path):
    return json.loads(Path(path).read_text('utf-8-sig'), object_pairs_hook=unique_object)


def junit_cases(path):
    raw = Path(path).read_bytes()
    check(len(raw) <= 64 * 1024 * 1024, 'bounded JUnit input')
    check(b'<!DOCTYPE' not in raw.upper() and b'<!ENTITY' not in raw.upper(), 'JUnit cannot declare entities')
    root = ET.fromstring(raw)
    check(root.tag == 'testsuite', 'expected the actual CTest testsuite')
    result = {}
    for case in root.findall('testcase'):
        name = case.get('name')
        check(bool(name) and name not in result, 'unique named JUnit cases')
        failed = case.find('failure') is not None or case.find('error') is not None
        skipped = case.find('skipped') is not None
        check(not (failed and skipped), 'a JUnit case cannot fail and skip')
        status = 'failed' if failed else 'skipped' if skipped else 'passed'
        check(case.get('status') == {'failed': 'fail', 'skipped': 'notrun', 'passed': 'run'}[status],
              'JUnit status and result must agree')
        result[name] = status
    check(bool(result), 'nonempty actual JUnit cases')
    for attribute, expected in [('tests', len(result)), ('failures', sum(v == 'failed' for v in result.values())),
                                ('skipped', sum(v == 'skipped' for v in result.values())), ('disabled', 0)]:
        check(root.get(attribute) == str(expected), 'JUnit summary agrees with actual ' + attribute)
    return result


def properties(test):
    result = {}
    for item in test.get('properties', []):
        check(item['name'] not in result, 'unique registration properties')
        result[item['name']] = item['value']
    check(not result.get('DISABLED') and not result.get('WILL_FAIL'), 'registered checks cannot be disabled or inverted')
    return result


def option(command, name):
    check(command.count(name) == 1, 'exactly one command option: ' + name)
    index = command.index(name)
    check(index + 1 < len(command), 'command option has a value: ' + name)
    return command[index + 1]


def check_raw_log(text, cases):
    starts = list(re.finditer(r'^\d+/\d+ Testing: (\S+)\s*$', text, re.M))
    check(len(starts) == 2 and {match[1] for match in starts} == set(RAW_NAMES),
          'complete raw LastTest for both original RSS cases')
    for index, match in enumerate(starts):
        block = text[match.start():starts[index + 1].start() if index + 1 < len(starts) else len(text)]
        name = match[1]
        results = re.findall(r'^Test (Passed|Failed)\.$', block, re.M)
        check(results == [('Failed' if cases[name] == 'failed' else 'Passed')], 'raw log and JUnit results agree')
        check(not any(token in block for token in ['ERROR: AddressSanitizer:', 'ERROR: LeakSanitizer:',
                  'runtime error:', 'WARNING: ThreadSanitizer:', 'FATAL:']), 'sanitizer errors cannot be waived as footprint observations')
        if cases[name] == 'failed':
            messages = re.findall(r'^AssertionError \[ERR_ASSERTION\]: ([^\r\n]+)$', block, re.M)
            pattern = (r'(?:16 replacements grow RSS by [0-9]+ KiB; retired pools are reclaimed|'
                       r'16 replacements grow VmSize by [0-9]+ KiB|full retire returns the footprint)'
                       if name.endswith('replacement-recursion') else
                       r'last retired large factory returns memory without another admin operation')
            check(len(messages) == 1 and re.fullmatch(pattern, messages[0]) is not None,
                  'only an actual original allocator footprint assertion may fail')
            check(not re.search(r'^(?:Error|AggregateError|TypeError|ReferenceError):', block, re.M),
                  'other raw scenario errors remain failures')


def audit(registration, raw_cases, matrix_cases, exit_text, outcome, report_root, raw_log, sanitizer='asan'):
    check(sanitizer in ('asan','tsan'), 'known instrumented resource profile')
    resource_names = tuple('websocket_managed_' + sanitizer + '_resources_' + name for name in SCENARIOS)
    check(isinstance(registration.get('tests'), list) and registration['tests'], 'complete nonempty registration')
    tests = {test['name']: test for test in registration['tests']}
    check(len(tests) == len(registration['tests']), 'unique registered test names')
    props = {name: properties(test) for name, test in tests.items()}
    check(set(raw_cases) == set(RAW_NAMES), 'both exact original RSS cases actually executed')
    check(all(status in ('passed', 'failed') for status in raw_cases.values()), 'raw RSS cases cannot skip')
    failed = sorted(name for name, status in raw_cases.items() if status == 'failed')
    check(exit_text == ('8\n' if failed else '0\n'), 'recorded raw CTest exit agrees with original results')
    check(outcome == ('failure' if failed else 'success'), 'raw workflow outcome remains unchanged')
    check_raw_log(raw_log, raw_cases)
    expected = {name for name in tests if name not in RAW_NAMES and name != 'wpt_conformance_not_configured'
                and not name.startswith('worker_package_')}
    check(set(matrix_cases) == expected, 'every selected non-RSS sanitizer registration actually reported')
    check(all(status in ('passed', 'skipped') for status in matrix_cases.values()), 'all selected sanitizer checks must pass')
    for name, status in matrix_cases.items():
        if status == 'skipped':
            check(type(props[name].get('SKIP_RETURN_CODE')) is int and props[name]['SKIP_RETURN_CODE'] == 77,
                  'skips require the registered prerequisite code 77')
            check('SKIP_REGULAR_EXPRESSION' not in props[name], 'regex skips cannot qualify sanitizer acceptance')
    reports = []
    for scenario, raw_name, resource_name in zip(SCENARIOS, RAW_NAMES, resource_names):
        check(raw_name in tests and resource_name in tests, 'both acceptance profiles remain registered')
        check(matrix_cases.get(resource_name) == 'passed', 'complete resource acceptance cannot skip')
        original = tests[raw_name]['command']
        command = tests[resource_name]['command']
        check('--memory-acceptance' not in original, 'original RSS command retains its default oracle')
        check(command[:2] == original[:2], 'both profiles execute the same actual test program')
        check(option(original, '--case') == option(command, '--case') == scenario, 'paired scenarios agree')
        check(option(command, '--memory-acceptance') == sanitizer + '-resources', 'explicit ASan resource profile')
        check(props[raw_name].get('TIMEOUT') == props[resource_name].get('TIMEOUT') == 120, 'original outer budgets remain 120 seconds')
        for flag in ('--host', '--worker'):
            check(option(command, flag) == option(original, flag), 'paired actual executable identities agree')
        expected_path = Path(report_root) / (sanitizer + '-memory-' + scenario + '.json')
        check(Path(option(command, '--report')).resolve() == expected_path.resolve(), 'registered durable resource report')
        report = read_json(expected_path)
        check(report.get('complete') is True and report.get('scenario') == scenario, 'complete actual scenario report')
        shutdown = report.get('shutdown', {})
        check(shutdown == dict(code=0, signal=None, forced=False) and type(shutdown.get('code')) is int
              and shutdown.get('forced') is False, 'natural successful shutdown with no forced cleanup')
        check(type(report.get('assertions')) is int and report['assertions'] > 0, 'actual resource assertions executed')
        memory = report['memoryAcceptance']
        check(memory.get('profile') == sanitizer + '-resources' and memory.get('sanitizerProfile') == sanitizer,
              'actual resource profile and selected sanitizer agree')
        check(memory.get('fullLeakDetectionRequired') is (sanitizer == 'asan'), 'LSan is required only in the actual ASan process')
        check(memory.get('separateCompleteLsanAcceptanceRequired') is (sanitizer == 'tsan'),
              'TSan requires the separate complete ASan/LSan gate and cannot claim leakage coverage itself')
        check(memory.get('productionRssAcceptanceRequiredSeparately') is True, 'production RSS remains an independent requirement')
        markers = memory.get('sanitizerRuntimeMarkers', {})
        check(markers == dict(host=True, worker=True) and all(value is True for value in markers.values()),
              'actual host and worker use selected sanitizer runtime markers')
        fields = memory.get('asanOptions' if sanitizer == 'asan' else 'tsanOptions', '').split(':')
        expected_options = ASAN_OPTIONS if sanitizer == 'asan' else {'halt_on_error=1','exitcode=66'}
        check(len(fields) == len(expected_options) and set(fields) == expected_options,
              'complete selected sanitizer diagnostics and default runtime options remain enabled')
        footprints = memory.get('footprintChecks')
        check(isinstance(footprints, list) and len(footprints) == (3 if scenario == 'replacement-recursion' else 1),
              'all original footprint predicates were recorded')
        check(all(type(row.get('passed')) is bool and isinstance(row.get('message'), str) and row['message'] for row in footprints),
              'original footprint results retain their actual Boolean observations')
        source = Path(command[1])
        for key, file in [('sourceSHA256', source), ('helperSHA256', source.parent / 'helpers/ws_host_backend.mjs'),
                          ('hostSHA256', Path(option(command, '--host'))), ('workerSHA256', Path(option(command, '--worker')))]:
            check(report.get(key) == digest(file), 'resource report input identity: ' + key)
        reports.append(dict(scenario=scenario,sha256=digest(expected_path),originalFootprintChecks=footprints,
                            naturalShutdown=True,fullLsan=sanitizer=='asan'))
    return dict(schema='capsid-sanitizer-memory-acceptance-v2',complete=True,sanitizerProfile=sanitizer,
                sanitizerResourceAcceptanceQualified=True,asanResourceAcceptanceQualified=sanitizer=='asan',
                completeAsanLsanGateRequired=True,tsanLsanPassedNotClaimedHere=sanitizer=='tsan',
                productionRssReleaseGateRequired=True,productionRssPassedNotClaimedHere=True,
                rawRssFailures=failed,rawRssCTestExitCode=int(exit_text),rawStepOutcome=outcome,
                selectedSanitizerCases=len(matrix_cases),originalRawResultsRetained=True,resourceReports=reports,
                wholeReleaseQualifiedNotClaimed=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ('registration', 'raw-junit', 'matrix-junit', 'raw-exit-code', 'raw-last-test', 'resource-report-root', 'output'):
        parser.add_argument('--' + argument, required=True, type=Path)
    parser.add_argument('--sanitizer', choices=('asan','tsan'), default='asan')
    parser.add_argument('--raw-outcome', required=True, choices=('success', 'failure'))
    args = parser.parse_args()
    check(not args.output.exists(), 'acceptance output must be fresh')
    report = audit(read_json(args.registration), junit_cases(args.raw_junit), junit_cases(args.matrix_junit),
                   args.raw_exit_code.read_text('utf-8'), args.raw_outcome, args.resource_report_root,
                   args.raw_last_test.read_text('utf-8'), args.sanitizer)
    report['inputs'] = {name: dict(path=str(file),sha256=digest(file)) for name,file in
                       [('registration',args.registration),('rawJUnit',args.raw_junit),('matrixJUnit',args.matrix_junit),('rawExitCode',args.raw_exit_code),('rawLastTest',args.raw_last_test)]}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x',encoding='utf-8',newline='\n') as stream:
        json.dump(report,stream,indent=2)
        stream.write('\n')
    print(json.dumps(dict(sanitizer=args.sanitizer,resourcesQualified=True,rawRssFailuresRetained=report['rawRssFailures'],productionRssReleaseGateRequired=True,completeAsanLsanGateRequired=True)))


if __name__ == '__main__':
    main()
