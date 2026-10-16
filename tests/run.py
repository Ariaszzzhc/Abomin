#!/usr/bin/env python3
"""Exercise the compiled runtime as a subprocess. 使用独立进程验证运行时。"""
import argparse
import gzip
import http.server
import json
import threading
import time
import pathlib
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--binary', default=str(ROOT / 'build/macosx/arm64/debug/abomin'))
options, rest = parser.parse_known_args()
BINARY = str(pathlib.Path(options.binary).resolve())

class RuntimeTests(unittest.TestCase):
    def run_runtime(self, args, *, code=0, env=None):
        result = subprocess.run([BINARY, *map(str,args)], cwd=ROOT, capture_output=True, text=True, timeout=20, env=env)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return result

    def test_cli(self):
        self.assertIn('0.1.0', self.run_runtime(['--version']).stdout)
        self.assertIn('Usage:', self.run_runtime(['--help']).stdout)
        self.run_runtime(['run'], code=2)
        self.assertEqual(self.run_runtime(['eval', 'console.log(6*7)']).stdout.strip(), '42')

    def test_basic(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertIn('basic:ok', self.run_runtime(['run', 'tests/fixtures/basic.ts', pathlib.Path(directory)/'data.txt']).stdout)

    def test_workers(self):
        self.assertIn('workers:ok', self.run_runtime(['run', 'tests/fixtures/workers.ts']).stdout)

    def test_http(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *args): pass
            def respond(self, status, body=b'', headers=None):
                self.send_response(status)
                for name, value in (headers or {}).items(): self.send_header(name, value)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                if body: self.wfile.write(body)
            def do_GET(self):
                try:
                    if self.path == '/redirect': self.respond(302, headers={'Location':'/json'})
                    elif self.path == '/gzip': self.respond(200, gzip.compress('compressed\0你好'.encode()), {'Content-Encoding':'gzip'})
                    elif self.path == '/delay': time.sleep(.3); self.respond(200, b'delayed')
                    elif self.path == '/stream':
                        self.send_response(200); self.send_header('Content-Length', '11'); self.end_headers()
                        self.wfile.write(b'first'); self.wfile.flush(); time.sleep(.3); self.wfile.write(b'second')
                    else: self.respond(200, json.dumps({'method': 'GET'}).encode(), {'Content-Type':'application/json'})
                except (BrokenPipeError, ConnectionResetError): pass
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get('Content-Length','0')))
                self.respond(200, json.dumps({'body':body.decode(), 'header':self.headers.get('x-test')}).encode())
        class Server(http.server.ThreadingHTTPServer):
            request_queue_size = 64
            def handle_error(self, request, client_address): pass
        server = Server(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            self.assertIn('http:ok', self.run_runtime(['tests/fixtures/http.ts', f'http://localhost:{server.server_port}']).stdout)
        finally: server.shutdown(); server.server_close(); thread.join()

    @unittest.skipUnless(shutil.which('node') and shutil.which('openssl'), 'TLS fixtures require Node and OpenSSL')
    def test_tls_http2(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            def openssl(*args):
                subprocess.run(['openssl', *map(str,args)], check=True, capture_output=True, cwd=path)
            openssl('req','-x509','-newkey','rsa:2048','-nodes','-keyout','ca.key','-out','ca.pem','-days','1','-subj','/CN=Abomin Test CA','-addext','basicConstraints=critical,CA:TRUE')
            openssl('req','-newkey','rsa:2048','-nodes','-keyout','server.key','-out','server.csr','-subj','/CN=localhost')
            (path/'extensions').write_text('basicConstraints=critical,CA:FALSE\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n')
            openssl('x509','-req','-in','server.csr','-CA','ca.pem','-CAkey','ca.key','-CAcreateserial','-out','server.pem','-days','1','-extfile','extensions')
            server = subprocess.Popen(['node',str(ROOT/'tests/fixtures/h2-server.mjs'),str(path/'server.key'),str(path/'server.pem')], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                port = int(server.stdout.readline().strip())
                url = f'https://localhost:{port}/'
                env = os.environ.copy(); env.pop('ABOMIN_CA_FILE', None)
                self.assertIn('tls:rejected', self.run_runtime(['tests/fixtures/tls.ts',url,'reject'], env=env).stdout)
                env['ABOMIN_CA_FILE'] = str(path/'ca.pem')
                self.assertIn('tls:h2:ok', self.run_runtime(['tests/fixtures/tls.ts',url,'accept'], env=env).stdout)
            finally:
                server.terminate(); server.communicate(timeout=5)

    def test_unicode_module_path(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)/'模块 空间'; path.mkdir()
            (path/'dep space.ts').write_text('export const value: number = 42;', encoding='utf-8')
            entry = path/'entry%#测试.ts'
            entry.write_text('import { value } from "./dep space.ts"; if (value !== 42) throw new Error("path"); console.log("unicode:ok");', encoding='utf-8')
            self.assertIn('unicode:ok', self.run_runtime([entry]).stdout)

    def test_failures(self):
        self.run_runtime(['eval', 'throw new Error("expected")'], code=1)
        self.run_runtime(['eval', 'Promise.reject(new Error("unhandled"))'], code=1)
        self.run_runtime(['eval', 'addEventListener("unhandledrejection", e => e.preventDefault()); Promise.reject(1);'])
        self.run_runtime(['tests/fixtures/missing.ts'], code=1)
        with tempfile.TemporaryDirectory() as directory:
            entry = pathlib.Path(directory)/'syntax.ts'; entry.write_text('export const = ;')
            self.run_runtime([entry], code=1)
            entry.write_text('await new Promise(() => {});')
            self.assertIn('unresolved', self.run_runtime([entry], code=1).stderr)

if __name__ == '__main__':
    unittest.main(argv=['runtime-tests', *rest], verbosity=2)
