"""Metadata mutation controls; real native scenarios are qualified separately."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--tool', type=Path, default=Path(__file__).resolve().parents[1] / 'tools/asan-memory-acceptance.py')
args, remaining = parser.parse_known_args()
sys.argv[1:] = remaining
spec = importlib.util.spec_from_file_location('asan_memory_acceptance', args.tool)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='capsid asan memory controls ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / 'tests/scenario.mjs'
        self.helper = self.root / 'tests/helpers/ws_host_backend.mjs'
        self.host, self.worker = self.root / 'host', self.root / 'worker'
        for file in [self.source, self.helper, self.host, self.worker]:
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text('private metadata fixture: ' + file.name)

    def fixture(self):
        tests, reports = [], {}
        for scenario, raw_name, resource_name in zip(gate.SCENARIOS, gate.RAW_NAMES, gate.RESOURCE_NAMES):
            command = ['node', str(self.source), '--host', str(self.host), '--worker', str(self.worker), '--case', scenario]
            tests.append(dict(name=raw_name, command=command, properties=[dict(name='TIMEOUT', value=120.0)]))
            tests.append(dict(name=resource_name, command=command + ['--memory-acceptance', 'asan-resources', '--report',
                         str(self.root / ('asan-memory-' + scenario + '.json'))], properties=[dict(name='TIMEOUT', value=120.0)]))
            reports[scenario] = dict(complete=True, scenario=scenario, assertions=100,
                shutdown=dict(code=0, signal=None, forced=False), sourceSHA256=gate.digest(self.source),
                helperSHA256=gate.digest(self.helper), hostSHA256=gate.digest(self.host), workerSHA256=gate.digest(self.worker),
                memoryAcceptance=dict(profile='asan-resources', sanitizerProfile='asan', fullLeakDetectionRequired=True, separateCompleteLsanAcceptanceRequired=False,
                    productionRssAcceptanceRequiredSeparately=True, sanitizerRuntimeMarkers=dict(host=True, worker=True),
                    asanOptions='detect_leaks=1:abort_on_error=1:strict_string_checks=1',
                    footprintChecks=[dict(passed=False, message='original footprint observation') for _ in range(3 if scenario=='replacement-recursion' else 1)]))
        for name in ['ordinary_native_guard', 'worker_package_smoke', 'wpt_conformance_not_configured']:
            tests.append(dict(name=name, command=['private-control'], properties=[]))
        tests.append(dict(name='prerequisite_probe', command=['private-control'], properties=[dict(name='SKIP_RETURN_CODE', value=77)]))
        return dict(registration=dict(tests=tests),raw={name:'failed' for name in gate.RAW_NAMES},
                    matrix={**{name:'passed' for name in gate.RESOURCE_NAMES},'ordinary_native_guard':'passed','prerequisite_probe':'skipped'},
                    exit='8\n',outcome='failure',reports=reports)

    def invoke(self, data):
        for scenario, report in data['reports'].items():
            (self.root / (data.get('sanitizer','asan')+'-memory-' + scenario + '.json')).write_text(json.dumps(report))
        return gate.audit(data['registration'],data['raw'],data['matrix'],data['exit'],data['outcome'],self.root,self.raw_log(data),data.get('sanitizer','asan'))

    def raw_log(self, data):
        text=''
        for index,(name,status) in enumerate(data['raw'].items()):
            message=('16 replacements grow RSS by 198000 KiB; retired pools are reclaimed' if name.endswith('replacement-recursion') else
                     'last retired large factory returns memory without another admin operation')
            text+=f'{index+1}/4 Testing: {name}\n'
            if status=='failed':text+='AssertionError [ERR_ASSERTION]: '+message+'\n'
            text+='Test '+('Failed' if status=='failed' else 'Passed')+'.\n'
        return data.get('rawLogOverride',text)

    def thread_fixture(self):
        data=self.fixture();data['sanitizer']='tsan'
        for scenario,old in zip(gate.SCENARIOS,gate.RESOURCE_NAMES):
            new='websocket_managed_tsan_resources_'+scenario
            row=next(t for t in data['registration']['tests'] if t['name']==old);row['name']=new
            row['command']=[value.replace('asan-resources','tsan-resources').replace('asan-memory-','tsan-memory-') for value in row['command']]
            data['matrix'][new]=data['matrix'].pop(old)
            memory=data['reports'][scenario]['memoryAcceptance'];memory.pop('asanOptions')
            memory.update(profile='tsan-resources',sanitizerProfile='tsan',fullLeakDetectionRequired=False,separateCompleteLsanAcceptanceRequired=True,tsanOptions='halt_on_error=1:exitcode=66')
        return data

    def test_thread_profile_requires_independent_complete_lsan(self):
        result=self.invoke(self.thread_fixture())
        self.assertTrue(result['sanitizerResourceAcceptanceQualified'])
        self.assertFalse(result['asanResourceAcceptanceQualified'])
        self.assertTrue(result['completeAsanLsanGateRequired'])
        self.assertTrue(result['tsanLsanPassedNotClaimedHere'])
        self.assertTrue(all(row['fullLsan'] is False for row in result['resourceReports']))

    def test_thread_profile_negative_controls(self):
        faults=[('LSan claim',dict(fullLeakDetectionRequired=True)),
            ('missing independent LSan',dict(separateCompleteLsanAcceptanceRequired=False)),
            ('nonfatal races',dict(tsanOptions='halt_on_error=0:exitcode=66')),
            ('suppressed races',dict(tsanOptions='halt_on_error=1:exitcode=66:suppressions=/dev/null')),
            ('ASan profile substitution',dict(profile='asan-resources')),
            ('wrong sanitizer identity',dict(sanitizerProfile='asan'))]
        self.assertEqual(6,len(faults))
        for name,mutation in faults:
            with self.subTest(name=name):
                data=self.thread_fixture();data['reports'][gate.SCENARIOS[0]]['memoryAcceptance'].update(mutation)
                with self.assertRaises(ValueError):self.invoke(data)

    def test_healthy_preserves_both_raw_failures(self):
        result = self.invoke(self.fixture())
        self.assertEqual(sorted(gate.RAW_NAMES), result['rawRssFailures'])
        self.assertTrue(result['asanResourceAcceptanceQualified'])
        self.assertTrue(all(row['fullLsan'] is True for row in result['resourceReports']))
        self.assertTrue(result['productionRssPassedNotClaimedHere'])
        self.assertTrue(result['wholeReleaseQualifiedNotClaimed'])

    def test_actual_raw_success_is_not_forced_to_failure(self):
        data = self.fixture();data.update(raw={name:'passed' for name in gate.RAW_NAMES},exit='0\n',outcome='success')
        self.assertEqual([], self.invoke(data)['rawRssFailures'])

    def test_fault_controls(self):
        raw, resource = gate.RAW_NAMES[0], gate.RESOURCE_NAMES[0]
        def test(d, name):return next(t for t in d['registration']['tests'] if t['name']==name)
        def report(d):return d['reports'][gate.SCENARIOS[0]]
        def memory(d):return report(d)['memoryAcceptance']
        def property_(d,name,key,value):test(d,name)['properties'].append(dict(name=key,value=value))
        faults=[
            ('missing original',lambda d:d['raw'].pop(raw)),
            ('skipped original',lambda d:d['raw'].update({raw:'skipped'})),
            ('manufactured raw exit',lambda d:d.update(exit='0\n')),
            ('erased raw outcome',lambda d:d.update(outcome='success')),
            ('duplicate registration',lambda d:d['registration']['tests'].append(copy.deepcopy(test(d,raw)))),
            ('missing native result',lambda d:d['matrix'].pop('ordinary_native_guard')),
            ('unexpected native failure',lambda d:d['matrix'].update(ordinary_native_guard='failed')),
            ('resource skipped',lambda d:d['matrix'].update({resource:'skipped'})),
            ('extra result',lambda d:d['matrix'].update(unregistered='passed')),
            ('disabled check',lambda d:property_(d,raw,'DISABLED',True)),
            ('inverted check',lambda d:property_(d,raw,'WILL_FAIL',True)),
            ('duplicate property',lambda d:property_(d,raw,'TIMEOUT',120)),
            ('undeclared skip',lambda d:test(d,'prerequisite_probe').update(properties=[])),
            ('regex skip',lambda d:property_(d,'prerequisite_probe','SKIP_REGULAR_EXPRESSION','.*')),
            ('raw oracle replaced',lambda d:test(d,raw)['command'].extend(['--memory-acceptance','asan-resources'])),
            ('wrong resource profile',lambda d:test(d,resource)['command'].__setitem__(test(d,resource)['command'].index('asan-resources'),'waive')),
            ('different scenario',lambda d:test(d,resource)['command'].__setitem__(test(d,resource)['command'].index('--case')+1,'deploy')),
            ('different binary',lambda d:test(d,resource)['command'].__setitem__(test(d,resource)['command'].index('--host')+1,'different')),
            ('different program',lambda d:test(d,resource)['command'].__setitem__(1,'different.mjs')),
            ('weakened timeout',lambda d:test(d,resource).update(properties=[dict(name='TIMEOUT',value=999)])),
            ('wrong durable report',lambda d:test(d,resource)['command'].__setitem__(test(d,resource)['command'].index('--report')+1,'other.json')),
            ('incomplete report',lambda d:report(d).update(complete=False)),
            ('forced shutdown',lambda d:report(d)['shutdown'].update(forced=True)),
            ('failed shutdown',lambda d:report(d)['shutdown'].update(code=66)),
            ('Boolean exit',lambda d:report(d)['shutdown'].update(code=False)),
            ('signal shutdown',lambda d:report(d)['shutdown'].update(signal='SIGKILL')),
            ('no assertions',lambda d:report(d).update(assertions=0)),
            ('disabled leaks',lambda d:memory(d).update(asanOptions='detect_leaks=0:abort_on_error=1:strict_string_checks=1')),
            ('quarantine override',lambda d:memory(d).update(asanOptions=memory(d)['asanOptions']+':quarantine_size_mb=0')),
            ('duplicate options',lambda d:memory(d).update(asanOptions=memory(d)['asanOptions']+':detect_leaks=0')),
            ('no ASan host',lambda d:memory(d)['sanitizerRuntimeMarkers'].update(host=False)),
            ('numeric marker',lambda d:memory(d)['sanitizerRuntimeMarkers'].update(host=1)),
            ('missing footprint',lambda d:memory(d)['footprintChecks'].pop()),
            ('string footprint status',lambda d:memory(d)['footprintChecks'][0].update(passed='false')),
            ('erased footprint message',lambda d:memory(d)['footprintChecks'][0].update(message='')),
            ('wrong source hash',lambda d:report(d).update(sourceSHA256='0'*64)),
            ('wrong helper hash',lambda d:report(d).update(helperSHA256='0'*64)),
            ('wrong host hash',lambda d:report(d).update(hostSHA256='0'*64)),
            ('wrong worker hash',lambda d:report(d).update(workerSHA256='0'*64)),
            ('no separate production gate',lambda d:memory(d).update(productionRssAcceptanceRequiredSeparately=False)),
            ('unrelated raw failure',lambda d:d.update(rawLogOverride=self.raw_log(d).replace('16 replacements grow RSS by 198000 KiB; retired pools are reclaimed','admin startup failed'))),
            ('raw sanitizer error',lambda d:d.update(rawLogOverride=self.raw_log(d)+'\nERROR: LeakSanitizer: detected memory leaks\n')),
            ('incomplete raw log',lambda d:d.update(rawLogOverride='1/4 Testing: '+raw+'\nTest Failed.\n')),
        ]
        self.assertEqual(43,len(faults))
        for name, change in faults:
            with self.subTest(name=name):
                data=self.fixture();change(data)
                with self.assertRaises(ValueError):self.invoke(data)

    def xml(self, rows):
        root=ET.Element('testsuite',dict(tests=str(len(rows)),failures=str(sum(v=='failed' for _,v in rows)),skipped=str(sum(v=='skipped' for _,v in rows)),disabled='0'))
        for name,status in rows:
            case=ET.SubElement(root,'testcase',dict(name=name,status={'passed':'run','failed':'fail','skipped':'notrun'}[status]))
            if status!='passed':ET.SubElement(case,'failure' if status=='failed' else 'skipped')
        return root

    def test_junit_integrity_controls(self):
        path=self.root/'results.xml'
        root=self.xml([('original','failed')]);ET.ElementTree(root).write(path)
        self.assertEqual({'original':'failed'},gate.junit_cases(path))
        for name,change in [('duplicate',lambda r:r.append(copy.deepcopy(r[0]))),('summary',lambda r:r.set('tests','99')),('status',lambda r:r[0].set('status','run')),('skip and failure',lambda r:ET.SubElement(r[0],'skipped'))]:
            with self.subTest(name=name):
                root=self.xml([('original','failed')]);change(root);ET.ElementTree(root).write(path)
                with self.assertRaises(ValueError):gate.junit_cases(path)
        path.write_text('<!DOCTYPE testsuite [<!ENTITY data "hidden">]><testsuite/>')
        with self.assertRaises(ValueError):gate.junit_cases(path)

    def test_duplicate_json_keys_rejected(self):
        path=self.root/'duplicate.json';path.write_text('{"complete":false,"complete":true}')
        with self.assertRaises(ValueError):gate.read_json(path)

    def test_actual_cli_output_is_fresh_and_keeps_raw_result(self):
        data=self.fixture();self.invoke(data)
        registration=self.root/'registration.json';registration.write_text(json.dumps(data['registration']))
        raw=self.root/'raw.xml';ET.ElementTree(self.xml(list(data['raw'].items()))).write(raw)
        matrix=self.root/'matrix.xml';ET.ElementTree(self.xml(list(data['matrix'].items()))).write(matrix)
        exit_file=self.root/'exit-code.txt';exit_file.write_text('8\n')
        raw_log=self.root/'raw-LastTest.log';raw_log.write_text(self.raw_log(data))
        output=self.root/'acceptance.json'
        command=[sys.executable,str(args.tool),'--registration',str(registration),'--raw-junit',str(raw),'--matrix-junit',str(matrix),'--raw-exit-code',str(exit_file),'--raw-last-test',str(raw_log),'--resource-report-root',str(self.root),'--output',str(output),'--raw-outcome','failure']
        result=subprocess.run(command,capture_output=True)
        self.assertEqual(0,result.returncode,result.stderr)
        original=output.read_bytes();self.assertEqual(sorted(gate.RAW_NAMES),json.loads(original)['rawRssFailures'])
        result=subprocess.run(command,capture_output=True)
        self.assertNotEqual(0,result.returncode)
        self.assertEqual(original,output.read_bytes())


if __name__ == '__main__':
    unittest.main()
