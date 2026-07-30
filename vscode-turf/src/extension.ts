import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions
} from 'vscode-languageclient/node';

let client: LanguageClient;

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

    // Dev-mode fallback: this extension lives at turf-lsp/vscode-turf, so
    // from __dirname (.../vscode-turf/out) two '..' reaches turf-lsp/
    // itself, and the binary is turf-lsp/build/turf-lsp from there. Useful
    // when F5-launching the extension against a workspace that isn't one of
    // the layouts above (e.g. a folder with no turf-lsp/turf-lang in it at
    // all was opened, just to test some other .tr file).
    const devRelativePath = path.join(__dirname, '..', '..', 'build', binaryName);
    if (fs.existsSync(devRelativePath)) {
        return devRelativePath;
    }

    return undefined;
}

export function activate(context: vscode.ExtensionContext) {
    const config = vscode.workspace.getConfiguration('turf');

    // 1. Resolve binary path
    let serverPath = config.get<string>('languageServer.path') || '';

    if (!serverPath) {
        serverPath = findServerBinary() || '';

        if (!serverPath) {
            // Nothing found anywhere -- fall back to bare 'turf-lsp' on
            // PATH (works if the user installed it system-wide), but warn
            // explicitly instead of letting this fail silently as an opaque
            // "couldn't create connection to server" with no indication of
            // what was actually searched.
            serverPath = 'turf-lsp';
            vscode.window.showWarningMessage(
                "Turf: couldn't locate the turf-lsp binary in the open workspace or next to this extension. " +
                "Falling back to 'turf-lsp' on PATH -- set \"turf.languageServer.path\" in settings if that's not where it is."
            );
        }
    }

    // 2. Configure server options
    const args = config.get<string[]>('compiler.arguments') || [];
    
    const serverOptions: ServerOptions = {
        run: { command: serverPath, args: args },
        debug: { command: serverPath, args: args }
    };

    // 3. Configure client options
    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'turf' }],
        initializationOptions: {
            clientInfo: {
                name: 'vscode-turf',
                version: '0.1.0'
            },
            enableSemanticTokens: config.get<boolean>('enableSemanticTokens') !== false
        }
    };

    // 4. Create and start the client
    client = new LanguageClient(
        'turf-lang',
        'Turf Language Server',
        serverOptions,
        clientOptions
    );

    client.start();
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}
