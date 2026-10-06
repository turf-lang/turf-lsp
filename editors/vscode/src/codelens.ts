// "▶ Run" above `fn main`, and "▶ Run Tests" at the top of a project's test
// files. Purely textual (a regular expression over the document), so the links
// appear the moment the file opens, before the language server has analysed it.

import * as vscode from 'vscode';
import * as path from 'path';
import { projectRootFor } from './tasks';

const MAIN = /^fn\s+main\s*\(/;

export class TurfCodeLensProvider implements vscode.CodeLensProvider {
    private readonly changed = new vscode.EventEmitter<void>();
    readonly onDidChangeCodeLenses = this.changed.event;

    refresh(): void { this.changed.fire(); }

    provideCodeLenses(document: vscode.TextDocument): vscode.CodeLens[] {
        if (!vscode.workspace.getConfiguration('turf').get<boolean>('codeLens.enable', true)) return [];
        if (document.uri.scheme !== 'file') return [];

        const lenses: vscode.CodeLens[] = [];
        for (let line = 0; line < document.lineCount; line++) {
            if (MAIN.test(document.lineAt(line).text)) {
                const range = new vscode.Range(line, 0, line, 0);
                lenses.push(new vscode.CodeLens(range, { title: '$(play) Run', command: 'turf.run', tooltip: 'Run this program' }));
                break;
            }
        }

        // A test file of a project: the whole suite runs (`turf test`).
        const root = projectRootFor(document.uri.fsPath);
        if (root) {
            const relative = path.relative(root, document.uri.fsPath).split(path.sep);
            if (relative[0] === 'tests') {
                lenses.push(new vscode.CodeLens(new vscode.Range(0, 0, 0, 0),
                    { title: '$(beaker) Run Tests', command: 'turf.test', tooltip: "Run the project's tests" }));
            }
        }
        return lenses;
    }
}
