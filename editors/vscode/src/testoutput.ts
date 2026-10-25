// Reading `turf test`'s output. Pure (no `vscode`), so it is tested with plain
// Node against the exact text the tool prints.

const ANSI = /\x1b\[[0-9;]*m/g;

/** One test's outcome, parsed from a line of `turf test`'s output. */
export interface TestLine {
    /** The directory label turf prints in brackets: the test's immediate directory. */
    label: string;
    file: string;
    /** The `@test` function's name, when the line reports one test function of the file. */
    test?: string;
    passed: boolean;
    /** What follows the dots: `PASS`, `PASS (Rejected properly)`, `RUNTIME FAILED (Exit code 1)`, ... */
    detail: string;
}

/**
 * `[tests] ok.tr ... PASS`  /  `[tests] bad.tr ... RUNTIME FAILED (Exit code 1)`  /
 * `[tests] unit.tr: adds ... PASS` (one `@test` function of a file)
 */
export function parseTestLine(line: string): TestLine | undefined {
    const m = /^\[([^\]]+)\] (\S+\.tr)(?:: (\S+))? \.\.\. (.*)$/.exec(line.replace(ANSI, '').trimEnd());
    if (!m) return undefined;
    const parsed: TestLine = { label: m[1], file: m[2], passed: m[4].startsWith('PASS'), detail: m[4] };
    if (m[3] !== undefined) parsed.test = m[3];
    return parsed;
}

/**
 * The "Failed Tests Details" blocks: the test's path (`tests/bad.tr`) mapped to
 * the text under its `--- tests/bad.tr (Runtime Error exit 1):` header.
 */
export function parseFailureDetails(output: string): Map<string, string> {
    const clean = output.replace(ANSI, '');
    const details = new Map<string, string>();
    const start = clean.indexOf('Failed Tests Details:');
    if (start < 0) return details;
    let current: string | undefined;
    let body: string[] = [];
    const flush = () => { if (current !== undefined) details.set(current, body.join('\n').trim()); };
    for (const line of clean.slice(start).split(/\r?\n/).slice(1)) {
        const header = /^--- (.+?) \(/.exec(line);
        if (header) { flush(); current = header[1]; body = []; }
        else if (current !== undefined) body.push(line);
    }
    flush();
    return details;
}

/** One `@test` function of a file: its name and the 0-based line of its `fn`. */
export interface TestFunction {
    name: string;
    line: number;
}

/**
 * The `@test` functions a file declares, in order: a line that starts with
 * `@test` (after blanks), then, past any further annotation lines, a `fn` (with
 * or without `pub`). The same rule `turf test` uses to decide that a file holds
 * tests; the names are what it reports them by (`file.tr: name ... PASS`).
 */
export function findTestFunctions(text: string): TestFunction[] {
    const lines = text.split(/\r?\n/);
    const found: TestFunction[] = [];
    for (let i = 0; i < lines.length; i++) {
        if (!/^\s*@test(?![A-Za-z0-9_])/.test(lines[i])) continue;
        // `@test fn name(): void {` on one line, or the `fn` below it.
        for (let j = i; j < lines.length && j <= i + 8; j++) {
            const rest = j === i ? lines[j].replace(/^\s*@test(\([^)]*\))?/, '') : lines[j];
            const fn = /^\s*(?:pub(?:lic)?\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(/.exec(rest);
            if (fn) { found.push({ name: fn[1], line: j }); i = j; break; }
            if (j > i && !/^\s*(@|$|\/\/)/.test(lines[j])) break;
        }
    }
    return found;
}
