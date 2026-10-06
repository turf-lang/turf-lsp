// Finding things on the user's machine: the language server, the Turf
// toolchain, the project a file belongs to. Pure functions of what they are
// given (no `vscode`, no real file system access of their own), so they are
// tested with plain Node on any platform (src/test/unit.ts) -- including the
// Windows layouts, which only exist as data here.

import * as path from 'path';

/** What discovery needs to know about the machine. */
export interface Probe {
    platform: NodeJS.Platform;
    env: NodeJS.ProcessEnv;
    home: string;
    exists(file: string): boolean;
}

export type ServerSource = 'setting' | 'bundled' | 'path';

export interface ServerChoice {
    /** The executable to start. */
    command: string;
    source: ServerSource;
    /** The environment to start it with. */
    env: NodeJS.ProcessEnv;
    /** Every place that was looked at, for the "not found" message. */
    tried: string[];
}

export interface ServerSettings {
    /** `turf.server.path` */
    path: string;
    /** `turf.home` */
    home: string;
}

function exe(platform: NodeJS.Platform, name: string): string {
    return platform === 'win32' ? `${name}.exe` : name;
}

/** `path.join` in the style of the machine being described, not the host's. */
function joiner(platform: NodeJS.Platform): (...parts: string[]) => string {
    return platform === 'win32' ? path.win32.join : path.posix.join;
}

/** The directories on PATH, in order. */
export function pathEntries(probe: Probe): string[] {
    const raw = probe.env.PATH ?? probe.env.Path ?? '';
    const sep = probe.platform === 'win32' ? ';' : ':';
    return raw.split(sep).filter(p => p.length > 0);
}

/** `name` on PATH, or undefined. */
export function which(name: string, probe: Probe): string | undefined {
    const join = joiner(probe.platform);
    for (const dir of pathEntries(probe)) {
        const candidate = join(dir, exe(probe.platform, name));
        if (probe.exists(candidate)) return candidate;
    }
    return undefined;
}

/**
 * Which `turf-lsp` to run, in this order:
 *   1. `turf.server.path`, when set. An explicit choice is used as given and
 *      never silently replaced: if it does not exist the caller says so.
 *   2. The server that ships inside the extension (`server/bin`), started with
 *      TURF_HOME pointing at its own standard library so that a TURF_HOME in
 *      the user's environment -- some other, perhaps older, Turf -- cannot
 *      hide the library the server was built against.
 *   3. `turf-lsp` on PATH, then the default per-user install.
 * `turf.home`, when set, wins over the bundled TURF_HOME: someone working on
 * the standard library itself.
 */
export function chooseServer(extensionPath: string, settings: ServerSettings, probe: Probe): ServerChoice | undefined {
    const join = joiner(probe.platform);
    const tried: string[] = [];
    const env: NodeJS.ProcessEnv = { ...probe.env };
    if (settings.home) env.TURF_HOME = settings.home;

    if (settings.path) {
        return { command: settings.path, source: 'setting', env, tried: [settings.path] };
    }

    const bundledRoot = join(extensionPath, 'server');
    const bundled = join(bundledRoot, 'bin', exe(probe.platform, 'turf-lsp'));
    tried.push(bundled);
    if (probe.exists(bundled)) {
        if (!settings.home) env.TURF_HOME = bundledRoot;
        return { command: bundled, source: 'bundled', env, tried };
    }

    const onPath = which('turf-lsp', probe);
    if (onPath) return { command: onPath, source: 'path', env, tried: [...tried, onPath] };
    tried.push(`turf-lsp on PATH (${pathEntries(probe).length} directories)`);

    const installed = join(probe.home, '.turf', 'bin', exe(probe.platform, 'turf-lsp'));
    tried.push(installed);
    if (probe.exists(installed)) return { command: installed, source: 'path', env, tried };
    return undefined;
}

/**
 * Where a `turf` that can compile and run programs might be, best guess first.
 * (Whether it works is for `turf version` to say: the `turf` that ships beside
 * the server is only the project-graph helper, with no compiler next to it.)
 */
export function toolchainCandidates(setting: string, probe: Probe): string[] {
    const join = joiner(probe.platform);
    const out: string[] = [];
    const add = (p: string | undefined) => { if (p && !out.includes(p)) out.push(p); };

    if (setting) {
        // A directory (`~/.turf`, `~/.turf/bin`) or the executable itself.
        add(setting.toLowerCase().endsWith('turf') || setting.toLowerCase().endsWith('turf.exe') ? setting : undefined);
        add(join(setting, 'bin', exe(probe.platform, 'turf')));
        add(join(setting, exe(probe.platform, 'turf')));
        return out;
    }
    add(which('turf', probe));
    if (probe.env.TURF_HOME) add(join(probe.env.TURF_HOME, 'bin', exe(probe.platform, 'turf')));
    add(join(probe.home, '.turf', 'bin', exe(probe.platform, 'turf')));
    if (probe.platform === 'win32' && probe.env.LOCALAPPDATA)
        add(join(probe.env.LOCALAPPDATA, 'turf', 'bin', 'turf.exe'));
    return out.filter(p => probe.exists(p));
}

export interface ServerVersion {
    version: string;
    compiler?: string;
    stdlib?: string;
    warnings: string[];
}

/** Parses `turf-lsp --version`: `turf-lsp 0.1.0 (LSP protocol 3.17)`, then `compiler`, `stdlib`, `warning:` lines. */
export function parseServerVersion(stdout: string): ServerVersion | undefined {
    const lines = stdout.split(/\r?\n/);
    const first = /^turf-lsp (\d+\.\d+\.\d+\S*)/.exec(lines[0] ?? '');
    if (!first) return undefined;
    const out: ServerVersion = { version: first[1], warnings: [] };
    for (const line of lines.slice(1)) {
        if (line.startsWith('compiler ')) out.compiler = line.slice('compiler '.length).trim();
        else if (line.startsWith('stdlib ')) out.stdlib = line.slice('stdlib '.length).trim();
        else if (line.startsWith('warning: ')) out.warnings.push(line.slice('warning: '.length).trim());
    }
    return out;
}

/** Negative, zero or positive as `a` is older than, the same as or newer than `b` (numeric parts only). */
export function compareVersions(a: string, b: string): number {
    const parts = (v: string) => v.split(/[.-]/).slice(0, 3).map(n => parseInt(n, 10) || 0);
    const pa = parts(a), pb = parts(b);
    for (let i = 0; i < 3; i++) {
        const d = (pa[i] ?? 0) - (pb[i] ?? 0);
        if (d !== 0) return d;
    }
    return 0;
}

/** The directory holding `turf.toml`, searching upward from `file`'s directory. */
export function findProjectRoot(file: string, exists: (p: string) => boolean, platform: NodeJS.Platform = process.platform): string | undefined {
    const p = platform === 'win32' ? path.win32 : path.posix;
    let dir = p.dirname(file);
    for (let depth = 0; depth < 64; depth++) {
        if (exists(p.join(dir, 'turf.toml'))) return dir;
        const parent = p.dirname(dir);
        if (parent === dir) return undefined;
        dir = parent;
    }
    return undefined;
}

/** The `turf` command line (after the executable) to run `command` for a file. */
export function commandLine(command: 'run' | 'build' | 'check' | 'test', file: string | undefined, inProject: boolean): string[] | undefined {
    if (command === 'test') return inProject ? ['test'] : undefined; // tests belong to a project
    if (inProject) return [command];
    // A single file: `turf run file.tr`, `turf check file.tr`, `turf compile file.tr` (a build).
    if (!file) return undefined;
    return [command === 'build' ? 'compile' : command, file];
}

/** A one-line, human description of where a server came from. */
export function describeSource(source: ServerSource): string {
    switch (source) {
        case 'setting': return 'from the turf.server.path setting';
        case 'bundled': return 'bundled with the extension';
        case 'path': return 'found on your PATH';
    }
}
