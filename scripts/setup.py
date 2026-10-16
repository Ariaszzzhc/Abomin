#!/usr/bin/env python3
"""Install the pinned JavaScriptCore SDK with SHA-256 verification.
下载固定版本 JavaScriptCore SDK，并验证 SHA-256。
"""
import argparse
import hashlib
import json
import pathlib
import platform
import shutil
import tarfile
import tempfile
import urllib.request

VERSION = '34c01d13391e00c06862a3d2c5b7fff350ac87e0'
ASSETS = {
    'macos-arm64': ('bun-webkit-macos-arm64.tar.gz', '719366f3426ce70652c0d8a0bcafcc236b7430e12553a1a947c8e630db934c7e'),
    'linux-amd64': ('bun-webkit-linux-amd64.tar.gz', '80994aadb37662dc66cae05212db7794d8366da99f9b406f1573c4e0537daeb5'),
    'windows-amd64': ('bun-webkit-windows-amd64.tar.gz', '1123e0ddeba5095c751512ee1fabe3a5315eeadb1553b011e94d423a3759d018'),
}
ROOT = pathlib.Path(__file__).resolve().parents[1]

def install():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', choices=ASSETS, default={'Darwin':'macos', 'Linux':'linux', 'Windows':'windows'}[platform.system()] + '-' + {'arm64':'arm64','aarch64':'arm64','x86_64':'amd64','AMD64':'amd64'}[platform.machine()])
    args = parser.parse_args()
    filename, checksum = ASSETS[args.platform]
    root = ROOT/'.cache'/'jsc'/VERSION/args.platform/'release'
    sdk = root/'bun-webkit'
    stamp = root/'verified.json'
    expected = {'version':VERSION, 'sha256':checksum, 'platform':args.platform}
    if stamp.is_file() and json.loads(stamp.read_text()) == expected and (sdk/'include/cmakeconfig.h').is_file():
        print(sdk)
        return
    root.mkdir(parents=True, exist_ok=True)
    archive = root/filename
    def valid():
        if not archive.is_file(): return False
        digest = hashlib.sha256()
        with archive.open('rb') as stream:
            while chunk := stream.read(1024*1024): digest.update(chunk)
        return digest.hexdigest() == checksum
    if not valid():
        url = f'https://github.com/oven-sh/WebKit/releases/download/autobuild-{VERSION}/{filename}'
        request = urllib.request.Request(url, headers={'User-Agent':'Abomin-build'})
        temporary = archive.with_suffix('.partial')
        with urllib.request.urlopen(request, timeout=120) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
        temporary.replace(archive)
        if not valid():
            archive.unlink()
            raise SystemExit('JavaScriptCore checksum mismatch')
    with tempfile.TemporaryDirectory(dir=root) as directory:
        with tarfile.open(archive, 'r:gz') as bundle:
            # Reject paths and links escaping the extraction directory. 拒绝逃逸解压目录的路径和链接。
            bundle.extractall(directory, filter='data')
        if sdk.exists(): shutil.rmtree(sdk)
        shutil.move(str(pathlib.Path(directory)/'bun-webkit'), sdk)
    stamp.write_text(json.dumps(expected, indent=2)+'\n')
    print(sdk)

if __name__ == '__main__': install()
