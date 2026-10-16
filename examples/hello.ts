const message: string = 'Hello from Abomin';
console.log(message, Abomin.version);
await new Promise(resolve => setTimeout(resolve, 10));
console.log('Arguments:', Abomin.args);
