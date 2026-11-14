#!/usr/bin/env python3
"""Host regressions for coreutils provenance and ARM optional output staging."""
import hashlib
import io
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent

def run(*args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, **kwargs)

with tempfile.TemporaryDirectory() as td:
    root = Path(td)
    (root / 'tools').mkdir()
    cache = root / 'vendor/coreutils'
    cache.mkdir(parents=True)
    shutil.copy(ROOT / 'tools/fetch-coreutils.sh', root / 'tools/fetch-coreutils.sh')
    (root / 'COREUTILS_VERSION').write_text('test\n')
    package = cache / 'coreutils-test.hpkg'
    with tarfile.open(package, 'w') as tar:
        for name, data in [('manifest', b'id=coreutils\nversion=test\n'), ('bin/ls', b'new tools')]:
            item = tarfile.TarInfo(name)
            item.size = len(data)
            tar.addfile(item, io.BytesIO(data))
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    lock = root / 'tools/coreutils.sha256'
    lock.write_text(f'test {digest}\n')
    result = run('sh', str(root / 'tools/fetch-coreutils.sh'))
    assert result.returncode == 0, result.stderr
    assert (cache / 'bin/ls').read_bytes() == b'new tools'
    stamp = (cache / '.fetched').stat().st_mtime_ns
    package.write_bytes(package.read_bytes() + b'tampered')
    result = run('sh', str(root / 'tools/fetch-coreutils.sh'))
    assert result.returncode != 0, 'accepted corrupt cache'
    assert (cache / 'bin/ls').read_bytes() == b'new tools'
    assert (cache / '.fetched').stat().st_mtime_ns == stamp
    lock.write_text('')
    assert run('sh', str(root / 'tools/fetch-coreutils.sh')).returncode != 0
    print('coreutils: valid pin accepted; tampering/unpinned version rejected; old payload preserved')

with tempfile.TemporaryDirectory() as td:
    root = Path(td)
    blobs = root / 'blobs'
    blobs.mkdir()
    stage = root / 'stage/bin'
    stage.mkdir(parents=True)
    source = root / 'source/user/bin/hostname'
    source.mkdir(parents=True)
    (source / 'main.c').write_text('bad source')
    (blobs / 'hostname.bin').write_text('stale hostname')
    (blobs / 'vigictl.bin').write_text('stale missing-source tool')
    # Compiler may leave an output before failing; that must not be staged either.
    compiler = root / 'compiler'
    compiler.write_text('#!/bin/sh\nwhile [ "$1" != -o ]; do shift; done\nshift\nprintf partial > "$1"\nexit 1\n')
    compiler.chmod(0o755)
    src = (ROOT / 'tools/build-arm64-server.sh').read_text()
    section = src[src.index('INSTALL_TOOLS='):src.index('# /bin/sh')]
    staging = src[src.index('for t in "${SIMPLE_TOOLS[@]}" "${INSTALL_TOOLS[@]}"; do', src.index('# 4c.')):src.index('[ -f "$BLOBS/sh.bin" ]')]
    script = root / 'test.sh'
    script.write_text(f'''set -euo pipefail
REPO={root / 'source'}
BLOBS={blobs}
STAGE={root / 'stage'}
CC={compiler}
STRIP=true
SIMPLE_TOOLS=(hostname vigictl)
log() {{ echo "$*"; }}
{section}
{staging}
true
''')
    result = run('bash', str(script))
    assert result.returncode == 0, result.stderr
    assert not (stage / 'hostname').exists(), 'failed compile shipped stale/partial hostname'
    assert not (stage / 'vigictl').exists(), 'missing source shipped stale vigictl'
    print('ARM staging: failed/partial builds and missing sources cannot ship old binaries')
