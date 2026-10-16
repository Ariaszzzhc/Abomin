const url = Abomin.args[0];
if (Abomin.args[1] === 'reject') {
  let rejected = false;
  try { await fetch(url); } catch (error) { rejected = error instanceof TypeError; }
  if (!rejected) throw new Error('Untrusted certificate was accepted');
  console.log('tls:rejected');
} else {
  const response = await fetch(url, { method: 'POST', body: 'HTTP/2 你好' });
  const value = await response.json();
  if (value.protocol !== 'h2' || value.method !== 'POST' || value.body !== 'HTTP/2 你好') throw new Error('HTTP/2 mismatch');
  console.log('tls:h2:ok');
}
