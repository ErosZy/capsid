from pathlib import Path
import hashlib,json,os,subprocess
out=Path('build-perf-prerequisite/evidence');out.mkdir(parents=True,exist_ok=True)
def command(args,label,check=True):
 r=subprocess.run(args,capture_output=True,text=True,timeout=120)
 (out/(label+'.stdout.log')).write_text(r.stdout);(out/(label+'.stderr.log')).write_text(r.stderr)
 if check:assert r.returncode==0,(label,r.returncode)
 return r
before=command(['perf','stat','-e','task-clock','true'],'before-task-clock',False) if __import__('shutil').which('perf') else None
command(['sudo','sysctl','-w','kernel.perf_event_paranoid=-1'],'configure-perf-access')
packages=subprocess.check_output(['dpkg-query','-W','-f=${Package}\n','linux-tools-*'],text=True).splitlines()
binaries=set()
for package in packages:
 for name in subprocess.check_output(['dpkg-query','-L',package],text=True).splitlines():
  p=Path(name)
  if p.name=='perf' and '/usr/lib/' in name and p.is_file() and os.access(p,os.X_OK):binaries.add(p.resolve())
assert binaries
tools=Path(os.environ['RUNNER_TEMP'])/'capsid-perf-tools';tools.mkdir();chosen=None
for index,p in enumerate(sorted(binaries)):
 if command([str(p),'stat','-e','task-clock,context-switches,cpu-migrations,page-faults','true'],'actual-counters-'+str(index),False).returncode==0:
  chosen=p;break
assert chosen,'actual perf software counters must be available'
(tools/'perf').symlink_to(chosen)
with open(os.environ['GITHUB_PATH'],'a') as stream:stream.write(str(tools)+'\n')
record=dict(complete=True,selectedActualPerf=str(chosen),sha256=hashlib.sha256(chosen.read_bytes()).hexdigest(),
 version=command([str(chosen),'--version'],'actual-version').stdout.strip(),actualCountersPassed=True,
 beforeTaskClockCode=None if before is None else before.returncode,formalPerformanceNotClaimed=True)
(out/'prerequisite.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
