"""QPS producer contracts; synthetic adapters never produce real performance data."""
import argparse
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

parser=argparse.ArgumentParser(add_help=False)
parser.add_argument('--lane',type=Path,default=Path(__file__).resolve().parents[1]/'bench/qps-lane.py')
args,remaining=parser.parse_known_args();sys.argv[1:]=remaining
sys.path.insert(0,str(args.lane.resolve().parent))
sys.path.insert(0,str(Path(__file__).resolve().parent/'helpers'))
spec=importlib.util.spec_from_file_location('qps_lane_under_test',args.lane)
lane=importlib.util.module_from_spec(spec);spec.loader.exec_module(lane)
from qps_measurement_fixture import synthetic_memory
from measurement_runtime import ProbeError


def clean_environment():
    return {k:v for k,v in os.environ.items() if k not in ('OUT','STACKS','WORKLOADS','ROUNDS','WARMUP','DURATION',
        'CONNECTIONS','INFLIGHT','CAPSID_BENCH_METADATA','CAPSID_MANAGED','CAPSID_STRICT')}


class PreflightTests(unittest.TestCase):
    def test_missing_metadata_incomplete_protocol_and_nonlinux_never_launch(self):
        with tempfile.TemporaryDirectory(prefix='capsid-qps-preflight-') as temp:
            root=Path(temp);marker=root/'executed';fake=root/'executable'
            fake.write_text('#!'+sys.executable+'\nfrom pathlib import Path\nPath('+repr(str(marker))+').touch()\n');fake.chmod(0o700)
            argv=[sys.executable,'-B',str(args.lane.resolve()),'--out',str(root/'out'),'--workerd',str(fake)]
            for extra in ([],['--metadata',str(root/'unused'),'--stacks','workerd'],['--rounds','3']):
                run=subprocess.run(argv+extra,capture_output=True,text=True,env=clean_environment(),timeout=10)
                self.assertEqual(2,run.returncode);self.assertIn('formal QPS requires' if sys.platform=='linux' else 'requires Linux',run.stderr)
                self.assertFalse(marker.exists());self.assertFalse((root/'out').exists())

    def test_invalid_posture_protocol_and_existing_output_never_execute(self):
        with tempfile.TemporaryDirectory(prefix='capsid-qps-preflight-') as temp:
            root=Path(temp);out=root/'out';out.mkdir();sentinel=out/'keep';sentinel.write_bytes(b'prior evidence')
            cpu=str(min(os.sched_getaffinity(0))) if sys.platform=='linux' else '0'
            argv=[sys.executable,'-B',str(args.lane.resolve()),'--observation','--out',str(out),'--cpuset',cpu]
            for extra,env in (([],clean_environment()),(['--rounds','-1'],clean_environment()),
                              ([],dict(clean_environment(),CAPSID_MANAGED='0'))):
                run=subprocess.run(argv+extra,capture_output=True,env=env,timeout=10)
                self.assertEqual(2,run.returncode);self.assertEqual(b'prior evidence',sentinel.read_bytes())
                self.assertEqual([sentinel],list(out.iterdir()))


def raw_fixture(stack,workload,round_no,warmup,duration,connections,inflight,timeout):
    body=(b'b'*341+b'c'*341+b'd'*342) if workload=='stream' else json.dumps(
        {'data':'x'*((1024 if workload=='matrix-json-1k' else 16384)-11)},separators=(',',':')).encode()
    claims={'workload':workload,'response_validation':'exact-bytes-v2','payload_bytes':len(body),
            'expected_body_sha256':hashlib.sha256(body).hexdigest()}
    rows=[dict(claims,side=stack,round=round_no,phase=phase,duration_s=seconds,completed=seconds*100,
        requests_attempted=seconds*100,responses_checked=seconds*100,errors=0,timeouts=0,
        connections=connections,inflight=inflight,request_timeout_ms=timeout,qps=100.,p50_ms=1.,p95_ms=2.,p99_ms=3.)
        for phase,seconds in (('warmup',warmup),('measured',duration))]
    total=(warmup+duration)*100
    return rows,dict(claims,ok=True,errors_as_success=False,errors=0,timeouts=0,mismatches=0,
                     requests_attempted=total,responses_checked=total)


@unittest.skipUnless(sys.platform=='linux','formal QPS producer and process provenance require Linux')
class ProducerTests(unittest.TestCase):
    def setUp(self):
        available=sorted(os.sched_getaffinity(0))
        if len(available)<4:self.skipTest('formal protocol requires four available CPUs')
        self.affinity=set(available);self.addCleanup(lambda:self.assertEqual(self.affinity,os.sched_getaffinity(0)))
        self.temp=tempfile.TemporaryDirectory(prefix='capsid-qps-producer-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.source=self.root/'source';self.source.mkdir()
        # Synthetic archives have no measured processes. Keep the real flock
        # implementation, with a private path so parallel contract suites do
        # not contend with each other or with a real performance session.
        lock_directory=patch('perf_run.temp_directory',return_value=str(self.root))
        lock_directory.start();self.addCleanup(lock_directory.stop)
        (self.source/'CMakeLists.txt').write_text('project(capsid_runtime VERSION test LANGUAGES C)\n')
        env={k:v for k,v in os.environ.items() if not k.startswith('GIT_')};env.update(GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL=os.devnull)
        def git(*argv):
            run=subprocess.run(['git','-C',str(self.source),*argv],capture_output=True,text=True,env=env)
            self.assertEqual(0,run.returncode,run.stderr);return run.stdout.strip()
        git('init','--quiet');git('config','user.name','QPS fixture');git('config','user.email','qps-fixture@example.invalid')
        git('add','CMakeLists.txt');git('commit','--quiet','-m','synthetic fixture')
        origin={'commit':git('rev-parse','HEAD'),'version':'test','release':'v-test','tag':'','clean':True}
        self.metadata={'schema':1,'sourceRoot':str(self.source),'origin':origin,'machine':lane.machine_identity(),
            'session':'synthetic-qps-session','quietSession':True,'binaries':{}}
        executable=Path(sys.executable)
        for name in ('capsid-host','capsid-worker','loadgen','workerd','python','php-fpm','nginx'):
            self.metadata['binaries'][name]={k:origin[k] for k in ('commit','tag','version')}
            self.metadata['binaries'][name]['sha256']=lane.sha256(executable)
        self.meta=self.root/'metadata.json';self.meta.write_text(json.dumps(self.metadata))
        self.out=self.root/'bench/results/qps';self.apps=self.root/'read-only-apps';self.apps.mkdir()
        self.marker=self.apps/'keep';self.marker.write_bytes(b'shared inputs remain read-only')
        self.argv=['--repo',str(self.root),'--out',str(self.out),'--apps',str(self.apps),'--metadata',str(self.meta),
            '--cpuset',','.join(map(str,available[:4])),*[v for flag in ('host','worker','loadgen','workerd','python','php-fpm','nginx')
                                                      for v in ('--'+flag,str(executable))]]
        self.calls=[];self.hook=None
        outer=self
        class Adapter:
            def __init__(self,**kwargs):self.__dict__.update(kwargs)
            def __call__(self,*,stack,phase,ports,release_ports):
                release_ports();outer.calls.append((stack,phase,list(ports)))
                directory=self.out/'probes'/stack/phase;directory.mkdir(parents=True)
                value=synthetic_memory(stack,phase,seconds=self.trickle_seconds,rate=self.trickle_rate,
                                       clients=len(self.workloads)*self.rounds)
                value['probeDirectory']=directory.relative_to(self.out).as_posix()
                value['fixtureScope']='synthetic adapter; no kernel process or performance measurement'
                if phase=='serving':
                    value['records']=[]
                    for workload in self.workloads:
                        for number in range(1,self.rounds+1):
                            raw,correct=raw_fixture(stack,workload,number,self.warmup,self.duration,self.connections,self.inflight,self.request_timeout_ms)
                            stem=f'{workload}.r{number}';sample=directory/(stem+'.jsonl');verdict=directory/(stem+'.correctness.json')
                            sample.write_text(''.join(json.dumps(row)+'\n' for row in raw));verdict.write_text(json.dumps(correct))
                            client=value['loadgenProcesses'][len(value['records'])]
                            value['records'].append({'workload':workload,'round':number,'samples':sample.relative_to(self.out).as_posix(),
                                'correctness':verdict.relative_to(self.out).as_posix(),'measurement':raw[1],'clientProcess':client})
                if outer.hook:outer.hook(value,self)
                (directory/'result.json').write_text(json.dumps(value));return value
        self.adapter=Adapter

    def fixture_snapshot(self,apps,out,stacks):
        fixtures,inputs={},{}
        for name in ('qps-worker.mjs','fastapi_app.py','flask_app.py','php-slim/public/index.php'):
            path=out/'fixtures'/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'explicitly synthetic fixture')
            fixtures[name]={'file':path.relative_to(out).as_posix(),'sha256':lane.sha256(path)};inputs[str(path)]=lane.sha256(path)
        return fixtures,inputs

    def python_snapshot(self,binary,out,stacks):
        path=out/'dependencies/python/synthetic';path.parent.mkdir(parents=True);path.write_bytes(b'synthetic package fixture')
        names=('fastapi','uvicorn','starlette','flask','gunicorn')
        return {'version':'synthetic interpreter','executableSHA256':lane.sha256(binary),
            'packages':[{'name':n,'version':'synthetic'} for n in names],
            'files':[{'package':n,'logicalPath':'synthetic-module','file':path.relative_to(out).as_posix(),'sha256':lane.sha256(path)} for n in names]}, {str(path):lane.sha256(path)}

    def run_main(self):
        with (patch.object(lane,'QpsProbe',self.adapter),patch.object(lane,'snapshot_fixtures',self.fixture_snapshot),
              patch.object(lane,'snapshot_python',self.python_snapshot),redirect_stdout(io.StringIO())):
            lane.main(self.argv)

    def test_full_fifteen_cells_seventy_five_clients_seal_and_real_validator(self):
        self.run_main()
        manifest=json.loads((self.out/'lane.json').read_text());self.assertEqual(15,len(manifest['records']))
        self.assertEqual(75,sum(len(r['samples']) for r in manifest['records']))
        self.assertEqual(10,len(self.calls));self.assertTrue(all(a[2]!=b[2] for a,b in zip(self.calls,self.calls[1:])))
        self.assertEqual(b'shared inputs remain read-only',self.marker.read_bytes())
        collector_spec=importlib.util.spec_from_file_location('fixture_qps_collector',args.lane.with_name('collect-perf-point.py'))
        collector=importlib.util.module_from_spec(collector_spec);collector_spec.loader.exec_module(collector)
        lane.validate_lane(self.out,self.root,'qps',collector.parse_qps(self.out),'test','v-test')
        self.assertFalse((self.out/'failed.json').exists())
        seal={line.split('  ',1)[1]:line.split('  ',1)[0] for line in (self.out/'sha256sums.txt').read_text().splitlines()}
        self.assertTrue(all(lane.sha256(self.out/n)==v for n,v in seal.items()))

    def test_second_phase_failure_does_not_publish_a_partial_cell(self):
        def fail(value,adapter):
            if value['phase']=='serving':raise ProbeError('injected','synthetic serving failure')
        self.hook=fail
        with self.assertRaises(ProbeError) as raised:self.run_main()
        self.assertEqual('injected',raised.exception.code,'the serving phase must supply the failure')
        self.assertEqual(['lowLoad','serving'],[phase for _,phase,_ in self.calls])
        self.assertTrue((self.out/'failed.json').is_file());self.assertFalse((self.out/'cells.txt').exists());self.assertFalse((self.out/'lane.json').exists())

    def test_private_session_lock_still_rejects_overlap_and_is_released(self):
        import perf_run
        with perf_run.session_lock(True):
            with self.assertRaises(ProbeError) as raised:
                with perf_run.session_lock(True):self.fail('concurrent synthetic session accepted')
            self.assertEqual('session-overlap',raised.exception.code)
        with perf_run.session_lock(True) as state:self.assertTrue(state['held'])

    def test_corrupt_raw_result_cannot_be_replaced_by_a_success_summary(self):
        def fail(value,adapter):
            if value['phase']=='serving':
                file=self.out/value['records'][0]['samples'];rows=[json.loads(line) for line in file.read_text().splitlines()]
                rows[1]['errors']=1;file.write_text(''.join(json.dumps(row)+'\n' for row in rows))
        self.hook=fail
        with self.assertRaises(ProbeError):self.run_main()
        self.assertFalse((self.out/'cells.txt').exists());self.assertTrue((self.out/'failed.json').exists())

    def test_dirty_source_or_wrong_binary_is_rejected_before_any_probe(self):
        (self.source/'changed').write_bytes(b'dirty fixture source')
        with self.assertRaises(ProbeError):self.run_main()
        self.assertFalse(self.calls);self.assertFalse(self.out.exists())
        (self.source/'changed').unlink();self.metadata['binaries']['loadgen']['sha256']='0'*64
        self.meta.write_text(json.dumps(self.metadata))
        with self.assertRaises(ProbeError):self.run_main()
        self.assertFalse(self.calls);self.assertFalse(self.out.exists())

    def test_changed_frozen_input_is_rejected_before_sealing(self):
        def mutate(value,adapter):
            if value['phase']=='serving':(self.out/'fixtures/qps-worker.mjs').write_bytes(b'changed frozen input')
        self.hook=mutate
        with self.assertRaises(ProbeError):self.run_main()
        self.assertFalse((self.out/'lane.json').exists());self.assertFalse((self.out/'cells.txt').exists())


if __name__=='__main__':unittest.main()
