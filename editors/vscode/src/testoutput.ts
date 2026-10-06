// Reading `turf test`'s output. Pure (no `vscode`), so it is tested with plain
// Node against the exact text the tool prints.

const ANSI = /\x1b\[[0-9;]*m/g;

/** One test's outcome, parsed from a line of `turf test`'s output. */
export interface TestLine {
    /** The directory label turf prints in brackets: the test's immediate directory. */
    label: string;
    file: string;
    passed: boolean;
    /** What follows the dots: `PASS`, `PASS (Rejected properly)`, `RUNTIME FAILED (Exit code 1)`, ... */
    detail: string;
}

/** `[tests] ok.tr ... PASS`  /  `[tests] bad.tr ... RUNTIME FAILED (Exit code 1)` */
export function parseTestLine(line: string): TestLine | undefined {
    const m = /^\[([^\]]+)\] (\S+\.tr) \.\.\. (.*)$/.exec(line.replace(ANSI, '').trimEnd());
    if (!m) return undefined;
    return { label: m[1], file: m[2], passed: m[3].startsWith('PASS'), detail: m[3] };
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
