"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = require("vscode");
const path = require("path");
const fs = require("fs");
const node_1 = require("vscode-languageclient/node");
let client;
function activate(context) {
    const config = vscode.workspace.getConfiguration('turf');
    // 1. Resolve binary path
    let serverPath = config.get('languageServer.path') || '';
    if (!serverPath) {
        // Fallback: search in common locations
        const workspaceFolders = vscode.workspace.workspaceFolders;
        if (workspaceFolders) {
            const workspaceBin = path.join(workspaceFolders[0].uri.fsPath, '.bin', 'turf-lsp');
            if (fs.existsSync(workspaceBin)) {
                serverPath = workspaceBin;
            }
        }
        if (!serverPath) {
            // Check relative build path (assuming extension is side-by-side with turf-lsp)
            const relativeBuildPath = path.join(__dirname, '..', '..', 'turf-lsp', 'build', 'turf-lsp');
            if (fs.existsSync(relativeBuildPath)) {
                serverPath = relativeBuildPath;
            }
        }
        if (!serverPath) {
            // Fallback to system PATH
            serverPath = 'turf-lsp';
        }
    }
    // 2. Configure server options
    const args = config.get('compiler.arguments') || [];
    const serverOptions = {
        run: { command: serverPath, args: args },
        debug: { command: serverPath, args: args }
    };
    // 3. Configure client options
    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'turf' }],
        initializationOptions: {
            clientInfo: {
                name: 'vscode-turf',
                version: '0.1.0'
            },
            enableSemanticTokens: config.get('enableSemanticTokens') !== false
        }
    };
    // 4. Create and start the client
    client = new node_1.LanguageClient('turf-lang', 'Turf Language Server', serverOptions, clientOptions);
    client.start();
}
function deactivate() {
    if (!client) {
        return undefined;
    }
    return client.stop();
}
//# sourceMappingURL=extension.js.map