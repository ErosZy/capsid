# Verify the owned CPack archive before prerelease artifact upload/publication.
from pathlib import Path,PurePosixPath
import hashlib,json,os,subprocess,tarfile,tempfile,zipfile
source=Path.cwd();records=[]
for archive in sorted([*source.glob('build/capsid-*.tar.gz'),*source.glob('build/capsid-*.zip'),*source.glob('build-musl/capsid-*.tar.gz')]):
 raw=archive.read_bytes();digest=hashlib.sha256(raw).hexdigest();sidecar=Path(str(archive)+'.sha256');fields=sidecar.read_text().split();assert fields[0].lower()==digest and fields[-1].lstrip('*')==archive.name
 scratch=Path(tempfile.mkdtemp(prefix='capsid-beta-package-',dir=os.environ['RUNNER_TEMP']));stem=archive.name[:-7] if archive.name.endswith('.tar.gz') else archive.stem
 if archive.suffix=='.zip':
  with zipfile.ZipFile(archive) as z:
   for info in z.infolist():
    n=PurePosixPath(info.filename);assert not n.is_absolute() and '..' not in n.parts and '\\' not in info.filename;assert (info.external_attr>>16)&0o170000!=0o120000
   z.extractall(scratch)
 else:
  with tarfile.open(archive,'r:gz') as t:
   for info in t.getmembers():
    n=PurePosixPath(info.name);assert not n.is_absolute() and '..' not in n.parts and '\\' not in info.name;assert info.isfile() or info.isdir(),info.name
   t.extractall(scratch)
 roots=[p for p in scratch.iterdir() if p.is_dir()];assert len(roots)==1;root=roots[0];info=(root/'share/capsid/build-info.txt').read_text();assert 'commit='+os.environ['PRODUCT_SHA'] in info and 'treeClean=true' in info and 'version=0.3.0' in info
 script=scratch/'verify.cmake';script.write_text('cmake_minimum_required(VERSION 3.20)\ninclude("'+(source/'cmake/ValidatePackageMetadata.cmake').as_posix()+'")\ncapsid_validate_package_metadata("'+root.as_posix()+'" "'+source.as_posix()+'" "'+stem+'")\n');subprocess.run(['cmake','-P',str(script)],check=True)
 facts=dict(line.split('=',1) for line in info.splitlines() if '=' in line);compat=facts['compatibilityId'];assert compat.startswith('sha256:') and len(compat)==71
 binaries={};suffix='.exe' if os.name=='nt' else ''
 for name in ['capsid-host','capsid-worker','capsid-bytecode-compile']:
  p=root/'bin'/(name+suffix);assert p.is_file();data=p.read_bytes();assert compat.encode() in data,(name,'embedded compatibility identity');binaries[name]=hashlib.sha256(data).hexdigest()
  if name=='capsid-bytecode-compile':
   v=subprocess.run([str(p),'--print-compatibility-id'],capture_output=True,timeout=20);assert v.returncode==0 and v.stdout.decode().strip()==compat
 sbom=root/'share/capsid/SBOM.spdx.json';records.append(dict(archive=str(archive.relative_to(source)),sha256=digest,bytes=len(raw),sbomSHA256=hashlib.sha256(sbom.read_bytes()).hexdigest(),sourceCommit=os.environ['PRODUCT_SHA'],binaries=binaries,metadataValidatorPassed=True))
assert len(records)==1,records
Path(os.environ.get('CAPSID_PACKAGE_REVIEW_OUT','beta-package-review.json')).write_text(json.dumps(dict(complete=True,records=records,publicationPerformed=False),indent=2)+'\n')
print(json.dumps(records,indent=2))
