// A project's tests in the Testing view.
//
// The items are what `turf test` runs: every `.tr` file under a project's
// tests/ directory, and every file of its src/ that declares `@test` functions.
// A file's `@test` functions are its children, each at its `fn`. Running any
// item runs the project's whole suite (`turf test` can name tests, `turf test
// <path>...`, but one run of the suite is what the items are marked from) and
// marks every item with what it printed: a function by its own line
// (`[tests] unit.tr: adds ... PASS`), a file by its line when it has one (a
// plain test, or a file whose tests did not build) and otherwise by its
// functions. A program that fails to compile when it should (a `fail_*` test)
// shows as passed, as `turf test` counts it.

import * as vscode from 'vscode';
import * as path from 'path';
import { spawn } from 'child_process';
import { findToolchain, offerInstall } from './toolchain';
import { projectRootFor } from './tasks';
import { findTestFunctions, parseFailureDetails, parseTestLine } from './testoutput';

export function registerTesting(context: vscode.ExtensionContext): void {
    const controller = vscode.tests.createTestController('turf', 'Turf');
    context.subscriptions.push(controller);

    // "<immediate dir>/<file>|<project root>" -> a file's item, and
    // "<immediate dir>/<file>: <function>|<project root>" -> a `@test`
    // function's. turf prints a test as `[<immediate dir>] <file>` (and
    // `: <function>`), so that is what an output line is matched by.
    const items = new Map<string, vscode.TestItem>();

    const enabled = () => vscode.workspace.getConfiguration('turf').get<boolean>('testing.enable', true);

    async function addFile(uri: vscode.Uri, root: string, inSrc: boolean): Promise<void> {
        let text = '';
        try {
            text = Buffer.from(await vscode.workspace.fs.readFile(uri)).toString('utf8');
        } catch {
            return;
        }
        const functions = findTestFunctions(text);
        // A src/ file is a test only for its `@test` functions.
        if (inSrc && functions.length === 0) return;
        const key = `${path.basename(path.dirname(uri.fsPath))}/${path.basename(uri.fsPath)}`;
        const item = controller.createTestItem(uri.toString(), path.relative(root, uri.fsPath), uri);
        for (const fn of functions) {
            const child = controller.createTestItem(`${uri.toString()}#${fn.name}`, fn.name, uri);
            child.range = new vscode.Range(fn.line, 0, fn.line, 0);
            item.children.add(child);
            items.set(`${key}: ${fn.name}|${root}`, child);
        }
        controller.items.add(item);
        items.set(`${key}|${root}`, item);
    }

    async function discover(): Promise<void> {
        controller.items.forEach(item => controller.items.delete(item.id));
        items.clear();
        if (!enabled()) return;
        const files = await vscode.workspace.findFiles('**/{tests,src}/**/*.tr', '**/{node_modules,build,.git}/**', 4000);
        for (const uri of files) {
            const root = projectRootFor(uri.fsPath);
            const top = root ? path.relative(root, uri.fsPath).split(path.sep)[0] : undefined;
            if (!root || (top !== 'tests' && top !== 'src')) continue;
            await addFile(uri, root, top === 'src');
        }
    }

    const watcher = vscode.workspace.createFileSystemWatcher('**/{tests,src}/**/*.tr');
    watcher.onDidCreate(() => void discover());
    watcher.onDidDelete(() => void discover());
    // A saved file may have gained or lost a `@test` function.
    watcher.onDidChange(() => void discover());
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
                const failed = new Set<string>();
                for (const line of output.split(/\r?\n/)) {
                    const test = parseTestLine(line);
                    if (!test) continue;
                    const name = `${test.label}/${test.file}${test.test ? `: ${test.test}` : ''}`;
                    const item = items.get(`${name}|${root}`);
                    if (!item) continue;
                    reported.add(item.id);
                    if (test.passed) { if (!failed.has(item.id)) run.passed(item); continue; }
                    failed.add(item.id);
                    // The detail block's header names the test as its key spells
                    // it: `tests/bad.tr`, `tests/unit.tr: adds`, `src/main.tr: halves`.
                    const tail = `${test.label === 'tests' ? '' : test.label + '/'}${test.file}${test.test ? `: ${test.test}` : ''}`;
                    const why = [...details.entries()].find(([p]) => p.endsWith(tail))?.[1];
                    run.failed(item, new vscode.TestMessage(why ? `${test.detail}\n${why}` : test.detail));
                }
                for (const item of mine) {
                    // A file of `@test` functions is what its functions are; one
                    // that also has a `main` has a line of its own, and fails
                    // when that line or any of its functions does.
                    const children: vscode.TestItem[] = [];
                    item.children.forEach(child => { children.push(child); });
                    const ran = children.filter(child => reported.has(child.id));
                    const bad = ran.filter(child => failed.has(child.id));
                    if (bad.length > 0 && !failed.has(item.id)) {
                        run.failed(item, new vscode.TestMessage(`${bad.length} of its tests failed: ${bad.map(c => c.label).join(', ')}`));
                        continue;
                    }
                    if (reported.has(item.id)) continue;
                    if (children.length > 0 && ran.length === children.length) run.passed(item);
                    else run.skipped(item); // the suite stopped early
                }
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
