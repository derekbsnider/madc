// tools/vscode-madcide/extension.js — the VS Code client for madc's language
// server (client-server V6c-3a; split arc 1D: the server is `madc --lsp`).
//
// VS Code has no built-in way to point at a language server: the client lives
// in extension code. This is that client and nothing more — it resolves the
// madc binary, picks the document the server opens with, spawns
// `madc --lsp <file>` and hands the pipe to vscode-languageclient. Every
// answer the editor shows, and every command it runs, comes from madc's own
// compiler and its own command registry.
//
// Two things this file deliberately does NOT contain:
//   - a TextMate grammar. madc's parse classification reaches the editor as
//     SEMANTIC TOKENS, from the same parse that produces the diagnostics and
//     the outline; a grammar would be a second classification of the same
//     text, free to disagree with the compiler about what a word is.
//   - a list of madc's commands. The server advertises them
//     (executeCommandProvider: madc's `%` commands as `madc.NAME`), so the
//     palette is built from the capability at run time. A list here would be
//     a copy that goes stale the moment a command is added.

const fs = require('fs');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;
let out;

// The workspace folder the server serves: the active editor's, else the
// first — the folder is its root (its working directory).
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

// The document the server opens with (`madc --lsp FILE`: its project); every
// other file the editor opens joins through didOpen. The active .mad editor is
// the honest choice — the extension activates onLanguage:madc, so there
// almost always is one.
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
		ids.map(id => ({ label: id.replace(/^madc\./, ''), description: id, id: id })),
		{ placeHolder: 'madc command', matchOnDescription: true });
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

async function startClient(context) {
	const root = workspaceRoot();
	if (!root) {
		out.appendLine('no workspace folder is open — the server runs in one.');
		return;
	}
	const cfg = vscode.workspace.getConfiguration('madcide');
	const madc = resolveMadc(root);
	const file = await resolveStartFile(root);
	let args = ['--lsp'];
	if (file)
		args.push(file);
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

	client = new LanguageClient('madcide', 'madc', serverOptions, clientOptions);
	await client.start();
	out.appendLine('madc language server started.');
}

async function stopClient() {
	if (client) {
		await client.stop();
		client = undefined;
	}
}

async function activate(context) {
	out = vscode.window.createOutputChannel('madcide');
	context.subscriptions.push(out);
	context.subscriptions.push(
		vscode.commands.registerCommand('madcide.runCommand', runCommandPalette),
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
