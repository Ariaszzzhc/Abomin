/** Graph cloning never exposes resources or VM pointers to another isolate.
 * 图克隆不向其他隔离环境暴露资源或 VM 指针。 */
const ports = new Map();
const cloneError = message => new DOMException(message, 'DataCloneError');
function pack(value, transfer = []) {
  const list = Array.from(transfer), transferred = new Set();
  for (const item of list) {
    if (transferred.has(item)) throw cloneError('Duplicate transferable');
    if (!(item instanceof ArrayBuffer) && !(item instanceof MessagePort)) throw cloneError('Unsupported transferable');
    if (item instanceof ArrayBuffer) { try { new Uint8Array(item); } catch { throw cloneError('Detached ArrayBuffer'); } }
    else if (item._detached) throw cloneError('Detached MessagePort');
    transferred.add(item);
  }
  const seen = new Map(), nodes = [];
  function encode(value) {
    if (value === undefined) return ['undefined'];
    if (typeof value === 'bigint') return ['bigint', String(value)];
    if (typeof value === 'number' && !Number.isFinite(value)) return ['number', String(value)];
    if (Object.is(value, -0)) return ['number', '-0'];
    if (typeof value === 'function' || typeof value === 'symbol') throw cloneError('Value cannot be cloned');
    if (value === null || typeof value !== 'object') return value;
    if (seen.has(value)) return ['ref', seen.get(value)];
    const index = nodes.length; seen.set(value, index); nodes.push(null);
    let node;
    if (value instanceof MessagePort) {
      if (!transferred.has(value)) throw cloneError('MessagePort must be transferred');
      node = ['port', value._id, value._queueForClone()];
    } else if (value instanceof ArrayBuffer) {
      let view; try { view = new Uint8Array(value); } catch { throw cloneError('Detached ArrayBuffer'); }
      node = ['buffer', Array.from(view)];
    } else if (ArrayBuffer.isView(value)) node = ['view', value instanceof DataView ? 'DataView' : value.constructor.name, encode(value.buffer), value.byteOffset, value instanceof DataView ? value.byteLength : value.length];
    else if (value instanceof Date) node = ['date', encode(value.getTime())];
    else if (value instanceof RegExp) node = ['regexp', value.source, value.flags];
    else if (value instanceof Map) node = ['map', Array.from(value, ([k,v]) => [encode(k), encode(v)])];
    else if (value instanceof Set) node = ['set', Array.from(value, encode)];
    else if (value instanceof File) node = ['file', Array.from(value._bytes()), value.type, value.name, value.lastModified];
    else if (value instanceof Blob) node = ['blob', Array.from(value._bytes()), value.type];
    else if (value instanceof DOMException) node = ['exception', value.message, value.name];
    else if (value instanceof Error) node = ['error', value.name, value.message, value.stack, 'cause' in value ? encode(value.cause) : undefined];
    else if (value instanceof WeakMap || value instanceof WeakSet || value instanceof Promise || value instanceof EventTarget || (typeof SharedArrayBuffer !== 'undefined' && value instanceof SharedArrayBuffer)) throw cloneError('Unsupported object');
    else node = [Array.isArray(value) ? 'array' : 'object', Object.keys(value).map(k => [k, encode(value[k])]), Array.isArray(value) ? value.length : 0];
    nodes[index] = node; return ['ref', index];
  }
  const root = encode(value);
  const packet = JSON.stringify({ root, nodes });
  if (packet.length > 16 * 1024 * 1024) throw cloneError('Message exceeds 16 MiB');
  // Validate the complete graph before detaching. 完整验证对象图后才分离转移项。
  for (const item of list) {
    if (item instanceof ArrayBuffer) item.transfer();
    else item._detach();
  }
  return packet;
}
function unpack(packet) {
  const { root, nodes } = JSON.parse(packet), result = new Array(nodes.length), messagePorts = [];
  function decode(value) {
    if (!Array.isArray(value)) return value;
    if (value[0] === 'undefined') return undefined;
    if (value[0] === 'bigint') return BigInt(value[1]);
    if (value[0] === 'number') return value[1] === '-0' ? -0 : Number(value[1]);
    return build(value[1]);
  }
  function build(index) {
    if (index in result) return result[index];
    const node = nodes[index]; let value;
    switch (node[0]) {
      case 'buffer': value = new Uint8Array(node[1]).buffer; break;
      case 'date': value = new Date(decode(node[1])); break;
      case 'regexp': value = new RegExp(node[1], node[2]); break;
      case 'blob': value = new Blob([new Uint8Array(node[1])], { type: node[2] }); break;
      case 'file': value = new File([new Uint8Array(node[1])], node[3], { type: node[2], lastModified: node[4] }); break;
      case 'port': value = new MessagePort(MessagePort, node[1]); for (const packet of node[2]) value._receive(packet); messagePorts.push(value); break;
      case 'exception': value = new DOMException(node[1], node[2]); break;
      case 'error': {
        const Ctor = { Error, TypeError, RangeError, ReferenceError, SyntaxError, URIError, EvalError }[node[1]] ?? Error;
        value = new Ctor(node[2]); value.stack = node[3]; result[index] = value;
        if (node.length > 4 && node[4] !== undefined && node[4] !== null) value.cause = decode(node[4]);
        break;
      }
      case 'view': {
        const ctors = { Uint8Array, Uint8ClampedArray, Int8Array, Uint16Array, Int16Array, Uint32Array, Int32Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array, DataView };
        value = new ctors[node[1]](decode(node[2]), node[3], node[4]); break;
      }
      case 'map': value = new Map(); result[index] = value; for (const [k,v] of node[1]) value.set(decode(k), decode(v)); break;
      case 'set': value = new Set(); result[index] = value; for (const v of node[1]) value.add(decode(v)); break;
      case 'array': case 'object':
        value = node[0] === 'array' ? new Array(node[2]) : {}; result[index] = value;
        for (const [k,v] of node[1]) Object.defineProperty(value, k, { value: decode(v), enumerable: true, writable: true, configurable: true }); break;
      default: throw cloneError('Invalid clone packet');
    }
    result[index] = value; return value;
  }
  return { data: decode(root), ports: messagePorts };
}
function structuredClone(value, options = {}) {
  const transfer = options.transfer ?? [];
  return unpack(pack(value, transfer)).data;
}
class MessagePort extends EventTarget {
  _id; _detached = false; #started = false; #queue = []; #onmessage = null;
  constructor(token, id) { super(); if (token !== MessagePort) throw new TypeError('Illegal constructor'); this._id = id; ports.set(id, this); native.port('attach', id); }
  postMessage(value, options = []) {
    if (this._detached) return;
    const transfer = Array.isArray(options) ? options : options.transfer ?? [];
    if (transfer.includes(this)) throw cloneError('Cannot transfer the source port');
    native.port('post', this._id, pack(value, transfer));
  }
  start() { if (this.#started || this._detached) return; this.#started = true; native.retain(); this.#flush(); }
  #flush() { if (this.#started) for (const packet of this.#queue.splice(0)) this._receive(packet); }
  _receive(packet) { if (this._detached) return; if (!this.#started) { this.#queue.push(packet); return; } this.dispatchEvent(new MessageEvent('message', unpack(packet))); }
  _queueForClone() { return [...this.#queue]; }
  _detach() { if (this._detached) throw cloneError('Detached MessagePort'); native.port('disown', this._id); this._detached = true; ports.delete(this._id); if (this.#started) native.release(); }
  close() { if (this._detached) return; native.port('close', this._id); this._detached = true; ports.delete(this._id); if (this.#started) native.release(); }
  get onmessage() { return this.#onmessage; }
  set onmessage(value) { this.#onmessage = typeof value === 'function' ? value : null; if (this.#onmessage) this.start(); }
}
class MessageChannel {
  constructor() { const [a,b] = native.portCreate(); this.port1 = new MessagePort(MessagePort, a); this.port2 = new MessagePort(MessagePort, b); }
}
const workers = new Map();
class Worker extends EventTarget {
  #id; #closed = false;
  constructor(specifier, options = {}) {
    super(); if (options.type !== 'module') throw new TypeError('Only module Workers are supported; pass { type: "module" }');
    this.#id = native.worker(String(specifier)); workers.set(this.#id, this); this.name = String(options.name ?? '');
  }
  postMessage(value, options = []) { if (!this.#closed) native.postWorker(this.#id, pack(value, Array.isArray(options) ? options : options.transfer ?? [])); }
  terminate() { if (!this.#closed) { this.#closed = true; native.terminateWorker(this.#id); } }
  _receive(packet) { if (!this.#closed) this.dispatchEvent(new MessageEvent('message', unpack(packet))); }
  _error(message) { if (this.dispatchEvent(new ErrorEvent('error', { cancelable: true, message, error: new Error(message) }))) native.report(message); }
  _exit() { this.#closed = true; }
}
expose({ structuredClone, MessagePort, MessageChannel, Worker });
if (config.worker) {
  expose({ postMessage(value, options = []) { native.postParent(pack(value, Array.isArray(options) ? options : options.transfer ?? [])); }, close() { native.close(); } });
  for (const type of ['message', 'messageerror', 'error', 'unhandledrejection']) Object.defineProperty(globalThis, 'on' + type, { configurable: true, get: () => globalEvents['on' + type] ?? null, set: value => { globalEvents['on' + type] = value; } });
}
