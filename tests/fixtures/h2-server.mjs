import http2 from 'node:http2';
import fs from 'node:fs';
const server = http2.createSecureServer({ key: fs.readFileSync(process.argv[2]), cert: fs.readFileSync(process.argv[3]), allowHTTP1: false });
server.on('sessionError', () => {});
server.on('stream', (stream, headers) => {
  stream.on('error', () => {});
  let body = '';
  stream.setEncoding('utf8'); stream.on('data', chunk => { body += chunk; });
  stream.on('end', () => {
    const data = JSON.stringify({ protocol: 'h2', method: headers[':method'], body });
    stream.respond({ ':status': 200, 'content-type': 'application/json', 'content-length': Buffer.byteLength(data) });
    stream.end(data);
  });
});
server.listen(0, '127.0.0.1', () => console.log(server.address().port));
