#!/usr/bin/env python3
"""Fetch a fixed LLVM toolchain; GitHub asset digests verify downloads.
获取固定 LLVM 工具链；使用 GitHub 资产摘要验证下载。
"""
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERSION = json.loads((ROOT/'scripts/toolchains.json').read_text())['clang']
NAMES = {
    'Darwin': f'LLVM-{VERSION}-macOS-ARM64.tar.xz',
    'Linux': f'LLVM-{VERSION}-Linux-X64.tar.xz',
    'Windows': f'clang+llvm-{VERSION}-x86_64-pc-windows-msvc.tar.xz',
}
root = ROOT/'.cache'/'llvm'/platform.system()/VERSION
if not (root/'bin'/'clang.exe' if os.name == 'nt' else root/'bin'/'clang').is_file():
    metadata = json.loads(subprocess.check_output(['gh','api',f'repos/llvm/llvm-project/releases/tags/llvmorg-{VERSION}'], text=True))
    asset = next(a for a in metadata['assets'] if a['name'] == NAMES[platform.system()])
    archive = root.parent/asset['name']; archive.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['gh','release','download',f'llvmorg-{VERSION}','--repo','llvm/llvm-project','--pattern',asset['name'],'--dir',str(archive.parent),'--clobber'], check=True)
    digest = hashlib.sha256()
    with archive.open('rb') as stream:
        while chunk := stream.read(1024*1024): digest.update(chunk)
    if asset.get('digest') != 'sha256:'+digest.hexdigest(): raise SystemExit('LLVM checksum mismatch')
    extraction = root.parent/'extraction'; extraction.mkdir(exist_ok=True)
    with tarfile.open(archive) as bundle: bundle.extractall(extraction, filter='data')
    directories = list(extraction.iterdir())
    if len(directories) != 1: raise SystemExit('Unexpected LLVM archive layout')
    shutil.move(str(directories[0]), root); extraction.rmdir(); archive.unlink()
print(root/'bin')
if os.environ.get('GITHUB_PATH'):
    with open(os.environ['GITHUB_PATH'], 'a') as stream: stream.write(str(root/'bin')+'\n')

# Build-time SDK/runtime discovery also applies when the toolchain cache is hit.
# 工具链缓存命中时也需要配置构建期 SDK 与运行库路径。
environment = {}
if platform.system() == 'Darwin':
    environment['SDKROOT'] = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
elif platform.system() == 'Linux':
    directories = {str(root/'lib')}
    directories.update(str(path.parent) for path in root.rglob('libc++.so.1'))
    environment['LD_LIBRARY_PATH'] = ':'.join(sorted(directories)) + (':'+os.environ['LD_LIBRARY_PATH'] if os.environ.get('LD_LIBRARY_PATH') else '')
if os.environ.get('GITHUB_ENV'):
    with open(os.environ['GITHUB_ENV'], 'a') as stream:
        for name, value in environment.items(): stream.write(f'{name}={value}\n')
else:
    for name, value in environment.items(): print(f'{name}={value}')
