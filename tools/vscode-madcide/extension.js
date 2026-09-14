// tools/vscode-madcide/extension.js — the VS Code client for madcide's LSP
// face (client-server V6c-3a; plan docs/plans/2026-09-14-v6c3a-vscode-extension-plan.md).
//
// VS Code has no built-in way to point at a language server: the client lives
// in extension code. This is that client and nothing more — it resolves the
// madc binary, picks the document madcide opens with, spawns
// `madc <madcide.mad> <file> --lsp` on stdio and hands the pipe to
// vscode-languageclient. Every answer the editor shows comes from madcide's
// own compiler, over the protocol.
//
// There is deliberately NO TextMate grammar in this extension. madc's
// parse_spans classification reaches the editor as SEMANTIC TOKENS, from the
// same parse handle that produces the diagnostics and the outline; a grammar
// would be a second classification of the same text, free to disagree with the
// compiler about what a word is.

const fs = require('fs');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

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
function resolveMadc(root, out) {
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
async function resolveStartFile(root, out) {
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

async function activate(context) {
	const out = vscode.window.createOutputChannel('madcide');
	context.subscriptions.push(out);

	const root = workspaceRoot();
	if (!root) {
		out.appendLine('no workspace folder is open — madcide serves a project, so it needs one.');
		return;
	}
	const cfg = vscode.workspace.getConfiguration('madcide');
	const madc = resolveMadc(root, out);
	const madcide = absolute(root, cfg.get('madcidePath') || 'tools/madcide/madcide.mad');
	if (!fs.existsSync(madcide)) {
		out.appendLine('madcide.mad not found at ' + madcide +
			' — set madcide.madcidePath to where it lives.');
		return;
	}
	const file = await resolveStartFile(root, out);
	if (!file) {
		out.appendLine('no .mad document to open — madcide takes one on its command line.');
		return;
	}

	const args = ['--no-config', madcide, file, '--lsp'].concat(cfg.get('extraArgs') || []);
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
	out.appendLine('madcide language server started.');
}

function deactivate() {
	return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
