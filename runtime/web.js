/** Web platform primitives backed by native text, URL and crypto services.
 * 使用原生文本、URL 与密码学服务的 Web 基础接口。 */
function toBytes(value) {
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  throw new TypeError('Expected BufferSource');
}
class TextEncoder {
  get encoding() { return 'utf-8'; }
  encode(input = '') { return new Uint8Array(native.encode(String(input))); }
  encodeInto(input, destination) {
    if (!(destination instanceof Uint8Array)) throw new TypeError('Expected Uint8Array');
    input = String(input); let read = 0, written = 0;
    for (const char of input) {
      const bytes = this.encode(char);
      if (written + bytes.length > destination.length) break;
      destination.set(bytes, written); written += bytes.length; read += char.length;
    }
    return { read, written };
  }
}
class TextDecoder {
  #decoder; #label; #fatal; #ignoreBOM;
  constructor(label = 'utf-8', options = {}) {
    this.#label = native.encodingLabel(String(label)); this.#fatal = Boolean(options.fatal); this.#ignoreBOM = Boolean(options.ignoreBOM);
  }
  get encoding() { return this.#label; } get fatal() { return this.#fatal; } get ignoreBOM() { return this.#ignoreBOM; }
  decode(input = new Uint8Array(), options = {}) {
    this.#decoder ??= native.decoder(this.#label, this.#ignoreBOM);
    try { return native.decoderPush(this.#decoder, toBytes(input), this.#fatal, !options.stream); }
    finally { if (!options.stream) this.#decoder = undefined; }
  }
}
class TextEncoderStream {
  constructor() {
    let tail = '';
    const encoder = new TextEncoder();
    const stream = new TransformStream({
      transform(chunk, controller) {
        let text = tail + String(chunk); tail = '';
        const last = text.charCodeAt(text.length - 1);
        if (last >= 0xd800 && last <= 0xdbff) { tail = text.slice(-1); text = text.slice(0, -1); }
        if (text) controller.enqueue(encoder.encode(text));
      }, flush(controller) { if (tail) controller.enqueue(encoder.encode(tail)); }
    });
    this.readable = stream.readable; this.writable = stream.writable;
  }
  get encoding() { return 'utf-8'; }
}
class TextDecoderStream {
  constructor(label, options) {
    const decoder = new TextDecoder(label, options); this.encoding = decoder.encoding; this.fatal = decoder.fatal; this.ignoreBOM = decoder.ignoreBOM;
    const stream = new TransformStream({ transform(chunk, c) { const text = decoder.decode(chunk, { stream: true }); if (text) c.enqueue(text); }, flush(c) { const text = decoder.decode(); if (text) c.enqueue(text); } });
    this.readable = stream.readable; this.writable = stream.writable;
  }
}
function btoa(text) {
  text = String(text); const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) { const n = text.charCodeAt(i); if (n > 255) throw new DOMException('Invalid character', 'InvalidCharacterError'); bytes[i] = n; }
  return new TextDecoder().decode(native.base64(bytes, false));
}
function atob(text) {
  text = String(text).replace(/[\t\n\f\r ]/g, '');
  if (text.length % 4 === 0) text = text.replace(/={1,2}$/, '');
  if (text.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(text)) throw new DOMException('Invalid Base64', 'InvalidCharacterError');
  text += '='.repeat((4 - text.length % 4) % 4);
  const data = new Uint8Array(native.base64(new TextEncoder().encode(text), true));
  let result = ''; for (const byte of data) result += String.fromCharCode(byte); return result;
}
const formEncode = value => encodeURIComponent(String(value).toWellFormed()).replace(/[!'()~]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase()).replace(/%20/g, '+');
function formDecode(value) {
  value = value.replace(/\+/g, ' '); const bytes = [];
  for (let i = 0; i < value.length;) {
    if (value[i] === '%' && /^[\da-f]{2}$/i.test(value.slice(i + 1, i + 3))) { bytes.push(parseInt(value.slice(i + 1, i + 3), 16)); i += 3; }
    else { const c = String.fromCodePoint(value.codePointAt(i)); bytes.push(...new TextEncoder().encode(c)); i += c.length; }
  }
  return new TextDecoder().decode(new Uint8Array(bytes));
}
class URLSearchParams {
  #pairs = []; _update;
  constructor(init = '') {
    if (typeof init === 'string') this._parse(init);
    else if (init != null && typeof init[Symbol.iterator] === 'function') {
      for (const entry of init) { const pair = [...entry]; if (pair.length !== 2) throw new TypeError('Expected name/value pair'); this.#pairs.push(pair.map(v => String(v).toWellFormed())); }
    } else for (const key of Object.keys(init)) this.#pairs.push([key.toWellFormed(), String(init[key]).toWellFormed()]);
  }
  _parse(text) { this.#pairs = String(text).replace(/^\?/, '').split('&').filter(Boolean).map(p => { const i = p.indexOf('='); return [formDecode(i < 0 ? p : p.slice(0, i)), formDecode(i < 0 ? '' : p.slice(i + 1))]; }); }
  #changed() { this._update?.(this.toString()); }
  get size() { return this.#pairs.length; }
  append(name, value) { this.#pairs.push([String(name).toWellFormed(), String(value).toWellFormed()]); this.#changed(); }
  delete(name, value) { name = String(name); this.#pairs = this.#pairs.filter(p => p[0] !== name || (value !== undefined && p[1] !== String(value))); this.#changed(); }
  get(name) { return this.#pairs.find(p => p[0] === String(name))?.[1] ?? null; }
  getAll(name) { return this.#pairs.filter(p => p[0] === String(name)).map(p => p[1]); }
  has(name, value) { return this.#pairs.some(p => p[0] === String(name) && (value === undefined || p[1] === String(value))); }
  set(name, value) {
    name = String(name).toWellFormed(); value = String(value).toWellFormed(); let found = false;
    this.#pairs = this.#pairs.filter(p => { if (p[0] !== name) return true; if (found) return false; p[1] = value; found = true; return true; });
    if (!found) this.#pairs.push([name, value]); this.#changed();
  }
  sort() { this.#pairs.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0); this.#changed(); }
  *entries() { yield* this.#pairs.map(p => [...p]); } *keys() { for (const p of this.#pairs) yield p[0]; } *values() { for (const p of this.#pairs) yield p[1]; }
  [Symbol.iterator]() { return this.entries(); }
  forEach(fn, self) { for (const [key, value] of this.#pairs) fn.call(self, value, key, this); }
  toString() { return this.#pairs.map(p => p.map(formEncode).join('=')).join('&'); }
}
class URL {
  #parts; #params;
  constructor(input, base) { this.#parts = native.url(String(input), base === undefined ? undefined : String(base)); this.#params = new URLSearchParams(this.#parts.search); this.#params._update = text => this.#set('search', text); }
  #set(key, value) { const parts = native.url(this.href, undefined, key, String(value)); if (parts) { this.#parts = parts; this.#params._parse(parts.search); } }
  get href() { return this.#parts.href; } set href(v) { this.#parts = native.url(String(v)); this.#params._parse(this.#parts.search); }
  get origin() { return this.#parts.origin; } get searchParams() { return this.#params; }
  toString() { return this.href; } toJSON() { return this.href; }
  static canParse(input, base) { try { new URL(input, base); return true; } catch { return false; } }
  static parse(input, base) { try { return new URL(input, base); } catch { return null; } }
  static _property(key) { Object.defineProperty(URL.prototype, key, { enumerable: true, configurable: true, get() { return this.#parts[key]; }, set(value) { this.#set(key, value); } }); }
}
for (const key of ['protocol', 'username', 'password', 'host', 'hostname', 'port', 'pathname', 'search', 'hash']) URL._property(key);
class Blob {
  #bytes; #type;
  constructor(parts = [], options = {}) {
    const chunks = [...parts].map(p => p instanceof Blob ? p._bytes() : p instanceof ArrayBuffer || ArrayBuffer.isView(p) ? toBytes(p).slice() : new TextEncoder().encode(String(p)));
    this.#bytes = new Uint8Array(chunks.reduce((n, b) => n + b.length, 0)); let offset = 0;
    for (const chunk of chunks) { this.#bytes.set(chunk, offset); offset += chunk.length; }
    const type = String(options.type ?? ''); this.#type = /[^\x20-\x7e]/.test(type) ? '' : type.toLowerCase();
  }
  _bytes() { return this.#bytes.slice(); }
  get size() { return this.#bytes.length; } get type() { return this.#type; }
  slice(start = 0, end = this.size, type = '') { return new Blob([this.#bytes.slice(start, end)], { type }); }
  async arrayBuffer() { return this.#bytes.slice().buffer; } async bytes() { return this.#bytes.slice(); }
  async text() { return new TextDecoder().decode(this.#bytes); }
  stream() { const bytes = this.#bytes; let offset = 0; return new ReadableStream({ type: 'bytes', pull(c) { if (offset >= bytes.length) c.close(); else { const chunk = bytes.slice(offset, offset + 65536); offset += chunk.length; c.enqueue(chunk); } } }); }
}
class File extends Blob {
  constructor(parts, name, options = {}) { super(parts, options); this.name = String(name).toWellFormed(); this.lastModified = Number(options.lastModified ?? Date.now()); }
}
class FormData {
  #pairs = [];
  #value(value, filename) { return value instanceof Blob ? new File([value], filename ?? (value instanceof File ? value.name : 'blob'), { type: value.type, lastModified: value.lastModified }) : String(value).toWellFormed(); }
  append(name, value, filename) { this.#pairs.push([String(name).toWellFormed(), this.#value(value, filename)]); }
  set(name, value, filename) { const replacement = this.#value(value, filename); name = String(name); const i = this.#pairs.findIndex(p => p[0] === name); this.delete(name); this.#pairs.splice(i < 0 ? this.#pairs.length : i, 0, [name, replacement]); }
  delete(name) { this.#pairs = this.#pairs.filter(p => p[0] !== String(name)); }
  get(name) { return this.#pairs.find(p => p[0] === String(name))?.[1] ?? null; }
  getAll(name) { return this.#pairs.filter(p => p[0] === String(name)).map(p => p[1]); }
  has(name) { return this.#pairs.some(p => p[0] === String(name)); }
  *entries() { yield* this.#pairs; } *keys() { for (const [k] of this.#pairs) yield k; } *values() { for (const [,v] of this.#pairs) yield v; }
  [Symbol.iterator]() { return this.entries(); } forEach(fn, self) { for (const [key, value] of this.#pairs) fn.call(self, value, key, this); }
}
class Crypto {
  getRandomValues(array) {
    if (!ArrayBuffer.isView(array) || array instanceof Float32Array || array instanceof Float64Array || array instanceof DataView) throw new DOMException('Expected integer typed array', 'TypeMismatchError');
    if (array.byteLength > 65536) throw new DOMException('Quota exceeded', 'QuotaExceededError');
    native.random(array); return array;
  }
  randomUUID() { const bytes = this.getRandomValues(new Uint8Array(16)); bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128; const h = [...bytes].map(n => n.toString(16).padStart(2, '0')).join(''); return `${h.slice(0,8)}-${h.slice(8,12)}-${h.slice(12,16)}-${h.slice(16,20)}-${h.slice(20)}`; }
  subtle = Object.freeze({ async digest(algorithm, data) { const name = String(typeof algorithm === 'string' ? algorithm : algorithm.name).toUpperCase(); const id = { 'SHA-1': 1, 'SHA-256': 256, 'SHA-384': 384, 'SHA-512': 512 }[name]; if (!id) throw new DOMException('Unsupported digest', 'NotSupportedError'); return asyncOp("digestAsync", id, toBytes(data)); } });
}
function compression(format, decompress) {
  if (!['gzip', 'deflate', 'deflate-raw', 'brotli'].includes(format)) throw new TypeError('Unsupported compression format');
  const state = native.compression(format, decompress);
  return new TransformStream({ transform(chunk, c) { const data = new Uint8Array(native.compressionPush(state, toBytes(chunk), false)); if (data.length) c.enqueue(data); }, flush(c) { const data = new Uint8Array(native.compressionPush(state, new Uint8Array(), true)); if (data.length) c.enqueue(data); } });
}
class CompressionStream { constructor(format) { const s = compression(String(format), false); this.readable = s.readable; this.writable = s.writable; } }
class DecompressionStream { constructor(format) { const s = compression(String(format), true); this.readable = s.readable; this.writable = s.writable; } }
expose({ TextEncoder, TextDecoder, TextEncoderStream, TextDecoderStream, atob, btoa, URL, URLSearchParams, Blob, File, FormData, Crypto, crypto: new Crypto(), CompressionStream, DecompressionStream });
