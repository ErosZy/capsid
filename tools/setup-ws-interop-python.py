"""Install the digest-pinned Python and Node WebSocket test implementations."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import urllib.request
import venv
import tarfile
import io
import shutil

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('environment',type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parent.parent
lock=json.loads((root/'tests/fixtures/websocket-interop-lock.json').read_text())
if platform.python_implementation()!=lock['python']['implementation'] or platform.python_version()!=lock['python']['version']:
    parser.error('use the locked '+lock['python']['implementation']+' '+lock['python']['version'])
environment=args.environment.resolve()
if environment.exists() and not (environment/'pyvenv.cfg').exists():
    parser.error('destination already exists and is not a Python virtual environment')
wheel=lock['websockets']['wheel']
cache=environment.parent/(environment.name+'-wheel')
cache.mkdir(parents=True,exist_ok=True)
archive=cache/wheel['filename']
with urllib.request.urlopen(wheel['url'],timeout=30) as response:
    data=response.read(wheel['size']+1)
if len(data)!=wheel['size'] or hashlib.sha256(data).hexdigest()!=wheel['sha256']:
    raise SystemExit('locked WebSocket wheel size or SHA256 mismatch')
archive.write_bytes(data)
node_archive=lock['ws']['tarball']
with urllib.request.urlopen(node_archive['url'],timeout=30) as response:
    node_data=response.read(node_archive['size']+1)
if len(node_data)!=node_archive['size'] or hashlib.sha256(node_data).hexdigest()!=node_archive['sha256'] or 'sha512-'+base64.b64encode(hashlib.sha512(node_data).digest()).decode()!=node_archive['integrity']:
    raise SystemExit('locked Node ws tarball identity mismatch')
node_package=environment/'node-ws'
if node_package.exists():
    if not (node_package/'package.json').exists():
        parser.error('Node ws destination is not a package managed by this setup')
else:
    with tarfile.open(fileobj=io.BytesIO(node_data),mode='r:gz') as tar:
        for member in tar.getmembers():
            relative=Path(member.name)
            if relative.is_absolute() or '..' in relative.parts or relative.parts[0]!='package' or not (member.isdir() or member.isfile()):
                raise SystemExit('unexpected Node ws archive entry')
        for member in tar.getmembers():
            destination=node_package.joinpath(*Path(member.name).parts[1:])
            if member.isdir():destination.mkdir(parents=True,exist_ok=True)
            else:
                destination.parent.mkdir(parents=True,exist_ok=True)
                destination.write_bytes(tar.extractfile(member).read())
venv.EnvBuilder(with_pip=True).create(environment)
python=environment/('Scripts/python.exe' if sys.platform=='win32' else 'bin/python')
subprocess.run([str(python),'-I','-m','pip','install','--disable-pip-version-check',
                '--no-deps','--no-compile','--no-index','--force-reinstall',str(archive)],check=True)
subprocess.run([str(python),'-I',str(root/'tests/helpers/ws_interop_python.py'),'identity'],check=True)
node=shutil.which('node')
if node is None:
    parser.error('locked Node runtime must be on PATH')
subprocess.run([node,str(root/'tests/helpers/ws_interop_node.mjs'),'identity'],check=True,
               env={**os.environ,'CAPSID_WS_INTEROP_NODE_PACKAGE':str(node_package)})
print('CAPSID_WS_INTEROP_PYTHON='+str(python))
print('CAPSID_WS_INTEROP_NODE_PACKAGE='+str(node_package))
