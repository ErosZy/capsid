"""Actual owned HTTP/process/CLI fixtures; no reference-stack performance claims."""
import argparse
import http.server
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

parser=argparse.ArgumentParser(add_help=False)
parser.add_argument('--loadgen',type=Path,required=True)
parser.add_argument('--probe',type=Path,default=Path(__file__).resolve().parents[1]/'bench/qps_probe.py')
parser.add_argument('--peer',action='store_true');parser.add_argument('--port',type=int)
parser.add_argument('--records',type=Path);parser.add_argument('--fork-one',action='store_true')
parser.add_argument('--bad-after-ready',action='store_true');parser.add_argument('--bad-ready',action='store_true')
args,remaining=parser.parse_known_args();sys.argv[1:]=remaining
sys.path.insert(0,str(args.probe.resolve().parent))
from qps_probe import QpsProbe
from density_probe import DensityProbe, admin_request
from measurement_runtime import ProcFS,PortAllocator,OwnedProcess,OwnedGroup,ProbeError
from qps_control import checked_memory_pair


def record_identity():
    value=ProcFS().identity(os.getpid())
    fd=os.open(args.records,os.O_WRONLY|os.O_CREAT|os.O_APPEND,0o600)
    try:os.write(fd,(json.dumps({'pid':value.pid,'start':value.start})+'\n').encode())
    finally:os.close(fd)


def peer():
    record_identity()
    class Server(http.server.ThreadingHTTPServer):
        daemon_threads=True;request_queue_size=128
    count=[0]
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*_):pass
        def do_GET(self):
            count[0]+=1
            route=self.path.rsplit('/',1)[-1]
            body=(b'b'*341+b'c'*341+b'd'*342) if route=='stream' else json.dumps(
                {'data':'x'*((1024 if route=='matrix-json-1k' else 16384)-11)},separators=(',',':')).encode()
            if args.bad_ready or args.bad_after_ready and count[0]>1:body=body[:100]+b'!'+body[101:]
            self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers()
            try:self.wfile.write(body)
            except (BrokenPipeError,ConnectionResetError):pass
    server=Server(('127.0.0.1',args.port),Handler)
    if args.fork_one and os.fork()==0:
        record_identity()
        while True:time.sleep(1)
    server.serve_forever(poll_interval=.01)


if args.peer:
    peer();raise SystemExit(0)


def command(port,records,*extra):
    return [sys.executable,'-B',str(Path(__file__).resolve()),'--loadgen',str(args.loadgen.resolve()),
        '--probe',str(args.probe.resolve()),'--peer','--port',str(port),'--records',str(records),*extra]


def gone(records):
    proc=ProcFS()
    return all((p:=proc.identity(row['pid'])) is None or p.start!=row['start'] for row in records)


def wait_for(condition,timeout=20):
    deadline=time.monotonic()+timeout
    while not condition():
        if time.monotonic()>=deadline:return False
        time.sleep(.01)
    return True


@unittest.skipUnless(sys.platform == 'linux', 'owned benchmark probes require Linux')
class AdminReadinessTests(unittest.TestCase):
    """Real Unix bind/listen boundaries, not performance or synthetic lane data."""
    def exercise(self, kind, refuse_post=False):
        import socketserver
        with tempfile.TemporaryDirectory(prefix='capsid-admin-probe-') as temp:
            folder=Path(temp);(folder/'run').mkdir()
            address=folder/'run/admin.sock';requests=[];threads=[]
            class Handler(http.server.BaseHTTPRequestHandler):
                protocol_version='HTTP/1.1'
                def log_message(self,*_):pass
                def do_POST(self):self.reply()
                def do_GET(self):self.reply()
                def reply(self):
                    self.rfile.read(int(self.headers.get('Content-Length','0')))
                    requests.append((self.command,self.path))
                    body=b'{"operationId":"one"}' if self.command=='POST' else b'{"active":true}'
                    self.send_response(202 if self.command=='POST' else 200)
                    self.send_header('Content-Length',str(len(body)))
                    self.send_header('Connection','close');self.end_headers();self.wfile.write(body)
            server=socketserver.UnixStreamServer(str(address),Handler,bind_and_activate=False)
            server.server_bind()
            refused=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
            refused.bind(str(folder/'run/refuse.sock'))
            real_socket=socket.socket;refusals=[]
            def activate():
                server.server_activate()
                thread=threading.Thread(target=server.serve_forever,kwargs={'poll_interval':.01},daemon=True)
                thread.start();threads.append(thread)
            class ObservedSocket(real_socket):
                def connect(self,target):
                    try:return super().connect(target)
                    except ConnectionRefusedError:
                        if str(target)==str(address) and not threads:
                            refusals.append(target);activate()
                        raise
            tree=SimpleNamespace(alive=lambda:True,discover=lambda:[object(),object()],listeners_owned=lambda ports:True)
            if kind=='qps':
                adapter=QpsProbe(binaries={},apps=folder,out=folder,cpuset='0',workloads=['stream'])
                invoke=lambda:adapter.wait_ready(None,[('host','worker',tree,[41000])],folder,'capsid',41000)
                module='qps_probe'
            else:
                adapter=DensityProbe(host=Path(sys.executable),worker=Path(sys.executable),workerd=Path(sys.executable),
                    apps=folder,out=folder,cpuset='0')
                invoke=lambda:adapter.wait_ready(tree,folder,'capsid',[41000])
                module='density_probe'
            posts=[]
            def business(path,method,route,*values,**options):
                if method=='POST':
                    posts.append(route)
                    if refuse_post and len(posts)==1:path=folder/'run/refuse.sock'
                return admin_request(path,method,route,*values,**options)
            try:
                if refuse_post:activate()
                with patch('density_probe.socket.socket',ObservedSocket),patch(module+'.admin_request',business),\
                        patch('qps_probe.exact_request',return_value={'ok':True}):
                    if refuse_post:
                        with self.assertRaises(ConnectionRefusedError):invoke()
                        self.assertEqual(['/v1/deploy'],posts)
                        self.assertEqual([],requests)
                    else:
                        invoke()
                        self.assertEqual(1,len(refusals),'the actual pre-listen refusal must precede activation')
                        self.assertEqual(['/v1/deploy'],posts)
                        self.assertEqual([('POST','/v1/deploy'),('GET','/v1/apps/orders')],requests)
            finally:
                if threads:server.shutdown()
                server.server_close();refused.close()
                for thread in threads:thread.join(2);self.assertFalse(thread.is_alive())

    def test_readiness_waits_for_listen_not_just_the_bound_file(self):
        for kind in ('qps','density'):
            with self.subTest(kind=kind):self.exercise(kind)

    def test_business_connection_refusal_is_never_retried(self):
        for kind in ('qps','density'):
            with self.subTest(kind=kind):self.exercise(kind,refuse_post=True)


class LaunchConfigurationTests(unittest.TestCase):
    def test_private_reference_workers_keep_controller_identity(self):
        apps=Path(__file__).resolve().parents[1]/'bench/bench-apps'
        binaries={'php-fpm':Path('/usr/sbin/php-fpm8.3'),'nginx':Path('/usr/sbin/nginx'),
                  'python':Path(sys.executable)}
        with tempfile.TemporaryDirectory(prefix='capsid-reference-config-') as temp:
            root=Path(temp)
            probe=QpsProbe(binaries=binaries,apps=apps,out=root,cpuset='0',workloads=['stream'])
            for uid in (0,1000):
                folder=root/str(uid);folder.mkdir(mode=0o700)
                with patch('qps_probe.os.geteuid',return_value=uid):
                    specs=probe.prepare(folder,'php',[48001,48002])
                fpm=next(row for row in specs if row[0]=='fpm-master')
                self.assertEqual(uid==0,'--allow-to-run-as-root' in fpm[2])
                self.assertEqual(uid==0,'user = root\ngroup = root\n' in (folder/'php-fpm.conf').read_text())
                self.assertEqual(uid==0,'user root;\n' in (folder/'nginx.conf').read_text())
                self.assertIn('pm.max_children = 1',(folder/'php-fpm.conf').read_text())
                self.assertEqual([48002],fpm[3])
            flask=root/'flask';flask.mkdir(mode=0o700)
            specs=probe.prepare(flask,'flask',[48003])
            self.assertTrue((flask/'run').is_dir())
            self.assertIn(str(flask/'run/gunicorn.ctl'),specs[0][2])


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='capsid-qps-process-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.out=self.root/'out';self.out.mkdir()
        self.records=self.root/'processes.jsonl';self.cpuset=str(min(os.sched_getaffinity(0)))

    def rows(self):
        return [json.loads(line) for line in self.records.read_text().splitlines()] if self.records.exists() else []

    def adapter(self,php=False,bad=''):
        records=self.records
        class Fixture(QpsProbe):
            def prepare(self,folder,stack,ports):
                extra=['--'+bad] if bad else []
                if php:
                    return [('fpm-master','fpm-worker',command(ports[1],records,'--fork-one',*extra),[ports[1]]),
                            ('nginx','nginx-worker',command(ports[0],records,'--fork-one',*extra),[ports[0]])]
                return [('process','process',command(ports[0],records,*extra),ports)]
        return Fixture(binaries={'loadgen':args.loadgen.resolve()},apps=self.root,out=self.out,cpuset=self.cpuset,
            workloads=['stream'],rounds=1,warmup=1,duration=1,connections=2,inflight=2,
            trickle_seconds=1,trickle_rate=10,settle_seconds=.05,pss_interval=.05)

    def test_two_root_forest_samples_serving_only_and_reaps_every_generation(self):
        adapter=self.adapter(php=True);allocator=PortAllocator();phases={}
        for phase in ('lowLoad','serving'):
            with allocator.allocate(2) as lease:
                phases[phase]=adapter(stack='php',phase=phase,ports=lease.ports,release_ports=lease.release)
        checked_memory_pair(phases,'php',trickle_seconds=1,trickle_rate=10,expected_clients=1)
        self.assertEqual(8,len(self.rows()));self.assertTrue(gone(self.rows()))
        measured={p['pid'] for p in phases['serving']['measurementProcesses']}
        self.assertEqual(4,len(measured));self.assertNotIn(phases['serving']['loadgenProcesses'][0]['pid'],measured)
        self.assertEqual({'nginx','nginx-worker','fpm-master','fpm-worker'},set(phases['serving']['roles_kb']))

    def test_bad_readiness_and_failed_actual_loadgen_cannot_leave_processes(self):
        for bad,phase in (('bad-ready','lowLoad'),('bad-after-ready','serving')):
            with self.subTest(bad=bad):
                target=self.out/bad;target.mkdir();adapter=self.adapter(bad=bad);adapter.out=target
                with PortAllocator().allocate(1) as lease,self.assertRaises(ProbeError):
                    adapter(stack='workerd',phase=phase,ports=lease.ports,release_ports=lease.release)
                result=json.loads((target/'probes/workerd'/phase/'result.json').read_text())
                self.assertEqual('error',result['status']);self.assertEqual(0,result['cleanup']['ownedPidsRemaining'])
                self.assertTrue(gone(self.rows()));self.assertFalse(OwnedProcess._active)

    def wrapper(self,name):
        wrapper=self.root/(name+'.py');records=self.root/(name+'.pids')
        # Keep the executable's arguments explicit; no shell or inherited PID.
        prefix=[sys.executable,'-B',str(Path(__file__).resolve()),'--loadgen',str(args.loadgen.resolve()),
                '--probe',str(args.probe.resolve()),'--peer','--records',str(records)]
        wrapper.write_text('#!'+sys.executable+'\nfrom pathlib import Path\nimport os,re,sys\n'
            'text=Path(sys.argv[-1]).read_text()\nport=int(re.search(r"127\\.0\\.0\\.1:(\\d+)",text)[1])\n'
            'os.execv('+repr(sys.executable)+','+repr(prefix)+'+["--port",str(port)])\n')
        wrapper.chmod(0o700);return wrapper,records

    def cli(self,out,wrapper,*extra):
        return [sys.executable,'-B',str(args.probe.with_name('qps-lane.py').resolve()),'--observation','--out',str(out),
            '--stacks','workerd','--workloads','stream','--workerd',str(wrapper),'--loadgen',str(args.loadgen.resolve()),
            '--cpuset',self.cpuset,'--rounds','1','--warmup','1','--duration','1','--connections','2','--inflight','2',
            '--trickle-seconds','1','--trickle-rate','10',*extra]

    def test_two_public_controllers_have_distinct_ports_logs_and_owned_processes(self):
        launched=[];outer=OwnedGroup();logs=[]
        try:
            for name in ('first','second'):
                wrapper,records=self.wrapper(name);out=self.root/name/'observation'
                stdout_path=self.root/(name+'.stdout');stderr_path=self.root/(name+'.stderr')
                stdout=stdout_path.open('wb');stderr=stderr_path.open('wb');logs.extend((stdout,stderr))
                tree=outer.spawn(self.cli(out,wrapper),stdout=stdout,stderr=stderr)
                launched.append((tree,out,records,stdout_path,stderr_path))
            def finished():
                for tree,*_ in launched:tree.discover()
                return all(not tree.alive() for tree,*_ in launched)
            self.assertTrue(wait_for(finished,45))
            port_sets=[]
            for tree,out,records,stdout_path,stderr_path in launched:
                tree.close()
                self.assertEqual(0,tree.process.returncode,stderr_path.read_text(errors='replace'))
                self.assertEqual(1,json.loads(stdout_path.read_text())['cells']);self.assertFalse((out/'lane.json').exists())
                memory=json.loads((out/'memory.json').read_text())['workerd']
                self.assertTrue(all(p['cleanup']['ownedPidsRemaining']==0 for p in memory.values()))
                self.assertTrue(gone([json.loads(line) for line in records.read_text().splitlines()]))
                port_sets.append({port for phase in ('lowLoad','serving') for port in json.loads((out/'probes/workerd'/phase/'launch.json').read_text())['ports']})
            self.assertFalse(port_sets[0]&port_sets[1])
        finally:
            outer.close()
            for log in logs:log.close()

    def test_public_signals_during_measured_traffic_reap_sut_and_loadgen_before_outer_cleanup(self):
        for sig in (signal.SIGINT,signal.SIGTERM):
            with self.subTest(signal=sig):
                wrapper,records=self.wrapper(sig.name);out=self.root/(sig.name+'-out')
                outer=OwnedGroup();logs=(self.root/(sig.name+'.log')).open('wb')
                tree=outer.spawn(self.cli(out,wrapper,'--duration','15'),stdout=logs)
                try:
                    sample=out/'probes/workerd/serving/stream.r1.jsonl'
                    def measured():
                        tree.discover();return sample.exists() and sample.stat().st_size>0
                    self.assertTrue(wait_for(measured))
                    tree.process.send_signal(sig)
                    def exited():
                        tree.discover();return not tree.alive()
                    self.assertTrue(wait_for(exited,12));tree.close()
                    self.assertEqual(1,tree.process.returncode,(self.root/(sig.name+'.log')).read_text(errors='replace'))
                    self.assertEqual('interrupted',json.loads((out/'failed.json').read_text())['error']['code'])
                    cleanup=json.loads((out/'probes/workerd/serving/cleanup.json').read_text())
                    self.assertEqual(0,cleanup['ownedPidsRemaining']);self.assertGreaterEqual(len(cleanup['trees']),2)
                    observed=[p for tree in cleanup['trees'] for p in tree['processes']]
                    self.assertTrue(gone(observed));self.assertFalse((out/'cells.txt').exists());self.assertFalse((out/'lane.json').exists())
                finally:
                    outer.close();logs.close()


if __name__=='__main__':unittest.main()
