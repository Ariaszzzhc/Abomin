/** File handles serialize operations and close only after in-flight I/O.
 * 文件句柄串行执行操作，等待在途 I/O 后关闭。 */
class FsFile {
  #rid; #tail = Promise.resolve(); #closed = false;
  constructor(rid) { this.#rid = rid; }
  #queue(action) {
    if (this.#closed) return Promise.reject(new Error('File is closed'));
    const promise = this.#tail.then(action); this.#tail = promise.catch(() => {}); return promise;
  }
  read(destination) {
    if (!(destination instanceof Uint8Array)) return Promise.reject(new TypeError('Expected Uint8Array'));
    if (!destination.length) return Promise.resolve(0);
    return this.#queue(async () => { const data = new Uint8Array(await asyncOp('read', this.#rid, Math.min(destination.length, 16 * 1024 * 1024))); destination.set(data); return data.length || null; });
  }
  write(data) { const copy = toBytes(data).slice(); return this.#queue(() => asyncOp('write', this.#rid, copy)); }
  close() { if (this.#closed) return this.#tail; this.#closed = true; const promise = this.#tail.then(() => native.closeFile(this.#rid)); this.#tail = promise; return promise; }
  [Symbol.asyncDispose]() { return this.close(); }
  get readable() { const file = this; return new ReadableStream({ async pull(c) { const data = new Uint8Array(65536); try { const n = await file.read(data); if (n === null) { await file.close(); c.close(); } else c.enqueue(data.slice(0,n)); } catch (e) { await file.close(); c.error(e); } }, cancel() { return file.close(); } }); }
  get writable() { const file = this; return new WritableStream({ async write(data) { let offset = 0; data = toBytes(data); while (offset < data.length) { const n = await file.write(data.subarray(offset, offset + 16 * 1024 * 1024)); if (!n) throw new Error('Write made no progress'); offset += n; } }, close() { return file.close(); }, abort() { return file.close(); } }); }
}
function filePath(value) {
  if (value instanceof URL) { if (value.protocol !== 'file:') throw new TypeError('Expected file URL'); let path = decodeURIComponent(value.pathname); if (/^\/[a-z]:\//i.test(path)) path = path.slice(1); return path; }
  value = String(value); if (value.includes('\0')) throw new TypeError('Invalid path'); return value;
}
async function open(path, options = {}) { const mode = options.append ? 'a' : options.write ? 'w' : 'r'; return new FsFile(await asyncOp('open', filePath(path), mode)); }
async function readFile(path, options = {}) {
  options.signal?.throwIfAborted(); const file = await open(path); const chunks = []; let size = 0;
  try { while (true) { options.signal?.throwIfAborted(); const data = new Uint8Array(65536), n = await file.read(data); if (n === null) break; chunks.push(data.slice(0,n)); size += n; if (size > 256 * 1024 * 1024) throw new RangeError('readFile exceeds 256 MiB; use open().readable'); } }
  finally { await file.close(); }
  const result = new Uint8Array(size); let offset = 0; for (const chunk of chunks) { result.set(chunk, offset); offset += chunk.length; } return result;
}
async function writeFile(path, data, options = {}) {
  options.signal?.throwIfAborted(); data = toBytes(data).slice(); const file = await open(path, { write: true, append: options.append });
  try { let offset = 0; while (offset < data.length) { options.signal?.throwIfAborted(); const n = await file.write(data.subarray(offset, offset + 16 * 1024 * 1024)); if (!n) throw new Error('Write made no progress'); offset += n; } }
  finally { await file.close(); }
}
const Abomin = Object.freeze({
  version: config.version, args: Object.freeze(config.args), mainModule: config.entry,
  cwd: () => native.cwd(), env: Object.freeze({ get: name => native.env(String(name)) }),
  exit: (code = 0) => native.exit(Number(code)), open, readFile, writeFile,
  readTextFile: async (path, options) => new TextDecoder().decode(await readFile(path, options)),
  writeTextFile: (path, data, options) => writeFile(path, new TextEncoder().encode(data), options),
  stat: path => asyncOp('stat', filePath(path)), remove: path => asyncOp('remove', filePath(path)), mkdir: path => asyncOp('mkdir', filePath(path)),
});
expose({ Abomin });
