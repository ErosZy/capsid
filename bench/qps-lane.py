#!/usr/bin/env python3
"""Owned five-stack serving lane with distinct trickle and loaded processes."""
import argparse
from contextlib import contextmanager
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import statistics
import sys

from cold_control import cpu_selection
from cold_probe import write_json
from measurement_runtime import ProbeError, PortAllocator
from qps_control import WORKLOADS, checked_round, checked_memory_pair, identity
from qps_inputs import snapshot_fixtures, snapshot_python
from qps_probe import QpsProbe
from perf_evidence import (PROTOCOL, STACKS, required_binaries, relative_name, read_json, sha256, validate_lane)
from perf_run import (utc_now, machine_identity, run_metadata, session_lock, snapshot_runner,
                      recheck_runner, seal_archive, readable_manifest)


@contextmanager
def client_affinity(selected):
    original = os.sched_getaffinity(0)
    os.sched_setaffinity(0, selected)
    try: yield sorted(os.sched_getaffinity(0))
    finally: os.sched_setaffinity(0, original)


def binary_paths(args):
    paths = {'capsid-host': args.host, 'capsid-worker': args.worker, 'loadgen': args.loadgen,
             'workerd': args.workerd, 'python': args.python, 'php-fpm': args.php_fpm, 'nginx': args.nginx}
    required = set().union(*(required_binaries('qps', stack) for stack in args.stacks))
    return {name: paths[name] for name in sorted(required)}


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    build = Path(os.environ.get('BUILD_DIR', 'build-m1d'))
    for flag, env, default in (('host','HOST_BIN',build/'capsid-host'), ('worker','WORKER',build/'capsid-worker'),
            ('loadgen','LOADGEN',build/'benchmark-tools/loadgen'), ('workerd','WORKERD_BIN',shutil.which('workerd')),
            ('python','VENV_PYTHON',shutil.which('python3')), ('php-fpm','PHP_FPM_BIN',shutil.which('php-fpm8.3')),
            ('nginx','NGINX_BIN',shutil.which('nginx'))):
        parser.add_argument('--'+flag,type=Path,default=os.environ.get(env,default))
    parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--apps',type=Path,default=os.environ.get('BENCH_APPS',Path(__file__).with_name('bench-apps')))
    parser.add_argument('--out',type=Path,default=os.environ.get('OUT'))
    parser.add_argument('--metadata',type=Path,default=os.environ.get('CAPSID_BENCH_METADATA'))
    parser.add_argument('--cpuset',default=os.environ.get('SUT_CPUSET','0-3'))
    parser.add_argument('--stacks',nargs='+',default=os.environ.get('STACKS',' '.join(STACKS['qps'])).split())
    parser.add_argument('--workloads',nargs='+',default=os.environ.get('WORKLOADS',' '.join(WORKLOADS)).split())
    for flag,env,default in (('rounds','ROUNDS',5),('warmup','WARMUP',5),('duration','DURATION',15),
            ('connections','CONNECTIONS',64),('inflight','INFLIGHT',64),
            ('trickle-seconds','LOWQPS_SECONDS',15),('trickle-rate','LOWQPS_RATE',10),('request-timeout-ms','CAPSID_BENCH_TIMEOUT_MS',30000)):
        parser.add_argument('--'+flag,type=int,default=os.environ.get(env,default))
    parser.add_argument('--observation',action='store_true')
    args=parser.parse_args(argv)
    if sys.platform!='linux':parser.error('QPS process-tree PSS measurement requires Linux')
    if (not args.stacks or not set(args.stacks)<=set(STACKS['qps']) or len(args.stacks)!=len(set(args.stacks)) or
            not args.workloads or not set(args.workloads)<=set(WORKLOADS) or len(args.workloads)!=len(set(args.workloads))):
        parser.error('unknown or duplicate QPS stacks/workloads')
    if (args.warmup<0 or any(getattr(args,n)<=0 for n in ('rounds','duration','connections','inflight','trickle_seconds','trickle_rate','request_timeout_ms')) or
            args.rounds>1000 or args.warmup>3600 or args.duration>3600 or args.connections>65536 or args.inflight>65536 or
            args.trickle_seconds>3600 or args.trickle_rate>1000 or args.request_timeout_ms>300000):
        parser.error('QPS parameters must be positive and bounded; warmup may be zero')
    formal = (args.rounds,args.warmup,args.duration,args.connections,args.inflight,args.trickle_seconds,args.trickle_rate,args.request_timeout_ms)==(5,5,15,64,64,15,10,30000)
    if not args.observation and (args.metadata is None or not formal or set(args.stacks)!=set(STACKS['qps']) or set(args.workloads)!=set(WORKLOADS)):
        parser.error('formal QPS requires metadata and the complete frozen protocol; use --observation for restricted correctness smoke')
    if os.environ.get('CAPSID_MANAGED','1')!='1' or os.environ.get('CAPSID_STRICT','1')!='1':
        parser.error('QPS Capsid always uses managed/strict posture')
    try:selected=cpu_selection(args.cpuset,os.sched_getaffinity(0))
    except ProbeError as exc:parser.error(str(exc))
    if not args.observation and len(selected)!=4:parser.error('formal QPS requires four available CPU indices')
    args.repo,args.apps=args.repo.resolve(),args.apps.resolve()
    args.out=(args.out or args.repo/'bench/results'/('qps-'+utc_now().replace(':',''))).resolve()
    if args.out.exists():parser.error('OUT must be new; existing evidence is never truncated')
    if not args.observation and not args.out.is_relative_to(args.repo/'bench/results'):
        parser.error('formal QPS archives must live under bench/results')
    for name,path in binary_paths(args).items():
        if path is None or not path.is_file() or not os.access(path,os.X_OK):parser.error(f'missing executable for {name}: {path}')
    for attr in ('host','worker','loadgen','workerd','python','php_fpm','nginx'):
        value=getattr(args,attr)
        # Resolving a venv/bin/python symlink would discard the venv prefix.
        if value is not None:setattr(args,attr,value.absolute() if attr=='python' else value.resolve())
    return args


def run_lane(args):
    args._owns_output=False
    with session_lock(not args.observation) as lock:
        return run_locked(args,lock)


def run_locked(args,lock):
    paths=binary_paths(args)
    before={name:sha256(path) for name,path in paths.items()}
    metadata=None if args.observation else run_metadata(args.metadata,paths)
    selected=cpu_selection(args.cpuset,os.sched_getaffinity(0))
    if metadata is not None and len(selected)!=4:raise ProbeError('qps-affinity','formal QPS requires four CPU indices')
    args.out.mkdir(parents=True);args._owns_output=True
    started=utc_now()
    files=('qps-lane.sh','qps-lane.py','qps_probe.py','qps_control.py','qps_inputs.py','measurement_runtime.py',
        'density_probe.py','density_control.py','cold_probe.py','cold_control.py','perf_run.py','perf_evidence.py','collect-perf-point.py')
    runner,inputs=snapshot_runner(args.out,files=files)
    if metadata is not None:inputs[str(args.metadata.resolve())]=sha256(args.metadata)
    fixtures,fixture_inputs=snapshot_fixtures(args.apps,args.out,args.stacks);inputs.update(fixture_inputs)
    dependencies={}
    if 'python' in paths:
        dependencies['python'],extra=snapshot_python(paths['python'],args.out,args.stacks);inputs.update(extra)
    write_json(args.out/'runtime-dependencies.json',dependencies)
    command=shlex.join([sys.executable,'-B',str(Path(__file__).resolve()),'--out',str(args.out),'--repo',str(args.repo),
        '--apps',str(args.apps),'--cpuset',args.cpuset,'--stacks',*args.stacks,'--workloads',*args.workloads,
        *[v for key in ('rounds','warmup','duration','connections','inflight','trickle_seconds','trickle_rate','request_timeout_ms')
          for v in ('--'+key.replace('_','-'),str(getattr(args,key)))],
        *[v for name,path in paths.items() for v in ('--'+{'capsid-host':'host','capsid-worker':'worker'}.get(name,name),str(path))],
        *(['--observation'] if metadata is None else ['--metadata',str(args.metadata.resolve())])])
    write_json(args.out/'run.json',{'status':'running','startedAt':started,'command':command,'runner':runner,'observedBinarySHA256':before})
    lines,records,memory=[],[],{}
    with client_affinity(selected) as affinity:
        adapter=QpsProbe(binaries=paths,apps=args.out/'fixtures',out=args.out,cpuset=args.cpuset,workloads=args.workloads,
            rounds=args.rounds,warmup=args.warmup,duration=args.duration,connections=args.connections,inflight=args.inflight,
            trickle_seconds=args.trickle_seconds,trickle_rate=args.trickle_rate,request_timeout_ms=args.request_timeout_ms)
        allocator=PortAllocator()
        for stack in args.stacks:
            memory[stack]={}
            for phase in ('lowLoad','serving'):
                with allocator.allocate(2 if stack=='php' else 1) as lease:
                    result=adapter(stack=stack,phase=phase,ports=lease.ports,release_ports=lease.release)
                relative=relative_name(result['probeDirectory']+'/result.json')
                if read_json(args.out/relative)!=result:raise ProbeError('qps-proof','probe record differs from its actual result file')
                memory[stack][phase]=dict(result,resultFile=relative)
            checked_memory_pair(memory[stack],stack,trickle_seconds=args.trickle_seconds,trickle_rate=args.trickle_rate,
                                expected_clients=len(args.workloads)*args.rounds)
            serving=memory[stack]['serving']
            for phase in ('lowLoad','serving'):
                runtime=args.out/memory[stack][phase]['probeDirectory']/'runtime'
                for file in runtime.rglob('*'):
                    if file.is_file():
                        name=file.relative_to(args.out).as_posix()
                        fixtures[name]={'file':name,'sha256':sha256(file)};inputs[str(file)]=sha256(file)
            for workload in args.workloads:
                rounds=[r for r in serving['records'] if r['workload']==workload]
                if len(rounds)!=args.rounds or [r['round'] for r in rounds]!=list(range(1,args.rounds+1)):
                    raise ProbeError('qps-round','missing or duplicate per-workload round')
                for row in rounds:
                    sample=relative_name(row['samples']);correct=relative_name(row['correctness'])
                    raw=[json.loads(line) for line in (args.out/sample).read_text().splitlines()]
                    measured=checked_round(raw,read_json(args.out/correct),stack,workload,row['round'],warmup=args.warmup,
                        duration=args.duration,connections=args.connections,inflight=args.inflight,request_timeout_ms=args.request_timeout_ms)
                    if measured!=row['measurement']:raise ProbeError('qps-round','measurement differs from actual load-generator file')
                last=rounds[-1]['measurement']
                lines.append(f"{stack} {workload} qps_med={statistics.median([r['measurement']['qps'] for r in rounds]):.1f} p50_ms={last['p50_ms']:.9f} p95_ms={last['p95_ms']:.9f}")
                fixture_names=[n for n in fixtures if (n=='qps-worker.mjs' if stack in ('capsid','workerd') else
                    n==stack+'_app.py' if stack in ('fastapi','flask') else n.startswith('php-slim/')) or n.startswith('probes/'+stack+'/')]
                records.append({'stack':stack,'workload':workload,'samples':[r['samples'] for r in rounds],
                    'correctness':[r['correctness'] for r in rounds],'clientProcesses':[r['clientProcess'] for r in rounds],
                    'fixtures':fixture_names,'binaries':sorted(required_binaries('qps',stack))})
            for prefix,phase in (('lowqps','lowLoad'),('serving','serving')):
                sample=memory[stack][phase]
                detail=';'.join(f'{role}={kb/1024:.1f}' for role,kb in sample['roles_kb'].items())
                lines += [f"{stack} {prefix}_pss_mb={sample['pss_kb']/1024:.1f}",f"{stack} {prefix}_pss_detail={detail}"]
        if any(sha256(path)!=before[name] for name,path in paths.items()):raise ProbeError('binary-changed','QPS executables changed during measurement')
        recheck_runner(inputs)
        if metadata is not None:run_metadata(args.metadata,paths)
        (args.out/'cells.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8',newline='\n')
        write_json(args.out/'memory.json',memory)
        common={'status':'complete','startedAt':started,'completedAt':utc_now(),'command':command,
            'profile':'observation' if metadata is None else 'formal','machine':machine_identity(),'clientAffinity':affinity,
            'posture':{'platform':'linux','mode':'managed','sandbox':'strict'},'runner':runner,'observedBinarySHA256':before,
            'protocol':dict(PROTOCOL['qps'],sutCpuSet=args.cpuset,rounds=args.rounds,warmupSeconds=args.warmup,
                durationSeconds=args.duration,connections=args.connections,inflight=args.inflight,lowLoadSeconds=args.trickle_seconds,
                lowLoadRate=args.trickle_rate,requestTimeoutMilliseconds=args.request_timeout_ms)}
        write_json(args.out/'run.json',common)
        if metadata is None:
            write_json(args.out/'observation.json',common);seal_archive(args.out)
            return {'status':'complete','profile':'observation','cells':len(records),'publicationEligible':False}
        manifest=dict(common,schema=1,lane='qps',session=metadata['session'],origin=metadata['origin'],
            summary='cells.txt',isolation='isolation.json',binaries=metadata['binaries'],fixtures=fixtures,records=records,
            memory='memory.json',runtimeDependencies='runtime-dependencies.json')
        write_json(args.out/'run-metadata.json',metadata)
        write_json(args.out/'isolation.json',{'session':metadata['session'],'origin':metadata['origin'],
            'machine':common['machine'],'posture':common['posture'],'quietSession':True,'ownedPidsRemaining':0,'coordinationLock':lock})
        (args.out/'manifest.txt').write_text(readable_manifest(manifest),encoding='utf-8',newline='\n')
        write_json(args.out/'lane.json',manifest);seal_archive(args.out)
        spec=importlib.util.spec_from_file_location('qps_collector',Path(__file__).with_name('collect-perf-point.py'))
        collector=importlib.util.module_from_spec(spec);spec.loader.exec_module(collector)
        validate_lane(args.out,args.repo,'qps',collector.parse_qps(args.out),metadata['origin']['version'],metadata['origin']['release'])
        return {'status':'complete','profile':'formal','cells':len(records),'publicationEligible':True}


def main(argv=None):
    args=parse_args(argv)
    def stop(signum,_):raise ProbeError('interrupted','QPS controller was interrupted',signal=signum)
    signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop)
    try:result=run_lane(args)
    except BaseException as exc:
        if getattr(args,'_owns_output',False) and args.out.is_dir():
            error=exc.record() if isinstance(exc,ProbeError) else {'status':'error','error':{'type':type(exc).__name__,'message':str(exc)}}
            write_json(args.out/'failed.json',error)
            for name in ('run.json','lane.json'):
                if (args.out/name).is_file():write_json(args.out/name,dict(read_json(args.out/name),status='error',error=error['error']))
        raise
    print(json.dumps(result,sort_keys=True))


if __name__=='__main__':
    try:main()
    except (ProbeError,OSError,ValueError,KeyError,TypeError) as exc:
        print(f'QPS lane failed: {exc}',file=sys.stderr);sys.exit(1)
