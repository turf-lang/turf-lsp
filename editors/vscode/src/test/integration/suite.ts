// Runs INSIDE a real VS Code (see runTest.ts), against the installed extension:
// each test asks VS Code for what a user would get -- completion, hover,
// diagnostics, formatting -- through the same provider commands the editor's own
// UI uses, so a feature passes only if the extension, the language client, the
// bundled server and the bundled standard library all work together.

import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

const EXTENSION_ID = 'turflang.turf';

function write(file: string, text: string): vscode.Uri {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, text);
    return vscode.Uri.file(file);
}

async function waitFor<T>(what: string, probe: () => T | undefined | Promise<T | undefined>, timeoutMs = 60000): Promise<T> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
        const value = await probe();
        if (value) return value;
        if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
        await new Promise(r => setTimeout(r, 200));
    }
}

async function open(uri: vscode.Uri): Promise<vscode.TextDocument> {
    const doc = await vscode.workspace.openTextDocument(uri);
    await vscode.window.showTextDocument(doc);
    return doc;
}

function at(doc: vscode.TextDocument, needle: string, offset = 0): vscode.Position {
    const index = doc.getText().indexOf(needle);
    assert.ok(index >= 0, `"${needle}" is not in ${path.basename(doc.uri.fsPath)}`);
    return doc.positionAt(index + offset);
}

function labelOf(item: vscode.CompletionItem): string {
    return typeof item.label === 'string' ? item.label : item.label.label;
}

type Test = { name: string; run: () => Promise<void> };
const tests: Test[] = [];
function test(name: string, run: () => Promise<void>): void { tests.push({ name, run }); }

export async function run(): Promise<void> {
    const project = process.env.TURF_IT_WORKSPACE!;
    const dependency = path.join(path.dirname(project), 'dep');
    assert.ok(project, 'TURF_IT_WORKSPACE is not set');

    // The fixture project: `turf new`'s shape, with a nested module and a dependency.
    write(path.join(project, 'turf.toml'), '[package]\nname = "proj"\nversion = "0.1.0"\nturf = "2.0"\n\n[dependencies]\ndep = { path = "../dep" }\n');
    write(path.join(dependency, 'turf.toml'), '[package]\nname = "dep"\nversion = "0.1.0"\nturf = "2.0"\n');
    write(path.join(dependency, 'src', 'dep.tr'), 'pub fn answer(): int {\n    return 21;\n}\n');
    write(path.join(project, 'src', 'lib', 'util.tr'), 'pub fn double_it(x: int): int {\n    return x * 2;\n}\n\nfn hidden_helper(): int {\n    return 0;\n}\n');
    write(path.join(project, 'tests', 'basic.tr'), 'fn main(): int {\n    return 0;\n}\n');
    const mainText = 'import io;\nimport util;\nimport dep;\n\nfn add(a: int, b: int): int {\n    return a + b;\n}\n\n' +
        'fn main(): int {\n    io::printline("hi");\n    return util::double_it(dep::answer()) + add(1, 2);\n}\n';
    const mainUri = write(path.join(project, 'src', 'main.tr'), mainText);

    const extension = vscode.extensions.getExtension(EXTENSION_ID);
    assert.ok(extension, `${EXTENSION_ID} is not installed`);
    await extension.activate();

    test('the extension is the packaged one, with its server and standard library bundled', async () => {
        const bin = path.join(extension.extensionPath, 'server', 'bin', process.platform === 'win32' ? 'turf-lsp.exe' : 'turf-lsp');
        assert.ok(fs.existsSync(bin), `no bundled server at ${bin}`);
        assert.ok(fs.existsSync(path.join(extension.extensionPath, 'server', 'lib', 'std', 'core', 'int.tr')), 'no bundled standard library');
    });

    test('its commands are registered', async () => {
        const commands = await vscode.commands.getCommands(true);
        for (const c of ['turf.run', 'turf.build', 'turf.check', 'turf.test', 'turf.restartServer', 'turf.showServerLog', 'turf.installToolchain'])
            assert.ok(commands.includes(c), `command ${c} is missing`);
    });

    test('a project file with imports of its own modules and a dependency has no errors', async () => {
        const doc = await open(mainUri);
        await waitFor('the first analysis', () => vscode.languages.getDiagnostics(doc.uri).length === 0 ? true : undefined, 90000);
        // Give the analysis time to arrive: an empty list before it ran is not a pass.
        const hover = await waitFor('hover to answer', async () => {
            const h = await vscode.commands.executeCommand<vscode.Hover[]>('vscode.executeHoverProvider', doc.uri, at(doc, 'add(1', 1));
            return h && h.length ? h : undefined;
        });
        assert.ok(hover.length > 0);
        assert.deepStrictEqual(vscode.languages.getDiagnostics(doc.uri).map(d => d.message), [], 'a valid project file has diagnostics');
    });

    test('an error is underlined across the whole identifier, with a code', async () => {
        const uri = write(path.join(project, 'scratch.tr'), 'fn main(): int {\n    return missing;\n}\n');
        const doc = await open(uri);
        const diags = await waitFor('a diagnostic', () => { const d = vscode.languages.getDiagnostics(doc.uri); return d.length ? d : undefined; });
        assert.ok(diags[0].message.includes('missing'), diags[0].message);
        assert.strictEqual(diags[0].range.end.character - diags[0].range.start.character, 'missing'.length);
        assert.ok(diags[0].code, 'the diagnostic has no error code');
    });

    test('completion: module members, project modules (only pub), imports, keywords', async () => {
        const doc = await open(mainUri);
        const at_ = async (needle: string, offset: number, trigger?: string) =>
            (await vscode.commands.executeCommand<vscode.CompletionList>('vscode.executeCompletionItemProvider', doc.uri, at(doc, needle, offset), trigger)).items.map(labelOf);
        const io = await at_('printline', 0, ':');
        assert.ok(io.includes('printline') && io.includes('print'), `io:: lacks printline: ${io.slice(0, 12)}`);
        const util = await at_('double_it', 0, ':');
        assert.ok(util.includes('double_it'), 'util:: lacks double_it');
        assert.ok(!util.includes('hidden_helper'), 'util:: offers a private function');
    });

    test('completion: import names include the project\'s modules and dependencies; a module can be auto-imported', async () => {
        const uri = write(path.join(project, 'src', 'imports.tr'), 'import \n\nfn main(): int {\n    js\n    return 0;\n}\n');
        const doc = await open(uri);
        await new Promise(r => setTimeout(r, 1500));
        const imp = (await vscode.commands.executeCommand<vscode.CompletionList>('vscode.executeCompletionItemProvider', doc.uri, new vscode.Position(0, 7))).items.map(labelOf);
        for (const want of ['io', 'json', 'util', 'dep']) assert.ok(imp.includes(want), `import completion lacks ${want}: ${imp.slice(0, 20)}`);
        const bare = (await vscode.commands.executeCommand<vscode.CompletionList>('vscode.executeCompletionItemProvider', doc.uri, at(doc, 'js\n', 2))).items;
        const json = bare.find(i => labelOf(i) === 'json');
        assert.ok(json && json.additionalTextEdits && json.additionalTextEdits.length === 1, 'json is not offered with an import edit');
    });

    test('go to definition reaches the project module and the BUNDLED standard library', async () => {
        const doc = await open(mainUri);
        // The client asks for LocationLinks, so a result is either shape.
        const target = (x: vscode.Location | vscode.LocationLink): vscode.Uri =>
            (x as vscode.LocationLink).targetUri ?? (x as vscode.Location).uri;
        const util = await vscode.commands.executeCommand<Array<vscode.Location | vscode.LocationLink>>('vscode.executeDefinitionProvider', doc.uri, at(doc, 'double_it', 2));
        assert.ok(util.length && target(util[0]).fsPath.endsWith(path.join('src', 'lib', 'util.tr')), `definition: ${JSON.stringify(util)}`);
        const std = await vscode.commands.executeCommand<Array<vscode.Location | vscode.LocationLink>>('vscode.executeDefinitionProvider', doc.uri, at(doc, 'printline', 3));
        assert.ok(std.length && target(std[0]).fsPath.endsWith('io.tr'), `stdlib definition: ${JSON.stringify(std)}`);
        // Not the TURF_HOME of whoever runs this test: the library the extension carries.
        assert.ok(target(std[0]).fsPath.startsWith(path.join(extension.extensionPath, 'server', 'lib', 'std')),
            `the standard library came from ${target(std[0]).fsPath}, not the bundled one`);
    });

    test('semantic tokens do not overlap (a dependency module is coloured as written)', async () => {
        const doc = await open(mainUri);
        const legend = await vscode.commands.executeCommand<vscode.SemanticTokensLegend>('vscode.provideDocumentSemanticTokensLegend', doc.uri);
        const tokens = await vscode.commands.executeCommand<vscode.SemanticTokens>('vscode.provideDocumentSemanticTokens', doc.uri);
        assert.ok(legend && tokens && tokens.data.length >= 5 * 10, 'too few semantic tokens');
        let line = 0, col = 0, last: { line: number; end: number } | undefined;
        for (let i = 0; i < tokens.data.length; i += 5) {
            line += tokens.data[i];
            col = tokens.data[i] ? tokens.data[i + 1] : col + tokens.data[i + 1];
            assert.ok(!last || last.line !== line || col >= last.end, `overlapping tokens at line ${line + 1}, column ${col + 1}`);
            last = { line, end: col + tokens.data[i + 2] };
        }
    });

    test('signature help', async () => {
        const doc = await open(mainUri);
        const help = await vscode.commands.executeCommand<vscode.SignatureHelp>('vscode.executeSignatureHelpProvider', doc.uri, at(doc, 'add(1, 2)', 7), '(');
        assert.ok(help && help.signatures.length, 'no signature help');
        assert.ok(help.signatures[0].label.startsWith('add(int a, int b)'), help.signatures[0].label);
        assert.strictEqual(help.activeParameter, 1);
    });

    test('formatting touches only the lines that differ', async () => {
        const uri = write(path.join(project, 'src', 'messy.tr'), 'fn a(): int {\n    return 1;\n}\n\nfn b(): int {\nreturn 2;\n}\n');
        const doc = await open(uri);
        await new Promise(r => setTimeout(r, 1000));
        const edits = await vscode.commands.executeCommand<vscode.TextEdit[]>('vscode.executeFormatDocumentProvider', doc.uri, { tabSize: 4, insertSpaces: true });
        assert.strictEqual(edits.length, 1, `expected one edit, got ${edits.length}`);
        assert.strictEqual(edits[0].range.start.line, 5);
    });

    test('folding, document links and outline', async () => {
        const doc = await open(mainUri);
        const folds = await vscode.commands.executeCommand<vscode.FoldingRange[]>('vscode.executeFoldingRangeProvider', doc.uri);
        assert.ok(folds.some(f => f.start === 4 && f.end === 5), `no fold for add(): ${JSON.stringify(folds)}`);
        const links = await vscode.commands.executeCommand<vscode.DocumentLink[]>('vscode.executeLinkProvider', doc.uri);
        assert.ok(links.some(l => l.target && l.target.fsPath.endsWith('util.tr')), `no link for import util: ${JSON.stringify(links)}`);
        const symbols = await vscode.commands.executeCommand<Array<vscode.SymbolInformation | vscode.DocumentSymbol>>('vscode.executeDocumentSymbolProvider', doc.uri);
        const names = JSON.stringify(symbols);
        assert.ok(names.includes('add') && names.includes('main'), `outline: ${names}`);
    });

    test('quick fix: add a missing import, placed with the others', async () => {
        const uri = write(path.join(project, 'src', 'noimport.tr'), 'import math;\n\nfn main(): int {\n    io::printline("hi");\n    return 0;\n}\n');
        const doc = await open(uri);
        const diags = await waitFor('E031', () => { const d = vscode.languages.getDiagnostics(doc.uri).filter(x => String(typeof x.code === 'object' ? x.code.value : x.code) === 'E031'); return d.length ? d : undefined; });
        const actions = await vscode.commands.executeCommand<vscode.CodeAction[]>('vscode.executeCodeActionProvider', doc.uri, diags[0].range);
        const fix = actions.find(a => a.title === 'Add `import io;`');
        assert.ok(fix && fix.edit, `no quick fix; got ${actions.map(a => a.title)}`);
        await vscode.workspace.applyEdit(fix.edit);
        assert.ok(doc.getText().startsWith('import math;\nimport io;\n'), doc.getText().slice(0, 40));
    });

    test('tasks: build, check, run and test exist for the project', async () => {
        const tasks = await vscode.tasks.fetchTasks({ type: 'turf' });
        const names = tasks.map(t => t.name).sort();
        assert.deepStrictEqual(names, ['build', 'check', 'run', 'test'], `tasks: ${names}`);
    });

    test('the code lens "Run" sits above main', async () => {
        const doc = await open(mainUri);
        const lenses = await vscode.commands.executeCommand<vscode.CodeLens[]>('vscode.executeCodeLensProvider', doc.uri);
        assert.ok(lenses.some(l => l.command && l.command.command === 'turf.run'), `lenses: ${JSON.stringify(lenses.map(l => l.command?.title))}`);
    });

    let failed = 0;
    for (const t of tests) {
        try {
            await t.run();
            console.log(`ok   ${t.name}`);
        } catch (e) {
            failed++;
            console.log(`FAIL ${t.name}: ${(e as Error).message}`);
        }
    }
    if (failed) throw new Error(`${failed} integration test(s) failed`);
    console.log('integration tests: all passed');
}
