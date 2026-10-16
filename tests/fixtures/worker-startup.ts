await new Promise(resolve => setTimeout(resolve, 20));
onmessage = e => { postMessage(e.data); close(); };
