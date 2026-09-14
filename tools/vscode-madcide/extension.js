// tools/vscode-madcide/extension.js — the VS Code client for madcide
// (client-server V6c-3a + V6c-3b; plans
// docs/plans/2026-09-14-v6c3a-vscode-extension-plan.md and
// docs/plans/2026-09-14-v6c3b-executecommand-webview-plan.md).
//
// VS Code has no built-in way to point at a language server: the client lives
// in extension code. This is that client and nothing more — it resolves the
// madc binary, picks the document madcide opens with, spawns
// `madc <madcide.mad> <file> --lsp [--serve 127.0.0.1:0]` and hands the pipe to
// vscode-languageclient. Every answer the editor shows, and every command it
// runs, comes from madcide's own compiler and its own command registry.
//
// Two things this file deliberately does NOT contain:
//   - a TextMate grammar. madc's parse_spans classification reaches the editor
//     as SEMANTIC TOKENS, from the same parse handle that produces the
//     diagnostics and the outline; a grammar would be a second classification
//     of the same text, free to disagree with the compiler about what a word is.
//   - a list of madcide's commands. The server advertises them
//     (executeCommandProvider, built from the registry's own cmd_table), so the
//     palette is built from the capability at run time. A list here would be a
//     copy that goes stale the moment a command is added.

const fs = require('fs');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;
let out;
let status;
let serveUrl;			// the session's other face, from $/madc/serve
let windowPanel;

// The workspace folder this session serves: the active editor's, else the
// first. madcide is one session over one project — the folder is its root.
function workspaceRoot() {
	const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
	if (doc && doc.uri.scheme === 'file') {
		const f = vscode.workspace.getWorkspaceFolder(doc.uri);
		if (f) return f.uri.fsPath;
	}
	const folders = vscode.workspace.workspaceFolders;
	return folders && folders.length ? folders[0].uri.fsPath : undefined;
}

function absolute(root, p) {
	if (!p) return undefined;
	return path.isAbsolute(p) ? p : (root ? path.join(root, p) : undefined);
}

// The madc binary: the setting, else `bin/madc` under the workspace folder
// (the in-tree build, which is what a madc developer has), else `madc` on
// PATH. Saying which one was chosen belongs in the output channel, not in a
// popup — a wrong guess shows up as a spawn failure with the path in it.
function resolveMadc(root) {
	const configured = vscode.workspace.getConfiguration('madcide').get('madcPath');
	if (configured) {
		out.appendLine('madc: ' + configured + ' (madcide.madcPath)');
		return configured;
	}
	const inTree = absolute(root, 'bin/madc');
	if (inTree && fs.existsSync(inTree)) {
		out.appendLine('madc: ' + inTree + ' (the workspace build)');
		return inTree;
	}
	out.appendLine('madc: madc (on PATH — set madcide.madcPath to choose another)');
	return 'madc';
}

// The document madcide opens with. madcide is a SESSION over a document, so it
// takes one on the command line; every other file the editor opens joins the
// same session through didOpen. The active .mad editor is the honest choice —
// the extension activates onLanguage:madc, so there almost always is one.
async function resolveStartFile(root) {
	const configured = vscode.workspace.getConfiguration('madcide').get('serverFile');
	if (configured) {
		const p = absolute(root, configured);
		out.appendLine('document: ' + p + ' (madcide.serverFile)');
		return p;
	}
	const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
	if (doc && doc.languageId === 'madc' && doc.uri.scheme === 'file') {
		out.appendLine('document: ' + doc.uri.fsPath + ' (the active editor)');
		return doc.uri.fsPath;
	}
	const found = await vscode.workspace.findFiles('**/*.mad', '**/node_modules/**', 1);
	if (found.length) {
		out.appendLine('document: ' + found[0].fsPath + ' (the first .mad in the workspace)');
		return found[0].fsPath;
	}
	return undefined;
}

// The commands the SERVER says it accepts, stripped of the wire prefix for
// display. The capability is the list; there is no second one here.
function serverCommands() {
	const caps = client && client.initializeResult && client.initializeResult.capabilities;
	const provider = caps && caps.executeCommandProvider;
	return (provider && provider.commands) || [];
}

// madcide's own notifications (V6c-3b). A client that did not know them would
// ignore them by the protocol's own rule for the `$/` space; this one uses them.
function wireMadcNotifications() {
	client.onNotification('$/madc/serve', p => {
		serveUrl = p && p.url;
		out.appendLine('window: ' + serveUrl);
	});
	client.onNotification('$/madc/message', p => {
		const text = (p && p.message) || '';
		if (!text) return;
		out.appendLine('madcide: ' + text);
		status.text = '$(tools) ' + text;
		status.show();
	});
	client.onNotification('$/madc/event', p => {
		// The session's change log, the same records every other client sees.
		// Tracing territory, not a popup.
		out.appendLine('event: ' + JSON.stringify(p));
	});
}

async function runCommandPalette() {
	if (!client) {
		vscode.window.showWarningMessage('madcide: the language server is not running.');
		return;
	}
	const ids = serverCommands();
	if (!ids.length) {
		vscode.window.showWarningMessage('madcide: this server advertises no commands.');
		return;
	}
	const picked = await vscode.window.showQuickPick(
		ids.map(id => ({ label: id.replace(/^madcide\./, ''), description: id, id: id })),
		{ placeHolder: 'madcide command', matchOnDescription: true });
	if (!picked) return;
	const arg = await vscode.window.showInputBox({
		prompt: 'Argument for ' + picked.label + ' (leave empty for none)',
		placeHolder: 'optional'
	});
	if (arg === undefined) return;		// cancelled, as opposed to empty
	try {
		const result = await client.sendRequest('workspace/executeCommand', {
			command: picked.id,
			arguments: arg ? [arg] : []
		});
		if (result && result.ok === false) {
			vscode.window.showErrorMessage('madcide: ' + (result.error || 'refused'));
			return;
		}
		const errors = result && result.errors;
		out.appendLine(picked.id + ': ok' + (errors ? ' (' + errors + ' problem(s))' : ''));
	} catch (e) {
		vscode.window.showErrorMessage('madcide: ' + e.message);
	}
}

// The real madcide UI, in a webview, against the SAME session the editor is
// using — that is the point of the server's second face. asExternalUri is what
// makes the loopback port reachable when the extension host is remote
// (Remote-SSH, containers): VS Code forwards it. The port is never exposed.
async function openWindow() {
	if (!serveUrl) {
		vscode.window.showWarningMessage(
			'madcide: this server has no window face. Set madcide.window and restart the server.');
		return;
	}
	const external = await vscode.env.asExternalUri(vscode.Uri.parse(serveUrl));
	if (windowPanel) {
		windowPanel.reveal();
		return;
	}
	windowPanel = vscode.window.createWebviewPanel(
		'madcideWindow', 'madcide', vscode.ViewColumn.Beside,
		{ enableScripts: true, retainContextWhenHidden: true });
	windowPanel.onDidDispose(() => { windowPanel = undefined; });
	const src = external.toString();
	windowPanel.webview.html = '<!DOCTYPE html><html><head><meta charset="utf-8">' +
		'<style>html,body,iframe{margin:0;padding:0;border:0;width:100%;height:100vh;display:block}</style>' +
		'</head><body><iframe src="' + src + '" sandbox="allow-scripts allow-same-origin"></iframe></body></html>';
	out.appendLine('window opened: ' + src);
}

async function startClient(context) {
	const root = workspaceRoot();
	if (!root) {
		out.appendLine('no workspace folder is open — madcide serves a project, so it needs one.');
		return;
	}
	const cfg = vscode.workspace.getConfiguration('madcide');
	const madc = resolveMadc(root);
	const madcide = absolute(root, cfg.get('madcidePath') || 'tools/madcide/madcide.mad');
	if (!fs.existsSync(madcide)) {
		out.appendLine('madcide.mad not found at ' + madcide +
			' — set madcide.madcidePath to where it lives.');
		return;
	}
	const file = await resolveStartFile(root);
	if (!file) {
		out.appendLine('no .mad document to open — madcide takes one on its command line.');
		return;
	}

	let args = ['--no-config', madcide, file, '--lsp'];
	if (cfg.get('window'))
		args = args.concat(['--serve', '127.0.0.1:0']);	// loopback only: no auth exists
	args = args.concat(cfg.get('extraArgs') || []);
	out.appendLine('spawning: ' + madc + ' ' + args.join(' '));
	out.appendLine('cwd: ' + root);

	const serverOptions = {
		command: madc,
		args: args,
		transport: TransportKind.stdio,
		options: { cwd: root }
	};
	const clientOptions = {
		documentSelector: [{ scheme: 'file', language: 'madc' }],
		outputChannel: out,
		// The server's own stderr is diagnostic, not a failure: leave the
		// channel where the user can read it, and do not steal focus for it.
		revealOutputChannelOn: 4
	};

	client = new LanguageClient('madcide', 'madcide', serverOptions, clientOptions);
	await client.start();
	wireMadcNotifications();
	out.appendLine('madcide language server started.');
}

async function stopClient() {
	serveUrl = undefined;
	if (windowPanel) {
		windowPanel.dispose();
		windowPanel = undefined;
	}
	if (client) {
		await client.stop();
		client = undefined;
	}
}

async function activate(context) {
	out = vscode.window.createOutputChannel('madcide');
	status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
	context.subscriptions.push(out, status);
	context.subscriptions.push(
		vscode.commands.registerCommand('madcide.runCommand', runCommandPalette),
		vscode.commands.registerCommand('madcide.openWindow', openWindow),
		vscode.commands.registerCommand('madcide.restart', async () => {
			await stopClient();
			await startClient(context);
		}));
	await startClient(context);
}

function deactivate() {
	return stopClient();
}

module.exports = { activate, deactivate };
