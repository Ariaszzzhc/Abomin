#!/usr/bin/env python3
"""Verify that the executable can start without build-machine library paths.
验证可执行文件不依赖构建机的动态库路径即可启动。
"""
import argparse
import os
import pathlib
import platform
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('binary')
args = parser.parse_args()
binary = str(pathlib.Path(args.binary).resolve())
environment = os.environ.copy()
for name in ('LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH', 'DYLD_FALLBACK_LIBRARY_PATH'):
    environment.pop(name, None)
if platform.system() == 'Darwin':
    output = subprocess.check_output(['otool', '-L', binary], text=True)
    for line in output.splitlines()[1:]:
        dependency = line.strip().split(' (')[0]
        assert dependency.startswith(('/usr/lib/', '/System/Library/')), output
elif platform.system() == 'Linux':
    output = subprocess.check_output(['ldd', binary], text=True, env=environment)
    assert 'not found' not in output, output
    assert not re.search(r'libc\+\+(?:abi)?\.so|libbrotli|libuv\.so|libicu', output), output
else:
    output = subprocess.check_output(['dumpbin', '/dependents', binary], text=True)
    assert not re.search(r'(?i)(?:msvcp|vcruntime)\d+.*\.dll', output), output
print(output)
subprocess.run([binary, '--version'], env=environment, check=True, timeout=10)
result = subprocess.run([binary, 'eval', 'console.log(1+2)'], env=environment, text=True, capture_output=True, check=True, timeout=10)
assert result.stdout.strip() == '3', result
print('Standalone executable check passed')
