// tools/vscode-madcide/test/attach_probe.js — THE THREE-CLIENT PICTURE, with
// real clients (client-server V6c-3c; plan
// docs/plans/2026-09-14-v6c3c-attach-relay-plan.md).
//
// An LSP editor (through `madcide --lsp --attach`) and a plain api client drive
// ONE already-running session, and the api client sees the editor's edit. That
// is the gatekeeper shape, and the thing a second session would silently break.
//
// Needs a session to attach to, and no VS Code, display or network:
//
//   bin/madc tools/madcide/madcide.mad tmp/attach_doc.mad --serve 127.0.0.1:45998 &
//   cd tools/vscode-madcide && npm install
//   node test/attach_probe.js
//
// Like protocol_probe.js this is the INSTRUMENT, not a gate: tests/testmadcide_attach
// is the gate, and it runs in fulltest with no network and no node.
const cp = require('child_process');
const rpc = require('vscode-jsonrpc/node');
const net = require('net');
const REPO = '/workspace/madc';
const ADDR = '127.0.0.1:45998';

const child = cp.spawn(REPO + '/bin/madc',
  ['--no-config','tools/madcide/madcide.mad','tmp/attach_doc.mad','--lsp','--attach',ADDR],
  { cwd: REPO, stdio: ['pipe','pipe','pipe'] });
let stderr = '';
child.stderr.on('data', d => stderr += d);
const conn = rpc.createMessageConnection(
  new rpc.StreamMessageReader(child.stdout), new rpc.StreamMessageWriter(child.stdin));
const notes = [];
conn.onNotification('$/madc/event', p => notes.push('event:' + p.kind));
conn.onNotification('textDocument/publishDiagnostics', () => notes.push('diagnostics'));
conn.listen();
const settle = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const init = await conn.sendRequest('initialize', { capabilities: {} });
  console.log('lsp editor attached; providers:',
    Object.keys(init.capabilities).filter(k => k.endsWith('Provider')).join(', '));
  conn.sendNotification('initialized', {});
  const syms = await conn.sendRequest('textDocument/documentSymbol',
    { textDocument: { uri: 'file://' + REPO + '/tmp/attach_doc.mad' } });
  console.log('symbols from the shared session:', syms.map(s => s.name).join(', '));
  await conn.sendRequest('workspace/executeCommand', { command: 'madcide.gotoline', arguments: ['8'] });
  await conn.sendRequest('workspace/executeCommand', { command: 'madcide.delline', arguments: [] });
  await settle(800);
  console.log('notifications through the relay:', notes.join(', '));

  // A SECOND client, speaking plain api on the same session.
  const api = net.connect(45998, '127.0.0.1');
  const reply = await new Promise(res => {
    api.on('connect', () => api.write(JSON.stringify({cmd:'top',args:'',seq:1}) + '\n'));
    let buf = '';
    api.on('data', d => { buf += d; if (buf.includes('\n')) res(JSON.parse(buf.split('\n')[0])); });
  });
  console.log('api client sees the LSP edit:', !reply.text.includes('return twice(21);'));
  api.end();

  await conn.sendRequest('shutdown');
  conn.sendNotification('exit');
  await settle(800);
  console.log('relay exit:', child.exitCode);
  console.log('relay stderr:', stderr.trim());
  process.exit(0);
})();
