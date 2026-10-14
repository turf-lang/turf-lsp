// A project's tests in the Testing view.
//
// Every `.tr` file under a project's tests/ directory is a test (`turf test`'s
// own rule); each shows up as an item, and running any of them runs the project's
// whole suite (`turf test` can name tests, `turf test <path>...`, but one run
// of the suite is what the items are marked from) and marks every item with
// what it printed. A program that fails to compile when it should (a `fail_*` test)
// shows as passed, as `turf test` counts it.

import * as vscode from 'vscode';
import * as path from 'path';
import { spawn } from 'child_process';
import { findToolchain, offerInstall } from './toolchain';
import { projectRootFor } from './tasks';
import { parseFailureDetails, parseTestLine } from './testoutput';

export function registerTesting(context: vscode.ExtensionContext): void {
    const controller = vscode.tests.createTestController('turf', 'Turf');
    context.subscriptions.push(controller);

    // "<immediate dir>/<file>|<project root>" -> item. turf prints a test as
    // `[<immediate dir>] <file>`, so that is what an output line is matched by.
    const items = new Map<string, vscode.TestItem>();

    const enabled = () => vscode.workspace.getConfiguration('turf').get<boolean>('testing.enable', true);

    async function discover(): Promise<void> {
        controller.items.forEach(item => controller.items.delete(item.id));
        items.clear();
        if (!enabled()) return;
        const files = await vscode.workspace.findFiles('**/tests/**/*.tr', '**/{node_modules,build,.git}/**', 2000);
        for (const uri of files) {
            const root = projectRootFor(uri.fsPath);
            if (!root || path.relative(root, uri.fsPath).split(path.sep)[0] !== 'tests') continue;
            const item = controller.createTestItem(uri.toString(), path.relative(root, uri.fsPath), uri);
            controller.items.add(item);
            items.set(`${path.basename(path.dirname(uri.fsPath))}/${path.basename(uri.fsPath)}|${root}`, item);
        }
    }

    const watcher = vscode.workspace.createFileSystemWatcher('**/tests/**/*.tr');
    watcher.onDidCreate(() => void discover());
    watcher.onDidDelete(() => void discover());
    context.subscriptions.push(watcher, vscode.workspace.onDidChangeConfiguration(e => {
        if (e.affectsConfiguration('turf.testing.enable')) void discover();
    }));
    controller.refreshHandler = () => discover();
    void discover();

    controller.createRunProfile('Run (the whole project)', vscode.TestRunProfileKind.Run, async (request, token) => {
        const run = controller.createTestRun(request);
        try {
            if (!vscode.workspace.isTrusted) {
                vscode.window.showWarningMessage('Turf will not run tests in a workspace you have not trusted.');
                return;
            }
            const toolchain = await findToolchain();
            if (!toolchain) { await offerInstall('Running tests'); return; }

            // Every project that has a requested (or, with no selection, any) test.
            const roots = new Set<string>();
            const requested = request.include ?? [...items.values()];
            for (const item of requested) {
                const root = item.uri ? projectRootFor(item.uri.fsPath) : undefined;
                if (root) roots.add(root);
            }

            for (const root of roots) {
                const mine = [...items.entries()].filter(([key]) => key.endsWith(`|${root}`)).map(([, item]) => item);
                mine.forEach(item => run.started(item));

                const output = await runInProject(toolchain.turf, root, token, text => run.appendOutput(text.replace(/\r?\n/g, '\r\n')));
                const details = parseFailureDetails(output);
                const reported = new Set<string>();
                for (const line of output.split(/\r?\n/)) {
                    const test = parseTestLine(line);
                    const item = test ? items.get(`${test.label}/${test.file}|${root}`) : undefined;
                    if (!test || !item) continue;
                    reported.add(item.id);
                    if (test.passed) { run.passed(item); continue; }
                    const tail = `${test.label === 'tests' ? '' : test.label + '/'}${test.file}`;
                    const why = [...details.entries()].find(([p]) => p.endsWith(tail))?.[1];
                    run.failed(item, new vscode.TestMessage(why ? `${test.detail}\n${why}` : test.detail));
                }
                // A test that printed no result did not run (the suite stopped early).
                for (const item of mine) if (!reported.has(item.id)) run.skipped(item);
            }
        } finally {
            run.end();
        }
    }, true);
}

function runInProject(turf: string, cwd: string, token: vscode.CancellationToken, onOutput: (text: string) => void): Promise<string> {
    return new Promise(resolve => {
        const child = spawn(turf, ['test'], { cwd, windowsHide: true });
        let all = '';
        const take = (chunk: Buffer) => { const text = chunk.toString('utf8'); all += text; onOutput(text); };
        child.stdout.on('data', take);
        child.stderr.on('data', take);
        const cancel = token.onCancellationRequested(() => child.kill());
        child.on('close', () => { cancel.dispose(); resolve(all); });
        child.on('error', e => { cancel.dispose(); resolve(`${all}\n${e.message}`); });
    });
}
