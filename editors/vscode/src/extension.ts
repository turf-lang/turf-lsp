import * as path from 'path';
import { workspace, ExtensionContext } from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    Executable
} from 'vscode-languageclient/node';

let client: LanguageClient;

export function activate(context: ExtensionContext) {
    // Get the path to the turf-lsp executable from settings, default to just 'turf-lsp' in PATH
    const serverPath = workspace.getConfiguration('turf').get<string>('server.path') || 'turf-lsp';

    const run: Executable = {
        command: serverPath,
        options: {
            env: process.env
        }
    };

    const serverOptions: ServerOptions = {
        run,
        debug: run
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'turf' }],
        synchronize: {
            fileEvents: workspace.createFileSystemWatcher('**/*.turf')
        }
    };

    client = new LanguageClient(
        'turfLanguageServer',
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
