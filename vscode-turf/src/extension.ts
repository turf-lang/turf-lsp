import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions
} from 'vscode-languageclient/node';

let client: LanguageClient;

export function activate(context: vscode.ExtensionContext) {
    const config = vscode.workspace.getConfiguration('turf');
    
    // 1. Resolve binary path
    let serverPath = config.get<string>('languageServer.path') || '';

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
