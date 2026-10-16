function assert(condition: unknown, message = 'assertion failed'): asserts condition { if (!condition) throw new Error(message); }
const workerURL = new URL('./worker.ts', import.meta.url);
const worker = new Worker(workerURL, { type: 'module' });
const bytes = new Uint8Array([7,8,9]); const message: any = { bytes }; message.self = message;
const result = new Promise<any>((resolve,reject) => { worker.onmessage = e => resolve(e.data); worker.onerror = reject; });
worker.postMessage(message, [bytes.buffer]); assert(bytes.byteLength === 0);
const value = await result; assert(value.answer === 42 && value.cycle && value.bytes[1] === 8);
const channel = new MessageChannel(); const second = new Worker(workerURL, { type: 'module' });
const viaPort = new Promise<any>((resolve,reject) => { channel.port1.onmessage = e => { channel.port1.close(); resolve(e.data); }; second.onerror = reject; });
second.postMessage({ port: channel.port2 }, [channel.port2]);
channel.port1.postMessage('hello');
const reply = await viaPort; assert(reply.answer === 42 && reply.payload === 'hello');
const spin = new Worker(workerURL, { type: 'module' }); spin.postMessage({ spin: true });
await new Promise(resolve => setTimeout(resolve, 100)); spin.terminate();
console.log('workers:ok');

const startup = new Worker(new URL('./worker-startup.ts', import.meta.url), { type: 'module' });
const started = new Promise(resolve => { startup.onmessage = e => resolve(e.data); });
startup.postMessage('queued-before-ready'); assert(await started === 'queued-before-ready');
const failed = new Worker(new URL('./worker-error.ts', import.meta.url), { type: 'module' });
const error = await new Promise<string>(resolve => { failed.onerror = event => { event.preventDefault(); resolve(event.message); }; });
assert(error.includes('worker failed'));

const missing = new Worker(new URL('./not-present.ts', import.meta.url), { type: 'module' });
const missingError = await new Promise<string>(resolve => { missing.onerror = e => { e.preventDefault(); resolve(e.message); }; });
assert(missingError.includes('Module not found'));
