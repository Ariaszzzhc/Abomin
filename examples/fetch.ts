const response = await fetch(Abomin.args[0] ?? 'https://example.com');
console.log(response.status, response.headers.get('content-type'));
for await (const chunk of response.body!) console.log('Received bytes:', chunk.byteLength);
