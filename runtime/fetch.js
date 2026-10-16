/** Fetch owns response resources and pulls one native body chunk at a time.
 * Fetch 拥有响应资源，每次拉取一个原生 body 数据块。 */
const headerName = name => { name = String(name).toLowerCase(); if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(name)) throw new TypeError('Invalid header name'); return name; };
const headerValue = value => { value = String(value).replace(/^[\t ]+|[\t ]+$/g, ''); if (/[\0\r\n]/.test(value) || [...value].some(c => c.charCodeAt(0) > 255)) throw new TypeError('Invalid header value'); return value; };
class Headers {
  #pairs = [];
  constructor(init = []) {
    if (init != null && typeof init[Symbol.iterator] === 'function') for (const entry of init) { const pair = [...entry]; if (pair.length !== 2) throw new TypeError('Expected header pair'); this.append(...pair); }
    else for (const key of Object.keys(init)) this.append(key, init[key]);
  }
  append(name, value) { this.#pairs.push([headerName(name), headerValue(value)]); }
  delete(name) { name = headerName(name); this.#pairs = this.#pairs.filter(p => p[0] !== name); }
  get(name) { name = headerName(name); const values = this.#pairs.filter(p => p[0] === name).map(p => p[1]); return values.length ? values.join(', ') : null; }
  getSetCookie() { return this.#pairs.filter(p => p[0] === 'set-cookie').map(p => p[1]); }
  has(name) { return this.get(name) !== null; }
  set(name, value) { name = headerName(name); value = headerValue(value); this.delete(name); this.#pairs.push([name, value]); }
  *entries() {
    const keys = [...new Set(this.#pairs.map(p => p[0]))].sort();
    for (const key of keys) { if (key === 'set-cookie') for (const value of this.getSetCookie()) yield [key,value]; else yield [key,this.get(key)]; }
  }
  *keys() { for (const [key] of this.entries()) yield key; } *values() { for (const [,value] of this.entries()) yield value; }
  [Symbol.iterator]() { return this.entries(); } forEach(fn, self) { for (const [key,value] of this) fn.call(self, value, key, this); }
}
function streamBytes(bytes) {
  let offset = 0;
  return new ReadableStream({ pull(c) { if (offset >= bytes.length) c.close(); else { const data = bytes.slice(offset, offset + 65536); offset += data.length; c.enqueue(data); } } }, { highWaterMark: 0 });
}
function bodyInit(body, headers) {
  if (body == null) return null;
  if (body instanceof ReadableStream) return body;
  let data;
  if (body instanceof Blob) { data = body._bytes(); if (body.type && !headers.has('content-type')) headers.set('content-type', body.type); }
  else if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) data = toBytes(body).slice();
  else if (body instanceof FormData) {
    const boundary = '----Abomin' + crypto.randomUUID();
    const encoder = new TextEncoder(), chunks = [];
    const escape = value => String(value).replace(/\r/g, '%0D').replace(/\n/g, '%0A').replace(/"/g, '%22');
    for (const [name, value] of body) {
      chunks.push(encoder.encode(`--${boundary}\r\nContent-Disposition: form-data; name="${escape(name)}"`));
      if (value instanceof File) chunks.push(encoder.encode(`; filename="${escape(value.name)}"\r\nContent-Type: ${value.type || 'application/octet-stream'}\r\n\r\n`), value._bytes(), encoder.encode('\r\n'));
      else chunks.push(encoder.encode(`\r\n\r\n${String(value).replace(/\r?\n|\r/g, '\r\n')}\r\n`));
    }
    chunks.push(encoder.encode(`--${boundary}--\r\n`)); data = new Blob(chunks)._bytes();
    if (!headers.has('content-type')) headers.set('content-type', 'multipart/form-data; boundary=' + boundary);
  } else {
    if (body instanceof URLSearchParams) { if (!headers.has('content-type')) headers.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8'); }
    else if (!headers.has('content-type')) headers.set('content-type', 'text/plain;charset=UTF-8');
    data = new TextEncoder().encode(String(body));
  }
  return streamBytes(data);
}
class Body {
  _stream = null; _used = false;
  get body() { return this._stream; }
  get bodyUsed() { return this._used; }
  async bytes() {
    if (this._used || this._stream?.locked) throw new TypeError('Body has already been consumed'); this._used = true;
    if (!this._stream) return new Uint8Array();
    const reader = this._stream.getReader(), chunks = []; let size = 0;
    try { while (true) { const { done, value } = await reader.read(); if (done) break; const data = toBytes(value); chunks.push(data.slice()); size += data.length; if (size > 256 * 1024 * 1024) { await reader.cancel(); throw new RangeError('Body exceeds 256 MiB; consume body as a stream'); } } }
    finally { reader.releaseLock(); }
    const result = new Uint8Array(size); let offset = 0; for (const chunk of chunks) { result.set(chunk, offset); offset += chunk.length; } return result;
  }
  async arrayBuffer() { return (await this.bytes()).buffer; }
  async text() { return new TextDecoder().decode(await this.bytes()); }
  async json() { return JSON.parse(await this.text()); }
  async blob() { return new Blob([await this.bytes()], { type: this.headers.get('content-type') ?? '' }); }
  async formData() {
    if (!this.headers.get('content-type')?.startsWith('application/x-www-form-urlencoded')) throw new TypeError('Only URL-encoded form bodies are supported');
    const form = new FormData(); for (const [key,value] of new URLSearchParams(await this.text())) form.append(key,value); return form;
  }
  _cloneBody() { if (this._used || this._stream?.locked) throw new TypeError('Body has already been consumed'); if (!this._stream) return null; const [left,right] = this._stream.tee(); this._stream = left; return right; }
}
class Request extends Body {
  constructor(input, init = {}) {
    super(); const from = input instanceof Request ? input : null;
    const url = new URL(from ? from.url : String(input));
    if (url.username || url.password) throw new TypeError('Request URL cannot contain credentials');
    url.hash = ''; this.url = url.href;
    const raw = String(init.method ?? from?.method ?? 'GET');
    if (!/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/.test(raw) || ['CONNECT','TRACE','TRACK'].includes(raw.toUpperCase())) throw new TypeError('Invalid HTTP method');
    this.method = ['DELETE','GET','HEAD','OPTIONS','POST','PUT'].includes(raw.toUpperCase()) ? raw.toUpperCase() : raw;
    this.headers = new Headers(init.headers ?? from?.headers);
    this.redirect = init.redirect ?? from?.redirect ?? 'follow';
    if (!['follow','error','manual'].includes(this.redirect)) throw new TypeError('Invalid redirect mode');
    this.signal = init.signal ?? from?.signal ?? new AbortController().signal;
    if (!(this.signal instanceof AbortSignal)) throw new TypeError('Expected AbortSignal');
    this.credentials = init.credentials ?? from?.credentials ?? 'same-origin'; this.mode = init.mode ?? from?.mode ?? 'cors'; this.cache = init.cache ?? from?.cache ?? 'default';
    this._stream = init.body !== undefined ? bodyInit(init.body, this.headers) : from ? from._cloneBody() : null;
    if ((this.method === 'GET' || this.method === 'HEAD') && this._stream) throw new TypeError('GET/HEAD request cannot have a body');
  }
  clone() { return new Request(this); }
}
class Response extends Body {
  constructor(body = null, init = {}) {
    super(); this.status = init.status ?? 200;
    if (!Number.isInteger(this.status) || this.status < 200 || this.status > 599) throw new RangeError('Invalid response status');
    this.statusText = String(init.statusText ?? ''); if (/[\r\n]/.test(this.statusText)) throw new TypeError('Invalid status text');
    this.headers = new Headers(init.headers); this.url = ''; this.redirected = false; this.type = 'default';
    if (body !== null && [204,205,304].includes(this.status)) throw new TypeError('Status cannot have a body');
    this._stream = bodyInit(body, this.headers);
  }
  get ok() { return this.status >= 200 && this.status <= 299; }
  clone() { const r = new Response(null, { status: this.status === 0 ? 200 : this.status, statusText: this.statusText, headers: this.headers }); r.status = this.status; r._stream = this._cloneBody(); r.url = this.url; r.redirected = this.redirected; r.type = this.type; return r; }
  static error() { const r = new Response(); r.status = 0; r.type = 'error'; return r; }
  static redirect(url, status = 302) { if (![301,302,303,307,308].includes(status)) throw new RangeError('Invalid redirect status'); return new Response(null, { status, headers: { location: new URL(url).href } }); }
  static json(value, init = {}) { const headers = new Headers(init.headers); if (!headers.has('content-type')) headers.set('content-type', 'application/json'); const data = JSON.stringify(value); if (data === undefined) throw new TypeError('Value is not JSON serializable'); return new Response(data, { ...init, headers }); }
}
const bodyFinalizers = new FinalizationRegistry(rid => native.cancelFetch(rid));
async function fetch(input, init) {
  const request = new Request(input, init); request.signal.throwIfAborted();
  let url = new URL(request.url), method = request.method, headers = new Headers(request.headers);
  let body = request.body ? await request.bytes() : new Uint8Array();
  if (body.length > 64 * 1024 * 1024) throw new RangeError('Upload exceeds 64 MiB');
  for (let redirects = 0; ; redirects++) {
    request.signal.throwIfAborted();
    if (url.protocol === 'data:') {
      const content = url.href.slice(5), comma = content.indexOf(','); if (comma < 0) throw new TypeError('Invalid data URL');
      const meta = content.slice(0,comma), raw = content.slice(comma+1); let data;
      if (/;base64$/i.test(meta)) data = Uint8Array.from(atob(decodeURIComponent(raw)), c => c.charCodeAt(0));
      else data = new TextEncoder().encode(decodeURIComponent(raw));
      const response = new Response(data, { headers: { 'content-type': meta.replace(/;base64$/i, '') || 'text/plain;charset=US-ASCII' } }); response.url = url.href; return response;
    }
    let rid, abort;
    const headPromise = new Promise((resolve, reject) => {
      const id = nextCall++; pending.set(id, { resolve, reject });
      try { rid = native.fetch(id, url.href, method, Array.from(headers), body); }
      catch (error) { pending.delete(id); reject(error); }
    });
    abort = () => { if (rid !== undefined) native.cancelFetch(rid); };
    request.signal.addEventListener('abort', abort, { once: true });
    let head;
    try { head = await headPromise; } catch (error) { request.signal.removeEventListener('abort', abort); request.signal.throwIfAborted(); throw new TypeError('fetch failed: ' + error.message); }
    const responseHeaders = new Headers(head.headers), location = responseHeaders.get('location');
    if ([301,302,303,307,308].includes(head.status) && location !== null && request.redirect !== 'manual') {
      native.cancelFetch(rid); request.signal.removeEventListener('abort', abort);
      if (request.redirect === 'error' || redirects >= 20) throw new TypeError('Redirect rejected');
      const next = new URL(location, url.href);
      if (next.origin !== url.origin) { headers.delete('authorization'); headers.delete('cookie'); headers.delete('proxy-authorization'); }
      if ((head.status === 303 && method !== 'HEAD') || ([301,302].includes(head.status) && method === 'POST')) { method = 'GET'; body = new Uint8Array(); for (const h of ['content-type','content-length','content-encoding']) headers.delete(h); }
      url = next; continue;
    }
    let ended = false, bodyController;
    const finish = () => { if (ended) return; ended = true; native.cancelFetch(rid); request.signal.removeEventListener('abort', abort); };
    request.signal.removeEventListener('abort', abort);
    abort = () => { const reason = request.signal.reason; finish(); bodyController?.error(reason); };
    request.signal.addEventListener('abort', abort, { once: true });
    const empty = method === 'HEAD' || [204,205,304].includes(head.status);
    let stream = empty ? null : new ReadableStream({
      start(c) { bodyController = c; },
      async pull(c) { try { request.signal.throwIfAborted(); const chunk = await asyncOp('bodyRead', rid); if (chunk === null) { finish(); c.close(); } else c.enqueue(new Uint8Array(chunk)); } catch (error) { finish(); c.error(request.signal.aborted ? request.signal.reason : error); } },
      cancel() { finish(); }
    }, { highWaterMark: 0 });
    if (empty) finish();
    else {
      bodyFinalizers.register(stream, rid);
      const encoding = responseHeaders.get('content-encoding');
      if (encoding && ['gzip','deflate','br'].includes(encoding)) stream = stream.pipeThrough(new DecompressionStream(encoding === 'br' ? 'brotli' : encoding));
    }
    const response = new Response(null, { status: head.status, headers: responseHeaders }); response._stream = stream; response.url = url.href; response.redirected = redirects > 0; response.type = 'basic';
    return response;
  }
}
expose({ Headers, Request, Response, fetch });
