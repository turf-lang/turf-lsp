// The Turf toolchain: the `turf` command-line tool and the compiler behind it.
//
// Editing needs none of it -- the language server and the standard library come
// with the extension. Running, building and testing a program does, because that
// is the compiler. So this finds a `turf` that really works (`turf version` exits
// 0 only when it can find its compiler) and, when there is none, says so
// plainly with a way to get one, instead of failing with a shell error.

import * as vscode from 'vscode';
import * as fs from 'fs';
import * as os from 'os';
import { execFile } from 'child_process';
import { Probe, toolchainCandidates } from './resolve';

export interface Toolchain {
    /** The `turf` executable. */
    turf: string;
    /** The first line of `turf version`, e.g. `turf 1.0.0 (needs turfc interface 1.1+)`. */
    version: string;
}

export function realProbe(): Probe {
    return {
        platform: process.platform,
        env: process.env,
        home: os.homedir(),
        exists: (p: string) => {
            try { return fs.statSync(p).isFile(); } catch { return false; }
        },
    };
}

let cache: { setting: string; result: Toolchain | undefined } | undefined;

/** Forget what was found (the setting changed, or the user just installed it). */
export function forgetToolchain(): void {
    cache = undefined;
}

function run(command: string, args: string[], timeoutMs: number): Promise<{ ok: boolean; stdout: string }> {
    return new Promise(resolve => {
        execFile(command, args, { timeout: timeoutMs, windowsHide: true }, (error, stdout) => {
            resolve({ ok: !error, stdout: String(stdout ?? '') });
        });
    });
}

/**
 * A `turf` that works, or undefined. Looked for in `turf.toolchain.path`, then on
 * PATH and in the default install locations. Remembered until the setting
 * changes; ask again with {@link forgetToolchain} after an install.
 */
export async function findToolchain(): Promise<Toolchain | undefined> {
    const setting = vscode.workspace.getConfiguration('turf').get<string>('toolchain.path', '');
    if (cache && cache.setting === setting && cache.result) return cache.result;

    for (const candidate of toolchainCandidates(setting, realProbe())) {
        const { ok, stdout } = await run(candidate, ['version'], 15000);
        if (ok) {
            const result = { turf: candidate, version: stdout.split(/\r?\n/)[0] ?? 'turf' };
            cache = { setting, result };
            return result;
        }
    }
    // Not remembered: the user may install it at any moment.
    return undefined;
}

/** Where to read how to install the toolchain. */
export const INSTALL_URL = 'https://github.com/turf-lang/turf-lang#installation';

/** Offers the way to get the toolchain; never silently does nothing. */
export async function offerInstall(what: string): Promise<void> {
    const choice = await vscode.window.showInformationMessage(
        `${what} needs the Turf compiler, which is not installed. ` +
        `Editing, completion and diagnostics work without it.`,
        'How to install', 'Set the path…');
    if (choice === 'How to install') {
        await vscode.commands.executeCommand('turf.installToolchain');
    } else if (choice === 'Set the path…') {
        await vscode.commands.executeCommand('workbench.action.openSettings', 'turf.toolchain.path');
    }
}
