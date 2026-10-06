// Run, Build, Check and Test as VS Code tasks: they show up in "Run Task…", can
// be bound to keys and put in tasks.json, and run in the terminal panel with
// the real output, which a command spawning a hidden process could not give.

import * as vscode from 'vscode';
import * as fs from 'fs';
import * as path from 'path';
import { commandLine, findProjectRoot } from './resolve';
import { findToolchain, offerInstall } from './toolchain';

export type TurfCommand = 'run' | 'build' | 'check' | 'test';

interface TurfTaskDefinition extends vscode.TaskDefinition {
    command: TurfCommand;
    args?: string[];
}

const COMMANDS: TurfCommand[] = ['run', 'build', 'check', 'test'];

function exists(p: string): boolean {
    try { return fs.statSync(p).isFile(); } catch { return false; }
}

/** The project root for a file, if it is in one. */
export function projectRootFor(file: string): string | undefined {
    return findProjectRoot(file, exists);
}

function makeTask(definition: TurfTaskDefinition, scope: vscode.WorkspaceFolder | vscode.TaskScope, cwd: string,
                  turf: string, args: string[]): vscode.Task {
    const task = new vscode.Task(
        definition, scope, `${definition.command}`, 'turf',
        new vscode.ShellExecution(turf, [...args, ...(definition.args ?? [])], { cwd }),
    );
    task.group = definition.command === 'build' ? vscode.TaskGroup.Build
               : definition.command === 'test' ? vscode.TaskGroup.Test : undefined;
    task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, clear: true, focus: false };
    return task;
}

/** The tasks of every project (turf.toml) in the open folders. */
export class TurfTaskProvider implements vscode.TaskProvider {
    async provideTasks(): Promise<vscode.Task[]> {
        const toolchain = await findToolchain();
        const tasks: vscode.Task[] = [];
        for (const folder of vscode.workspace.workspaceFolders ?? []) {
            if (!exists(path.join(folder.uri.fsPath, 'turf.toml'))) continue;
            for (const command of COMMANDS) {
                // Without a toolchain the task still exists (and says why when run).
                tasks.push(makeTask({ type: 'turf', command }, folder, folder.uri.fsPath, toolchain?.turf ?? 'turf', [command]));
            }
        }
        return tasks;
    }

    async resolveTask(task: vscode.Task): Promise<vscode.Task | undefined> {
        const definition = task.definition as TurfTaskDefinition;
        if (!COMMANDS.includes(definition.command)) return undefined;
        const folder = typeof task.scope === 'object' ? task.scope as vscode.WorkspaceFolder : vscode.workspace.workspaceFolders?.[0];
        if (!folder) return undefined;
        const toolchain = await findToolchain();
        return makeTask(definition, folder, folder.uri.fsPath, toolchain?.turf ?? 'turf', [definition.command]);
    }
}

/**
 * Run, build, check or test what is open: the project the active file belongs
 * to, or the file alone when it is in none. Everything that can go wrong is
 * answered with a message that says what to do.
 */
export async function runTurf(command: TurfCommand): Promise<void> {
    if (!vscode.workspace.isTrusted) {
        const choice = await vscode.window.showWarningMessage(
            `Turf will not ${command} code in a workspace you have not trusted: that would compile and run it.`,
            'Manage Workspace Trust');
        if (choice) await vscode.commands.executeCommand('workbench.trust.manage');
        return;
    }

    const editor = vscode.window.activeTextEditor;
    const file = editor && editor.document.languageId === 'turf' && editor.document.uri.scheme === 'file'
        ? editor.document.uri.fsPath : undefined;
    const folder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
    const root = file ? projectRootFor(file) : (folder && exists(path.join(folder, 'turf.toml')) ? folder : undefined);

    if (file && editor!.document.isDirty) await editor!.document.save();

    const args = commandLine(command, file, root !== undefined);
    if (!args) {
        vscode.window.showInformationMessage(
            command === 'test'
                ? 'Tests belong to a Turf project (a folder with turf.toml and a tests/ directory). Create one with "turf new".'
                : `Open a Turf (.tr) file to ${command}.`);
        return;
    }

    const toolchain = await findToolchain();
    if (!toolchain) {
        await offerInstall(`${command[0].toUpperCase()}${command.slice(1)}`);
        return;
    }

    const cwd = root ?? path.dirname(file!);
    const scope = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(cwd)) ?? vscode.TaskScope.Workspace;
    await vscode.tasks.executeTask(makeTask({ type: 'turf', command }, scope, cwd, toolchain.turf, args));
}
