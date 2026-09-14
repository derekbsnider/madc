// tools/vscode-madcide/test/protocol_probe.js — a REAL LSP client against
// `madcide <file> --lsp`, built on Microsoft's own protocol machinery
// (vscode-jsonrpc + vscode-languageserver-protocol: the same message reader,
// writer and types vscode-languageclient uses inside VS Code).
//
// WHY THIS EXISTS. The repo's own gates (tests/testmadcide_lsp,
// tests/testmadcide_lsp_stdio) pin what madcide believes the protocol is. This
// probe asks a real client, with a real VS Code-shaped initialize, whether it
// agrees — and it does not need VS Code, a display, or a network. It found
// three defects on its first run (V6c-3a): references highlighting the call's
// first ARGUMENT instead of the callee, documentSymbol selecting the whole
// declaration line instead of the symbol's name, and three conforming-client
// notifications landing on stderr as "unknown". Each was then pinned in
// testmadcide_lsp, which is the gate — this is the instrument.
//
// It is NOT part of `make -C src fulltest`: it needs `npm install`, and the
// battery must run with no network and no node.
//
//   cd tools/vscode-madcide
//   npm install
//   node test/protocol_probe.js            # uses ../../bin/madc
//   node test/protocol_probe.js /path/to/madc
//
// Every line it prints is an answer from the server. Read them against the
// protocol, not against the last run: a difference is a question, not a
// failure.

const cp = require('child_process');
const path = require('path');
const fs = require('fs');
const rpc = require('vscode-jsonrpc/node');

const REPO = path.resolve(__dirname, '../../..');
const MADC = process.argv[2] || path.join(REPO, 'bin/madc');
const DOC = path.join(REPO, 'tmp/vscode_madcide_probe.mad');
const TEXT = 'long twice(long a)\n{\n\treturn a + a;\n}\n\nint main()\n{\n\tlong n = twice(21);\n\treturn n;\n}\n';

fs.mkdirSync(path.dirname(DOC), { recursive: true });
fs.writeFileSync(DOC, TEXT);

const child = cp.spawn(MADC,
	['--no-config', 'tools/madcide/madcide.mad', DOC, '--lsp'],
	{ cwd: REPO, stdio: ['pipe', 'pipe', 'pipe'] });

let stderr = '';
let exited = null;
const t0 = Date.now();
child.stderr.on('data', d => { stderr += d.toString(); });
child.on('exit', (code, signal) => { exited = { code, signal, ms: Date.now() - t0 }; });

const conn = rpc.createMessageConnection(
	new rpc.StreamMessageReader(child.stdout),
	new rpc.StreamMessageWriter(child.stdin));

const diags = [];
conn.onNotification('textDocument/publishDiagnostics', p => diags.push(p));
conn.onUnhandledNotification(n => console.log('UNHANDLED NOTIFICATION: ' + n.method));
conn.onError(e => console.log('CONNECTION ERROR: ' + e));
conn.listen();

const uri = 'file://' + DOC;

// What vscode-languageclient 9.x sends at initialize. Every key here is one
// the real client sends; the shape is what matters, not the exhaustive list.
const initParams = {
	processId: process.pid,
	clientInfo: { name: 'Visual Studio Code', version: '1.95.0' },
	locale: 'en-us',
	rootPath: REPO,
	rootUri: 'file://' + REPO,
	capabilities: {
		workspace: {
			applyEdit: true,
			workspaceEdit: { documentChanges: true, resourceOperations: ['create', 'rename', 'delete'] },
			didChangeConfiguration: { dynamicRegistration: true },
			didChangeWatchedFiles: { dynamicRegistration: true, relativePatternSupport: true },
			symbol: { dynamicRegistration: true },
			executeCommand: { dynamicRegistration: true },
			configuration: true,
			workspaceFolders: true,
			semanticTokens: { refreshSupport: true },
			diagnostics: { refreshSupport: true }
		},
		textDocument: {
			publishDiagnostics: { relatedInformation: true, tagSupport: { valueSet: [1, 2] }, codeDescriptionSupport: true, dataSupport: true },
			synchronization: { dynamicRegistration: true, willSave: true, willSaveWaitUntil: true, didSave: true },
			completion: { dynamicRegistration: true, contextSupport: true },
			hover: { dynamicRegistration: true, contentFormat: ['markdown', 'plaintext'] },
			signatureHelp: { dynamicRegistration: true },
			definition: { dynamicRegistration: true, linkSupport: true },
			references: { dynamicRegistration: true },
			documentHighlight: { dynamicRegistration: true },
			documentSymbol: { dynamicRegistration: true, hierarchicalDocumentSymbolSupport: true, labelSupport: true },
			semanticTokens: {
				dynamicRegistration: true,
				tokenTypes: ['namespace', 'type', 'class', 'enum', 'interface', 'struct', 'typeParameter', 'parameter', 'variable', 'property', 'enumMember', 'event', 'function', 'method', 'macro', 'keyword', 'modifier', 'comment', 'string', 'number', 'regexp', 'operator', 'decorator'],
				tokenModifiers: ['declaration', 'definition', 'readonly', 'static', 'deprecated', 'abstract', 'async', 'modification', 'documentation', 'defaultLibrary'],
				formats: ['relative'],
				requests: { range: true, full: { delta: true } },
				serverCancelSupport: true,
				augmentsSyntaxTokens: true
			},
			rename: { dynamicRegistration: true, prepareSupport: true },
			foldingRange: { dynamicRegistration: true },
			inlayHint: { dynamicRegistration: true }
		},
		window: { workDoneProgress: true, showMessage: {}, showDocument: { support: true } },
		general: {
			staleRequestSupport: { cancel: true, retryOnContentModified: [] },
			regularExpressions: { engine: 'ECMAScript', version: 'ES2020' },
			markdown: { parser: 'marked', version: '1.1.0' },
			positionEncodings: ['utf-16']
		}
	},
	initializationOptions: {},
	trace: 'verbose',
	workspaceFolders: [{ uri: 'file://' + REPO, name: path.basename(REPO) }]
};

const show = (label, v) => console.log(label + ': ' + JSON.stringify(v));
const settle = ms => new Promise(r => setTimeout(r, ms));

async function main() {
	const init = await conn.sendRequest('initialize', initParams);
	show('initialize.capabilities', init.capabilities);
	show('initialize.serverInfo', init.serverInfo);

	conn.sendNotification('initialized', {});
	// The notifications a real client sends unasked. Nothing may come back,
	// and nothing may reach stderr.
	conn.sendNotification('$/setTrace', { value: 'verbose' });
	conn.sendNotification('workspace/didChangeConfiguration', { settings: {} });

	conn.sendNotification('textDocument/didOpen', {
		textDocument: { uri, languageId: 'madc', version: 1, text: TEXT }
	});
	await settle(1500);
	show('diagnostics.count', diags.length);
	if (diags.length) show('diagnostics.last', diags[diags.length - 1]);

	show('documentSymbol', await conn.sendRequest('textDocument/documentSymbol', { textDocument: { uri } }));

	// Line 7 is "\tlong n = twice(21);" — the caret inside `twice`.
	const at = { textDocument: { uri }, position: { line: 7, character: 11 } };
	show('hover', await conn.sendRequest('textDocument/hover', at));
	show('definition', await conn.sendRequest('textDocument/definition', at));
	show('references', await conn.sendRequest('textDocument/references', {
		textDocument: { uri }, position: { line: 0, character: 6 },
		context: { includeDeclaration: true }
	}));

	const toks = await conn.sendRequest('textDocument/semanticTokens/full', { textDocument: { uri } });
	const data = toks && toks.data ? toks.data : [];
	show('semanticTokens.count', data.length / 5);
	show('semanticTokens.head', data.slice(0, 20));

	// An incremental change, exactly as VS Code sends one.
	conn.sendNotification('textDocument/didChange', {
		textDocument: { uri, version: 2 },
		contentChanges: [{ range: { start: { line: 7, character: 1 }, end: { line: 7, character: 1 } }, rangeLength: 0, text: 'x' }]
	});
	await settle(1500);
	show('after-change.diagnostics', diags.length ? diags[diags.length - 1].diagnostics : null);

	conn.sendNotification('textDocument/willSave', { textDocument: { uri }, reason: 1 });
	conn.sendNotification('textDocument/didSave', { textDocument: { uri } });
	await settle(1200);
	conn.sendNotification('textDocument/didClose', { textDocument: { uri } });

	// A method the server does not implement: an error, never a hang.
	try {
		show('completion(unexpectedly answered)', await conn.sendRequest('textDocument/completion', at));
	} catch (e) {
		show('completion.error', { code: e.code, message: String(e.message).slice(0, 120) });
	}

	await conn.sendRequest('shutdown');
	conn.sendNotification('exit');
	for (let i = 0; i < 100 && !exited; i++)
		await settle(100);
	show('exit', exited);
	console.log('--- stderr (must be empty for a conforming session) ---');
	console.log(stderr.trim() || '(empty)');
}

main().then(() => process.exit(0)).catch(e => {
	console.log('PROBE FAILED: ' + e);
	console.log('--- stderr ---');
	console.log(stderr.trim() || '(empty)');
	child.kill();
	process.exit(1);
});
