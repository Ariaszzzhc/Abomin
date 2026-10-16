import { answer } from './module.ts';
onmessage = event => {
  if (event.data.spin) { while (true) {} }
  if (event.data.port) {
    const port = event.data.port;
    port.onmessage = e => { port.postMessage({ answer, payload: e.data }); port.close(); close(); };
    return;
  }
  const bytes = event.data.bytes;
  postMessage({ answer, cycle: event.data.self === event.data, bytes }, [bytes.buffer]);
  close();
};
