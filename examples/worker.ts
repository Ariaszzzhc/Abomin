if (typeof postMessage === 'function') {
  onmessage = event => { postMessage({ doubled: event.data * 2 }); close(); };
} else {
  const worker = new Worker(import.meta.url, { type: 'module' });
  worker.onmessage = event => console.log(event.data);
  worker.postMessage(21);
}
