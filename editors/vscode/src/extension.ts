import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let statusBarItem: vscode.StatusBarItem;

// Locates the turf-lsp binary without assuming WHERE on disk the turf-lang/
// turf-lsp checkout lives (Desktop, home dir, anywhere) -- every candidate
// below is either an absolute user setting or a path built relative to
// something dynamic at activation time (the opened workspace folder, or
// this extension's own installed location), never a path baked in relative
// to any one developer's machine.
function findServerBinary(): string | undefined {
    const binaryName = process.platform === 'win32' ? 'turf-lsp.exe' : 'turf-lsp';

    const workspaceFolders = vscode.workspace.workspaceFolders;
    if (workspaceFolders) {
        for (const folder of workspaceFolders) {
            const root = folder.uri.fsPath;
            const candidates = [
                // A project that ships/vendors its own binary explicitly.
                path.join(root, '.bin', binaryName),
                // Workspace root IS the turf-lsp checkout itself.
                path.join(root, 'build', binaryName),
                // Workspace root is the parent folder containing both
                // turf-lang/ and turf-lsp/ as subfolders (e.g. this repo's
                // own "turf-code" layout).
                path.join(root, 'turf-lsp', 'build', binaryName),
                // Workspace root is turf-lang itself, with turf-lsp checked
                // out as a sibling folder next to it.
                path.join(root, '..', 'turf-lsp', 'build', binaryName),
            ];
            for (const candidate of candidates) {
                if (fs.existsSync(candidate)) {
                    return candidate;
                }
            }
        }
    }

    // Dev-mode fallback: this extension lives at turf-lsp/editors/vscode, so
    // from __dirname (.../editors/vscode/out) three '..' reaches turf-lsp/
    // itself, and the binary is turf-lsp/build/turf-lsp from there. Useful
    // when F5-launching the extension against a workspace that isn't one of
    // the layouts above.
    const devRelativePath = path.join(__dirname, '..', '..', '..', 'build', binaryName);
    if (fs.existsSync(devRelativePath)) {
        return devRelativePath;
    }

    return undefined;
}

function resolveServerPath(config: vscode.WorkspaceConfiguration): string {
    const configured = config.get<string>('server.path');
    if (configured) {
        return configured;
    }

    const found = findServerBinary();
    if (found) {
        return found;
    }

    // Nothing found anywhere -- fall back to bare 'turf-lsp' on PATH (works
    // if the user installed it system-wide), but warn explicitly instead of
    // letting this fail silently as an opaque "couldn't create connection to
    // server" with no indication of what was actually searched.
    vscode.window.showWarningMessage(
        "Turf: couldn't locate the turf-lsp binary in the open workspace or next to this extension. " +
        "Falling back to 'turf-lsp' on PATH -- set \"turf.server.path\" in settings if that's not where it is."
    );
    return 'turf-lsp';
}

function buildClient(): LanguageClient {
    const config = vscode.workspace.getConfiguration('turf');
    const serverPath = resolveServerPath(config);

    const env = { ...process.env };
    const turfHome = config.get<string>('home');
    if (turfHome) {
        env.TURF_HOME = turfHome;
    }

    const serverOptions: ServerOptions = {
        run: { command: serverPath, transport: TransportKind.stdio, options: { env } },
        debug: { command: serverPath, transport: TransportKind.stdio, options: { env } }
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'turf' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.tr')
        },
        initializationOptions: {
            diagnosticsEnable: config.get<boolean>('diagnostics.enable') !== false,
            completionSnippets: config.get<boolean>('completion.snippets') !== false
        }
    };

    return new LanguageClient(
        'turfLanguageServer',
        'Turf Language Server',
        serverOptions,
        clientOptions
    );
}

async function startClient(): Promise<void> {
    client = buildClient();
    statusBarItem.text = '$(sync~spin) Turf';
    statusBarItem.tooltip = 'Turf Language Server starting...';
    try {
        await client.start();
        statusBarItem.text = '$(check) Turf';
        statusBarItem.tooltip = 'Turf Language Server running -- click to restart';
    } catch (err) {
        statusBarItem.text = '$(error) Turf';
        statusBarItem.tooltip = `Turf Language Server failed to start: ${err}`;
    }
}

async function restartClient(): Promise<void> {
    if (client) {
        await client.stop();
        client = undefined;
    }
    await startClient();
}

export function activate(context: vscode.ExtensionContext) {
    statusBarItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    statusBarItem.command = 'turf.restartServer';
    statusBarItem.text = '$(sync~spin) Turf';
    statusBarItem.show();
    context.subscriptions.push(statusBarItem);

    context.subscriptions.push(
        vscode.commands.registerCommand('turf.restartServer', () => restartClient())
    );

    context.subscriptions.push(
        vscode.workspace.onWillSaveTextDocument(event => {
            if (event.document.languageId !== 'turf') {
                return;
            }
            if (!vscode.workspace.getConfiguration('turf').get<boolean>('format.onSave')) {
                return;
            }
            event.waitUntil(
                vscode.commands.executeCommand<vscode.TextEdit[]>(
                    'vscode.executeFormatDocumentProvider',
                    event.document.uri
                ).then(edits => edits ?? [])
            );
        })
    );

    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration(event => {
            if (event.affectsConfiguration('turf.server.path') || event.affectsConfiguration('turf.home')) {
                restartClient();
            }
        })
    );

    startClient();
}

export function deactivate(): Thenable<void> | undefined {
    statusBarItem?.dispose();
    if (!client) {
        return undefined;
    }
    return client.stop();
}
