#!/usr/bin/env python3
"""Make an update bundle: a folder's files plus their MANIFEST, as a .tar.gz.

    make_update_bundle.py <folder> <output.tar.gz>

The editor's updater (tools/haumea/updatecore.cpp) installs exactly the
files MANIFEST lists ("<sha256>  <path>" per line) and refuses anything but
plain files and folders, so the archive is USTAR with no links. Entries are
sorted and dated 0, so the same files give the same archive.
"""
import gzip
import hashlib
import io
from pathlib import Path
import sys
import tarfile

folder, output = Path(sys.argv[1]), Path(sys.argv[2])
files = sorted(p for p in folder.rglob('*') if p.is_file() and not p.is_symlink())
if not files:
    raise SystemExit(f'{folder} has no files')
manifest = ''
for f in files:
    rel = f.relative_to(folder).as_posix()
    if rel == 'MANIFEST' or rel.startswith('.haumea-'):
        raise SystemExit(f'{rel} is a name the updater keeps for itself')
    manifest += hashlib.sha256(f.read_bytes()).hexdigest() + '  ' + rel + '\n'

raw = io.BytesIO()
with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT) as tar:
    def add(name, data, executable=False):
        info = tarfile.TarInfo(name)
        info.size = len(data)
        info.mode = 0o755 if executable else 0o644
        info.mtime = 0
        tar.addfile(info, io.BytesIO(data))
    add('MANIFEST', manifest.encode())
    for f in files:
        add(f.relative_to(folder).as_posix(), f.read_bytes(),
            executable=f.name in ('haumea', 'haumea.exe', 'haumea-d3d9.exe') or (f.stat().st_mode & 0o111) != 0)
with open(output, 'wb') as out:
    with gzip.GzipFile(fileobj=out, mode='wb', mtime=0, filename='') as gz:
        gz.write(raw.getvalue())
print(f'{output}: {len(files)} files')
