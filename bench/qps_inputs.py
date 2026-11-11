"""Read-only QPS fixtures and installed Python framework provenance."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

from measurement_runtime import ProbeError
from perf_evidence import sha256


def snapshot_fixtures(apps, out, stacks):
    canonical = Path(__file__).with_name('bench-apps')
    base = []
    if {'capsid', 'workerd'} & set(stacks): base.append('qps-worker.mjs')
    for stack in ('fastapi', 'flask'):
        if stack in stacks: base.append(stack + '_app.py')
    if 'php' in stacks: base += ['php-slim/public/index.php', 'php-slim/composer.json']
    for name in base:
        if (apps / name).read_bytes() != (canonical / name).read_bytes():
            raise ProbeError('qps-fixture', 'fixture differs from the maintained QPS workload', file=name)
    names = set(base)
    if 'php' in stacks:
        if not (apps / 'php-slim/vendor/autoload.php').is_file() or not (apps / 'php-slim/composer.lock').is_file():
            raise ProbeError('qps-fixture', 'PHP requires its installed vendor tree and composer.lock')
        for file in (apps / 'php-slim').rglob('*'):
            if file.is_symlink(): raise ProbeError('qps-fixture', 'fixture symlinks are not supported')
            if file.is_file(): names.add(file.relative_to(apps).as_posix())
    fixtures, inputs = {}, {}
    for name in sorted(names):
        source = apps / name
        if source.is_symlink(): raise ProbeError('qps-fixture', 'fixture symlinks are not supported')
        data = source.read_bytes()
        target = out / 'fixtures' / name
        target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
        value = hashlib.sha256(data).hexdigest()
        fixtures[name] = {'file': target.relative_to(out).as_posix(), 'sha256': value}
        inputs[str(target)] = value
    return fixtures, inputs


def snapshot_python(binary, out, stacks):
    """Snapshot the actual venv files; -I keeps inherited Python paths out.

    -B plus a fresh CLI pycache prefix on serving processes makes installed
    .pyc files irrelevant. Package files are copied once, before any traffic.
    """
    code = '''import hashlib,importlib.metadata as m,importlib.util,json,pathlib,sys
prefix=pathlib.Path(sys.prefix).resolve()
packages=[];files=[]
for d in m.distributions():
 name=d.metadata['Name'];packages.append({'name':name,'version':d.version})
 for f in d.files or []:
  p=pathlib.Path(d.locate_file(f))
  if '__pycache__' in p.parts or not p.is_file():continue
  if p.is_symlink() or not p.resolve().is_relative_to(prefix):raise RuntimeError('package file outside interpreter prefix')
  files.append({'package':name,'logicalPath':str(f),'path':str(p.absolute()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
origins={n:importlib.util.find_spec(n).origin if importlib.util.find_spec(n) else None for n in ('fastapi','uvicorn','flask','gunicorn')}
print(json.dumps({'version':sys.version,'prefix':str(prefix),'packages':packages,'files':files,'origins':origins}))
'''
    env = dict(os.environ); env.pop('PYTHONPATH', None); env.pop('PYTHONHOME', None)
    run = subprocess.run([str(binary), '-I', '-B', '-c', code], capture_output=True, env=env, timeout=60)
    if run.returncode: raise ProbeError('qps-python', 'cannot fingerprint Python framework installation', stderr=run.stderr.decode(errors='replace'))
    observed = json.loads(run.stdout)
    required = ({'fastapi', 'uvicorn', 'starlette'} if 'fastapi' in stacks else set()) | ({'flask', 'gunicorn'} if 'flask' in stacks else set())
    normalize = lambda s: s.lower().replace('_', '-').replace('.', '-')
    if not required <= {normalize(p['name']) for p in observed['packages']}:
        raise ProbeError('qps-python', 'required reference framework packages are missing')
    known = {str(Path(f['path']).resolve()) for f in observed['files']}
    for name in required - {'starlette'}:
        origin = observed['origins'].get(name)
        if origin is None or str(Path(origin).resolve()) not in known:
            raise ProbeError('qps-python', 'imported framework is not in the installed package inventory', package=name)
    inputs, files = {}, []
    for item in observed.pop('files'):
        source = Path(item['path']); data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != item['sha256']:
            raise ProbeError('qps-python', 'installed Python package changed while being snapshotted')
        target = out / 'dependencies/python' / item['sha256']
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists(): target.write_bytes(data)
        files.append({k: item[k] for k in ('package', 'logicalPath', 'sha256')} | {'file': target.relative_to(out).as_posix()})
        inputs[str(source)] = inputs[str(target)] = item['sha256']
    observed.update(files=files, executableSHA256=sha256(binary))
    return observed, inputs
