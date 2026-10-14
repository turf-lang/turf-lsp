// Starts a real VS Code with the PACKAGED extension installed and runs the suite
// in suite.ts inside its extension host: the closest an automated test gets to
// "install it, open a .tr file, start programming".
//
//   node out/test/integration/runTest.js path/to/turf-<target>-<version>.vsix
//
// It installs the .vsix with VS Code's own CLI into an empty extensions
// directory, so what is tested is the artifact a user installs -- the bundled
// server, the bundled standard library, the compiled extension -- not the source
// tree. A throwaway profile and workspace are used; nothing of the user's is
// touched. On Linux it needs a display (xvfb-run -a ... in CI).

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { spawnSync } from 'child_process';
import { downloadAndUnzipVSCode, resolveCliArgsFromVSCodeExecutablePath, runTests } from '@vscode/test-electron';

async function main(): Promise<void> {
    const vsix = process.argv[2];
    if (!vsix || !fs.existsSync(vsix)) {
        console.error('usage: runTest.js <path to the .vsix to test>');
        process.exit(2);
    }

    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'turf-vscode-it-'));
    const extensionsDir = path.join(root, 'extensions');
    const userDataDir = path.join(root, 'user-data');
    const workspace = path.join(root, 'ws', 'My Project'); // a space on purpose: the ordinary desktop case
    fs.mkdirSync(workspace, { recursive: true });

    const vscodeExecutablePath = await downloadAndUnzipVSCode(process.env.VSCODE_VERSION || 'stable');
    const [cli, ...cliArgs] = resolveCliArgsFromVSCodeExecutablePath(vscodeExecutablePath);

    const asRoot = process.getuid?.() === 0;
    const install = spawnSync(cli, [...cliArgs, '--extensions-dir', extensionsDir, '--user-data-dir', userDataDir,
        ...(asRoot ? ['--no-sandbox'] : []), '--install-extension', path.resolve(vsix), '--force'],
        // `input`: under WSL the downloaded Linux CLI asks "use it anyway? [y/N]".
        { encoding: 'utf8', shell: process.platform === 'win32', input: 'y\n' });
    process.stdout.write(install.stdout ?? '');
    process.stderr.write(install.stderr ?? '');
    if (install.status !== 0) throw new Error(`installing the .vsix failed (exit ${install.status})`);

    // The extension under test is the INSTALLED one; the "development" extension
    // is an empty stand-in that only carries the test runner's entry point.
    const stub = path.join(root, 'stub');
    fs.mkdirSync(stub, { recursive: true });
    fs.writeFileSync(path.join(stub, 'package.json'), JSON.stringify({ name: 'turf-it-stub', publisher: 'turflang-test', version: '0.0.0', engines: { vscode: '*' } }));

    // A TURF_HOME in the tester's environment (some other Turf) must not matter:
    // the bundled server uses its own standard library.
    await runTests({
        vscodeExecutablePath,
        extensionDevelopmentPath: stub,
        extensionTestsPath: path.resolve(__dirname, 'suite'),
        extensionTestsEnv: { TURF_IT_WORKSPACE: workspace },
        launchArgs: [workspace, '--extensions-dir', extensionsDir, '--user-data-dir', userDataDir,
            '--disable-workspace-trust', '--skip-welcome', '--skip-release-notes', '--disable-gpu',
            // Chromium will not start as root with its sandbox on; CI's Linux
            // job runs in a container as root (build.yml), a desktop never does.
            ...(asRoot ? ['--no-sandbox'] : [])],
    });
}

main().catch(err => {
    console.error(err);
    process.exit(1);
});
