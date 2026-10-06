import * as vscode from 'vscode';
import * as fs from 'fs';
import { execFile } from 'child_process';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    State,
    Trace,
    TransportKind,
} from 'vscode-languageclient/node';
import { chooseServer, compareVersions, describeSource, parseServerVersion, ServerChoice, ServerVersion } from './resolve';
import { findToolchain, forgetToolchain, INSTALL_URL, realProbe } from './toolchain';
import { runTurf, TurfTaskProvider } from './tasks';
import { TurfCodeLensProvider } from './codelens';
import { registerTesting } from './testing';

let client: LanguageClient | undefined;
let status: vscode.StatusBarItem;
let log: vscode.OutputChannel;
let started: { choice: ServerChoice; version: ServerVersion } | undefined;

const SETTINGS_REQUIRING_RESTART = [
    'turf.server.path', 'turf.home', 'turf.diagnostics.enable', 'turf.completion.snippets',
];

function setting<T>(key: string, fallback: T): T {
    return vscode.workspace.getConfiguration('turf').get<T>(key, fallback);
}

/** Runs `turf-lsp --version`: proof that the binary starts at all (a missing shared library, a wrong architecture) before anything depends on it. */
function probeServer(choice: ServerChoice): Promise<{ version?: ServerVersion; error?: string }> {
    return new Promise(resolve => {
        execFile(choice.command, ['--version'], { env: choice.env, timeout: 15000, windowsHide: true }, (error, stdout, stderr) => {
            if (error) {
                const detail = String(stderr || '').trim() || error.message;
                return resolve({ error: detail });
            }
            const version = parseServerVersion(String(stdout));
            resolve(version ? { version } : { error: `unexpected output from --version: ${String(stdout).slice(0, 200)}` });
        });
    });
}

function setStatus(text: string, tooltip: string, error = false): void {
    status.text = text;
    status.tooltip = new vscode.MarkdownString(tooltip);
    status.backgroundColor = error ? new vscode.ThemeColor('statusBarItem.errorBackground') : undefined;
}

async function startServer(context: vscode.ExtensionContext): Promise<void> {
    setStatus('$(sync~spin) Turf', 'Starting the Turf language server…');

    const settings = { path: setting('server.path', ''), home: setting('home', '') };
    const choice = chooseServer(context.extensionPath, settings, realProbe());

    if (!choice) {
        setStatus('$(error) Turf', 'The Turf language server was not found.', true);
        const action = await vscode.window.showErrorMessage(
            'Turf: the language server was not found. This extension should include it; ' +
            'reinstall the extension for your platform, or set "turf.server.path".',
            'Open Settings', 'Show Details');
        if (action === 'Open Settings') await vscode.commands.executeCommand('workbench.action.openSettings', 'turf.server.path');
        if (action === 'Show Details') {
            log.appendLine(`The extension is installed at ${context.extensionPath}; its server directory has no turf-lsp, and none is on PATH.`);
            log.show();
        }
        return;
    }

    if (choice.source !== 'path' && process.platform !== 'win32') {
        try { fs.chmodSync(choice.command, 0o755); } catch { /* read-only install: it is executable already, or the probe below says so */ }
    }

    const probe = await probeServer(choice);
    if (!probe.version) {
        log.appendLine(`The language server (${choice.command}) did not start: ${probe.error}`);
        setStatus('$(error) Turf', `The language server did not start.\n\n${probe.error}`, true);
        const action = await vscode.window.showErrorMessage(
            `Turf: the language server could not be started (${(probe.error ?? '').split('\n')[0]}).`,
            'Show Details', 'Open Settings');
        if (action === 'Show Details') log.show();
        if (action === 'Open Settings') await vscode.commands.executeCommand('workbench.action.openSettings', 'turf.server.path');
        return;
    }
    const version = probe.version;
    log.appendLine(`turf-lsp ${version.version} ${describeSource(choice.source)}: ${choice.command}`);
    if (version.stdlib) log.appendLine(`standard library: ${version.stdlib}`);
    for (const w of version.warnings) log.appendLine(`warning: ${w}`);

    // A server the user pointed at (or found on PATH) may be older than the
    // features this extension uses; the bundled one never is.
    const minimum: string | undefined = (context.extension.packageJSON as { turf?: { minServerVersion?: string } }).turf?.minServerVersion;
    if (choice.source !== 'bundled' && minimum && compareVersions(version.version, minimum) < 0) {
        vscode.window.showWarningMessage(
            `Turf: the language server ${version.version} is older than the ${minimum} this extension expects; ` +
            'some features may be missing. Remove "turf.server.path" to use the bundled server.');
    }

    const serverOptions: ServerOptions = {
        run: { command: choice.command, transport: TransportKind.stdio, options: { env: choice.env } },
        debug: { command: choice.command, transport: TransportKind.stdio, options: { env: choice.env } },
    };
    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'turf' }],
        outputChannel: log,
        synchronize: {
            // A project's manifest or lock, or a .tr file changed outside the
            // editor (a git checkout): the server re-analyses what is open.
            fileEvents: vscode.workspace.createFileSystemWatcher('**/{*.tr,turf.toml,turf.lock}'),
        },
        initializationOptions: {
            diagnosticsEnable: setting('diagnostics.enable', true),
            completionSnippets: setting('completion.snippets', true),
        },
    };

    client = new LanguageClient('turfLanguageServer', 'Turf Language Server', serverOptions, clientOptions);
    client.onDidChangeState(event => {
        const where = `${describeSource(choice.source)}, turf-lsp ${version.version}`;
        if (event.newState === State.Running) setStatus('$(check) Turf', `Turf language server is running (${where}).\n\nClick for options.`);
        else if (event.newState === State.Starting) setStatus('$(sync~spin) Turf', 'Starting the Turf language server…');
        else setStatus('$(error) Turf', 'The Turf language server stopped. Click to restart it.', true);
    });
    try {
        await client.start();
        await applyTrace();
        started = { choice, version };
    } catch (err) {
        log.appendLine(`Failed to start the language client: ${err}`);
        setStatus('$(error) Turf', `The language server failed to start: ${err}`, true);
    }
}

async function applyTrace(): Promise<void> {
    if (!client) return;
    const level = setting<string>('trace.server', 'off');
    await client.setTrace(level === 'verbose' ? Trace.Verbose : level === 'messages' ? Trace.Messages : Trace.Off);
}

async function stopServer(): Promise<void> {
    const old = client;
    client = undefined;
    started = undefined;
    if (old) { try { await old.stop(); } catch { /* already gone */ } }
}

async function restartServer(context: vscode.ExtensionContext): Promise<void> {
    await stopServer();
    await startServer(context);
}

async function showStatus(context: vscode.ExtensionContext): Promise<void> {
    const toolchain = await findToolchain();
    const items: (vscode.QuickPickItem & { run?: () => Thenable<unknown> | void })[] = [
        {
            label: started ? `$(check) Language server ${started.version.version}` : '$(error) Language server is not running',
            description: started ? describeSource(started.choice.source) : undefined,
            detail: started?.version.stdlib ? `Standard library: ${started.version.stdlib}` : undefined,
        },
        {
            label: toolchain ? '$(check) Toolchain found' : '$(info) Toolchain not installed (needed to run programs)',
            description: toolchain?.version,
            detail: toolchain ? toolchain.turf : 'Editing works without it; Run, Build and Test need the Turf compiler.',
            run: toolchain ? undefined : () => vscode.commands.executeCommand('turf.installToolchain'),
        },
        { label: '$(debug-restart) Restart language server', run: () => restartServer(context) },
        { label: '$(output) Show language server log', run: () => log.show() },
        { label: '$(gear) Open Turf settings', run: () => vscode.commands.executeCommand('workbench.action.openSettings', '@ext:turflang.turf') },
    ];
    const pick = await vscode.window.showQuickPick(items, { title: 'Turf' });
    await pick?.run?.();
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    log = vscode.window.createOutputChannel('Turf Language Server');
    status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    status.name = 'Turf';
    status.command = 'turf.showStatus';
    status.show();
    context.subscriptions.push(status, log);

    const lens = new TurfCodeLensProvider();
    context.subscriptions.push(
        vscode.commands.registerCommand('turf.restartServer', () => restartServer(context)),
        vscode.commands.registerCommand('turf.showServerLog', () => log.show()),
        vscode.commands.registerCommand('turf.showStatus', () => showStatus(context)),
        vscode.commands.registerCommand('turf.run', () => runTurf('run')),
        vscode.commands.registerCommand('turf.build', () => runTurf('build')),
        vscode.commands.registerCommand('turf.check', () => runTurf('check')),
        vscode.commands.registerCommand('turf.test', () => runTurf('test')),
        vscode.commands.registerCommand('turf.installToolchain', async () => {
            await vscode.env.openExternal(vscode.Uri.parse(INSTALL_URL));
            forgetToolchain();
        }),
        vscode.tasks.registerTaskProvider('turf', new TurfTaskProvider()),
        vscode.languages.registerCodeLensProvider({ language: 'turf', scheme: 'file' }, lens),
        vscode.workspace.onDidChangeConfiguration(event => {
            if (SETTINGS_REQUIRING_RESTART.some(key => event.affectsConfiguration(key))) void restartServer(context);
            if (event.affectsConfiguration('turf.toolchain.path')) forgetToolchain();
            if (event.affectsConfiguration('turf.trace.server')) void applyTrace();
            if (event.affectsConfiguration('turf.codeLens.enable')) lens.refresh();
        }),
        // `turf.format.onSave`: format with the language server before saving.
        vscode.workspace.onWillSaveTextDocument(event => {
            if (event.document.languageId !== 'turf' || !setting('format.onSave', false)) return;
            event.waitUntil(
                Promise.resolve(vscode.commands.executeCommand<vscode.TextEdit[]>('vscode.executeFormatDocumentProvider', event.document.uri))
                    .then(edits => edits ?? []));
        }),
    );

    registerTesting(context);
    await startServer(context);
}

export async function deactivate(): Promise<void> {
    await stopServer();
}
