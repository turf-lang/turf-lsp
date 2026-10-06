// Unit tests for the parts of the extension that need no VS Code: where the
// server and the toolchain are looked for, how their output is read. Run with
// `npm test` (plain Node), on every platform in CI -- the Windows and macOS
// layouts are exercised here as data, whatever the machine running this is.

import * as assert from 'assert';
import {
    chooseServer, commandLine, compareVersions, findProjectRoot, parseServerVersion, Probe, toolchainCandidates, which,
} from '../resolve';
import { parseFailureDetails, parseTestLine } from '../testoutput';

let failures = 0;
function test(name: string, body: () => void): void {
    try {
        body();
        console.log(`ok   ${name}`);
    } catch (e) {
        failures++;
        console.log(`FAIL ${name}: ${(e as Error).message}`);
    }
}

/** A machine described by the files that exist on it. */
function machine(platform: NodeJS.Platform, files: string[], env: NodeJS.ProcessEnv = {}, home = '/home/me'): Probe {
    const set = new Set(files);
    return { platform, env, home, exists: f => set.has(f) };
}

// ── choosing the server ─────────────────────────────────────────────────────

test('the bundled server is used, with TURF_HOME at its own standard library', () => {
    const probe = machine('linux', ['/ext/server/bin/turf-lsp'], { TURF_HOME: '/some/other/turf' });
    const choice = chooseServer('/ext', { path: '', home: '' }, probe)!;
    assert.strictEqual(choice.command, '/ext/server/bin/turf-lsp');
    assert.strictEqual(choice.source, 'bundled');
    // A TURF_HOME in the user's environment must not hide the bundled library.
    assert.strictEqual(choice.env.TURF_HOME, '/ext/server');
});

test('turf.home wins over the bundled library', () => {
    const probe = machine('linux', ['/ext/server/bin/turf-lsp']);
    const choice = chooseServer('/ext', { path: '', home: '/work/turf' }, probe)!;
    assert.strictEqual(choice.env.TURF_HOME, '/work/turf');
});

test('turf.server.path is used as given, even if it does not exist', () => {
    const probe = machine('linux', ['/ext/server/bin/turf-lsp']);
    const choice = chooseServer('/ext', { path: '/opt/custom/turf-lsp', home: '' }, probe)!;
    assert.strictEqual(choice.command, '/opt/custom/turf-lsp');
    assert.strictEqual(choice.source, 'setting');
});

test('without a bundled server: PATH, then the per-user install, else nothing', () => {
    const onPath = machine('linux', ['/usr/local/bin/turf-lsp'], { PATH: '/usr/bin:/usr/local/bin' });
    assert.strictEqual(chooseServer('/ext', { path: '', home: '' }, onPath)!.command, '/usr/local/bin/turf-lsp');
    const installed = machine('linux', ['/home/me/.turf/bin/turf-lsp'], { PATH: '/usr/bin' });
    assert.strictEqual(chooseServer('/ext', { path: '', home: '' }, installed)!.command, '/home/me/.turf/bin/turf-lsp');
    assert.strictEqual(chooseServer('/ext', { path: '', home: '' }, machine('linux', [], { PATH: '/usr/bin' })), undefined);
});

test('Windows: the .exe, backslash paths, and a PATH separated by semicolons', () => {
    const ext = 'C:\\Users\\me\\.vscode\\extensions\\turflang.turf-0.1.0-win32-x64';
    const probe = machine('win32', [`${ext}\\server\\bin\\turf-lsp.exe`], {}, 'C:\\Users\\me');
    const choice = chooseServer(ext, { path: '', home: '' }, probe)!;
    assert.strictEqual(choice.command, `${ext}\\server\\bin\\turf-lsp.exe`);
    assert.strictEqual(choice.env.TURF_HOME, `${ext}\\server`);

    const onPath = machine('win32', ['C:\\tools\\turf\\turf-lsp.exe'], { Path: 'C:\\Windows;C:\\tools\\turf' }, 'C:\\Users\\me');
    assert.strictEqual(which('turf-lsp', onPath), 'C:\\tools\\turf\\turf-lsp.exe');
});

// ── the toolchain ───────────────────────────────────────────────────────────

test('toolchain candidates: PATH, TURF_HOME, the user install; only those that exist', () => {
    const probe = machine('linux', ['/usr/bin/turf', '/home/me/.turf/bin/turf', '/opt/turf/bin/turf'],
        { PATH: '/usr/bin', TURF_HOME: '/opt/turf' });
    assert.deepStrictEqual(toolchainCandidates('', probe), ['/usr/bin/turf', '/opt/turf/bin/turf', '/home/me/.turf/bin/turf']);
    assert.deepStrictEqual(toolchainCandidates('', machine('linux', [])), []);
});

test('toolchain.path may be the executable or an installation directory', () => {
    const probe = machine('linux', ['/opt/t/bin/turf', '/opt/direct/turf']);
    assert.deepStrictEqual(toolchainCandidates('/opt/t', probe).filter(p => probe.exists(p)), ['/opt/t/bin/turf']);
    assert.deepStrictEqual(toolchainCandidates('/opt/direct/turf', probe).filter(p => probe.exists(p)), ['/opt/direct/turf']);
});

test('Windows: the per-user install under %LOCALAPPDATA%', () => {
    const probe = machine('win32', ['C:\\Users\\me\\AppData\\Local\\turf\\bin\\turf.exe'],
        { LOCALAPPDATA: 'C:\\Users\\me\\AppData\\Local', Path: 'C:\\Windows' }, 'C:\\Users\\me');
    assert.deepStrictEqual(toolchainCandidates('', probe), ['C:\\Users\\me\\AppData\\Local\\turf\\bin\\turf.exe']);
});

// ── the server's version line ───────────────────────────────────────────────

test('turf-lsp --version is parsed, with its compiler, stdlib and warnings', () => {
    const v = parseServerVersion('turf-lsp 0.1.0 (LSP protocol 3.17)\ncompiler d54a43c5b6f0\nstdlib /ext/server (next to the server (../lib/std))\nwarning: TURF_HOME is set to \'/x\' but no\n')!;
    assert.strictEqual(v.version, '0.1.0');
    assert.strictEqual(v.compiler, 'd54a43c5b6f0');
    assert.strictEqual(v.stdlib, '/ext/server (next to the server (../lib/std))');
    assert.strictEqual(v.warnings.length, 1);
    assert.strictEqual(parseServerVersion('error: no such thing'), undefined);
    assert.strictEqual(parseServerVersion('turf-lsp 0.1.0 (LSP protocol 3.17)\r\nstdlib not found\r\n')!.stdlib, 'not found');
});

test('version comparison', () => {
    assert.ok(compareVersions('0.1.0', '0.1.0') === 0);
    assert.ok(compareVersions('0.2.0', '0.1.9') > 0);
    assert.ok(compareVersions('0.1.0', '1.0.0') < 0);
    assert.ok(compareVersions('2.0.0-dev', '2.0.0') === 0);
});

// ── projects and commands ───────────────────────────────────────────────────

test('the project root is the nearest directory with turf.toml', () => {
    const files = new Set(['/w/app/turf.toml', '/w/app/src/main.tr']);
    const has = (p: string) => files.has(p);
    assert.strictEqual(findProjectRoot('/w/app/src/deep/x.tr', has, 'linux'), '/w/app');
    assert.strictEqual(findProjectRoot('/w/loose/a.tr', has, 'linux'), undefined);
    const win = new Set(['C:\\w\\app\\turf.toml']);
    assert.strictEqual(findProjectRoot('C:\\w\\app\\src\\x.tr', p => win.has(p), 'win32'), 'C:\\w\\app');
});

test('the turf command line for a file or a project', () => {
    assert.deepStrictEqual(commandLine('run', '/w/app/src/main.tr', true), ['run']);
    assert.deepStrictEqual(commandLine('run', '/w/loose/a.tr', false), ['run', '/w/loose/a.tr']);
    assert.deepStrictEqual(commandLine('build', '/w/loose/a.tr', false), ['compile', '/w/loose/a.tr']);
    assert.deepStrictEqual(commandLine('check', '/w/loose/a.tr', false), ['check', '/w/loose/a.tr']);
    assert.deepStrictEqual(commandLine('test', '/w/app/tests/t.tr', true), ['test']);
    assert.strictEqual(commandLine('test', '/w/loose/a.tr', false), undefined); // tests belong to a project
    assert.strictEqual(commandLine('run', undefined, false), undefined);
});

// ── turf test output ────────────────────────────────────────────────────────

test('turf test result lines', () => {
    assert.deepStrictEqual(parseTestLine('[tests] ok.tr ... PASS'), { label: 'tests', file: 'ok.tr', passed: true, detail: 'PASS' });
    assert.strictEqual(parseTestLine('[tests] fail_reject.tr ... PASS (Rejected properly)')!.passed, true);
    const bad = parseTestLine('[tests] bad.tr ... RUNTIME FAILED (Exit code 1)')!;
    assert.strictEqual(bad.passed, false);
    assert.strictEqual(bad.detail, 'RUNTIME FAILED (Exit code 1)');
    assert.strictEqual(parseTestLine('\x1b[32m[sub] a.tr ... PASS\x1b[0m')!.label, 'sub');
    assert.strictEqual(parseTestLine('=== Test Summary ==='), undefined);
    assert.strictEqual(parseTestLine('Total Tests: 3'), undefined);
});

test('turf test failure details', () => {
    const output = [
        'Running Turf Test Suites...', '', '[tests] bad.tr ... RUNTIME FAILED (Exit code 1)', '',
        '=== Test Summary ===', 'Total Tests: 2', 'Passed:      1', 'Failed:      1', '',
        'Failed Tests Details:', '', '--- tests/bad.tr (Runtime Error exit 1):', ' boom happened', ' ---',
        '--- tests/other.tr (Compile Error):', ' bad syntax',
    ].join('\n');
    const details = parseFailureDetails(output);
    assert.strictEqual(details.size, 2);
    assert.ok(details.get('tests/bad.tr')!.includes('boom happened'));
    assert.ok(details.get('tests/other.tr')!.includes('bad syntax'));
    assert.strictEqual(parseFailureDetails('all fine').size, 0);
});

if (failures) {
    console.log(`${failures} test(s) failed`);
    process.exit(1);
}
console.log('extension unit tests: all passed');
