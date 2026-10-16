native.setUnhandled((promise, reason) => {
  const event = new PromiseRejectionEvent('unhandledrejection', { cancelable: true, promise, reason });
  if (globalEvents.dispatchEvent(event)) native.report(formatError(reason));
});
native.setDispatcher((type, id, value) => {
  try {
    if (type === 'resolve' || type === 'reject') {
      const item = pending.get(id); if (!item) return;
      pending.delete(id); item[type](value);
    } else if (type === 'timer') {
      const timer = timers.get(id); if (!timer) return;
      if (!timer.repeat) timers.delete(id);
      timer.callback(...timer.args);
    } else if (type === 'message') {
      if (id === 0) globalEvents.dispatchEvent(new MessageEvent('message', unpack(value)));
      else workers.get(id)?._receive(value);
    } else if (type === 'workerError') workers.get(id)?._error(value);
    else if (type === 'workerExit') { workers.get(id)?._exit(); workers.delete(id); }
    else if (type === 'port') ports.get(id)?._receive(value);
  } catch (error) { native.report(formatError(error)); }
});
Object.defineProperty(globalThis, '__abominSettled', { value: false, writable: true });
Object.defineProperty(globalThis, '__abominStart', { value(promise) {
  Promise.resolve(promise).then(() => { globalThis.__abominSettled = true; }, error => {
    globalThis.__abominSettled = true; native.report(formatError(error));
  });
} });
