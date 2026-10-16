/** Typed native calls and JS-owned Promise state.
 * 类型化原生调用，Promise 状态由 JS 持有。 */
function formatError(error) { return error instanceof Error ? String(error) + (error.stack ? "\n" + error.stack : "") : String(error); }
const native = globalThis.__native;
delete globalThis.__native;
const config = native.config();
const pending = new Map();
const timers = new Map();
let nextCall = 1;
function asyncOp(name, ...args) {
  return new Promise((resolve, reject) => {
    const id = nextCall++;
    pending.set(id, { resolve, reject });
    try { native[name](id, ...args); }
    catch (error) { pending.delete(id); reject(error); }
  });
}
function expose(values) {
  for (const [key, value] of Object.entries(values))
    Object.defineProperty(globalThis, key, { value, configurable: true, writable: true });
}
class DOMException extends Error {
  constructor(message = '', name = 'Error') { super(String(message)); this.name = String(name); }
  get code() { return { AbortError: 20, DataCloneError: 25, InvalidCharacterError: 5, NotSupportedError: 9, QuotaExceededError: 22 }[this.name] ?? 0; }
}
class Event {
  static NONE = 0; static CAPTURING_PHASE = 1; static AT_TARGET = 2; static BUBBLING_PHASE = 3;
  constructor(type, init = {}) {
    this.type = String(type); this.bubbles = Boolean(init.bubbles); this.cancelable = Boolean(init.cancelable);
    this.composed = Boolean(init.composed); this.defaultPrevented = false; this.target = null;
    this.currentTarget = null; this.eventPhase = 0; this.timeStamp = native.now(); this.isTrusted = false;
  }
  preventDefault() { if (this.cancelable && !this._passive) this.defaultPrevented = true; }
  stopPropagation() { this.cancelBubble = true; }
  stopImmediatePropagation() { this._immediate = true; this.cancelBubble = true; }
  composedPath() { return this.target ? [this.target] : []; }
}
class CustomEvent extends Event { constructor(type, init = {}) { super(type, init); this.detail = init.detail ?? null; } }
class MessageEvent extends Event {
  constructor(type, init = {}) { super(type, init); this.data = init.data ?? null; this.origin = init.origin ?? ''; this.ports = init.ports ?? []; this.lastEventId = init.lastEventId ?? ''; this.source = init.source ?? null; }
}
class ErrorEvent extends Event {
  constructor(type, init = {}) { super(type, init); this.message = init.message ?? ''; this.filename = init.filename ?? ''; this.lineno = init.lineno ?? 0; this.colno = init.colno ?? 0; this.error = init.error ?? null; }
}
class PromiseRejectionEvent extends Event {
  constructor(type, init = {}) { super(type, init); this.promise = init.promise; this.reason = init.reason; }
}
class EventTarget {
  #listeners = new Map();
  addEventListener(type, callback, options = {}) {
    if (callback == null) return;
    type = String(type); if (typeof options === 'boolean') options = { capture: options };
    if (options.signal?.aborted) return;
    const list = this.#listeners.get(type) ?? [];
    if (list.some(item => item.callback === callback && item.capture === Boolean(options.capture))) return;
    const item = { callback, capture: Boolean(options.capture), once: Boolean(options.once), passive: Boolean(options.passive) };
    if (options.signal) {
      item.signal = options.signal;
      item.abort = () => this.removeEventListener(type, callback, options);
      item.signal.addEventListener('abort', item.abort, { once: true });
    }
    list.push(item); this.#listeners.set(type, list);
  }
  removeEventListener(type, callback, options = {}) {
    const capture = typeof options === 'boolean' ? options : Boolean(options.capture);
    const list = this.#listeners.get(String(type));
    if (!list) return;
    const index = list.findIndex(item => item.callback === callback && item.capture === capture);
    if (index >= 0) {
      const [item] = list.splice(index, 1);
      if (item.signal) item.signal.removeEventListener('abort', item.abort);
    }
  }
  dispatchEvent(event) {
    if (!(event instanceof Event)) throw new TypeError('Expected Event');
    if (event.eventPhase) throw new DOMException('Event is already being dispatched', 'InvalidStateError');
    event.target = this; event.currentTarget = this; event.eventPhase = Event.AT_TARGET; event._immediate = false;
    const list = this.#listeners.get(event.type) ?? [];
    for (const item of [...list]) {
      if (!list.includes(item)) continue;
      if (item.once) this.removeEventListener(event.type, item.callback, item.capture);
      event._passive = item.passive;
      try { typeof item.callback === 'function' ? item.callback.call(this, event) : item.callback.handleEvent(event); }
      catch (error) { native.report(formatError(error)); }
      event._passive = false;
      if (event._immediate) break;
    }
    const handler = this['on' + event.type];
    if (!event._immediate && typeof handler === 'function') {
      try { if (handler.call(this, event) === false) event.preventDefault(); }
      catch (error) { native.report(formatError(error)); }
    }
    event.currentTarget = null; event.eventPhase = 0;
    return !event.defaultPrevented;
  }
}
class AbortSignal extends EventTarget {
  #aborted = false; #reason;
  constructor(token) { super(); if (token !== AbortSignal) throw new TypeError('Illegal constructor'); }
  get aborted() { return this.#aborted; } get reason() { return this.#reason; }
  throwIfAborted() { if (this.#aborted) throw this.#reason; }
  _abort(reason) {
    if (this.#aborted) return;
    this.#aborted = true; this.#reason = reason === undefined ? new DOMException('The operation was aborted', 'AbortError') : reason;
    this.dispatchEvent(new Event('abort'));
  }
  static abort(reason) { const s = new AbortSignal(AbortSignal); s._abort(reason); return s; }
  static timeout(ms) {
    if (!Number.isSafeInteger(ms) || ms < 0) throw new RangeError('Invalid timeout');
    const s = new AbortSignal(AbortSignal);
    setTimeout(() => s._abort(new DOMException('The operation timed out', 'TimeoutError')), ms);
    return s;
  }
  static any(signals) {
    const s = new AbortSignal(AbortSignal), cleanups = [];
    const finish = reason => { s._abort(reason); for (const cleanup of cleanups) cleanup(); };
    for (const signal of signals) {
      if (!(signal instanceof AbortSignal)) throw new TypeError('Expected AbortSignal');
      if (signal.aborted) { finish(signal.reason); break; }
      const listener = () => finish(signal.reason);
      signal.addEventListener('abort', listener, { once: true });
      cleanups.push(() => signal.removeEventListener('abort', listener));
    }
    return s;
  }
}
class AbortController {
  #signal = new AbortSignal(AbortSignal);
  get signal() { return this.#signal; } abort(reason) { this.#signal._abort(reason); }
}
function setTimer(callback, delay = 0, repeat = false, args = []) {
  if (typeof callback !== 'function') throw new TypeError('Timer callback must be a function');
  const id = native.timer(Number(delay), repeat); timers.set(id, { callback, repeat, args }); return id;
}
function setTimeout(callback, delay = 0, ...args) { return setTimer(callback, delay, false, args); }
function setInterval(callback, delay = 0, ...args) { return setTimer(callback, delay, true, args); }
function clearTimeout(id) { id = Number(id); if (timers.delete(id)) native.cancelTimer(id); }
const clearInterval = clearTimeout;
function queueMicrotask(callback) {
  if (typeof callback !== 'function') throw new TypeError('Expected callback');
  Promise.resolve().then(callback).catch(error => native.report(formatError(error)));
}
const timeOrigin = Date.now() - native.now();
const performance = Object.freeze({ now: () => native.now(), timeOrigin, toJSON() { return { timeOrigin }; } });
function inspect(value, seen = new Set()) {
  if (typeof value === 'string') return value;
  if (value instanceof Error) return value.stack ?? String(value);
  if (typeof value === 'bigint') return value + 'n';
  if (value === undefined) return 'undefined';
  try { return JSON.stringify(value, (key, item) => {
    if (typeof item === 'bigint') return item + 'n';
    if (item && typeof item === 'object') { if (seen.has(item)) return '[Circular]'; seen.add(item); }
    return item;
  }); } catch { return String(value); }
}
const console = {};
for (const name of ['log', 'info', 'debug', 'warn', 'error', 'dir', 'trace'])
  console[name] = (...args) => native.print(args.map(v => inspect(v)).join(' '), name === 'warn' || name === 'error' || name === 'trace');
console.assert = (condition, ...args) => { if (!condition) console.error('Assertion failed:', ...args); };
const globalEvents = new EventTarget();
expose({ DOMException, Event, CustomEvent, MessageEvent, ErrorEvent, PromiseRejectionEvent, EventTarget, AbortController, AbortSignal,
  setTimeout, setInterval, clearTimeout, clearInterval, queueMicrotask, performance, console,
  addEventListener: globalEvents.addEventListener.bind(globalEvents), removeEventListener: globalEvents.removeEventListener.bind(globalEvents), dispatchEvent: globalEvents.dispatchEvent.bind(globalEvents), self: globalThis });
