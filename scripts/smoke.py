#!/usr/bin/env python3
"""End-to-end smoke test for turf-lsp, over stdio, like an editor drives it.

Starts the real server binary, opens fixtures and asks for each feature the
README promises, asserting on what comes back. Needs nothing but Python 3 and
a built server; the fixtures are written to a temporary directory.

    scripts/smoke.py [--server build/turf-lsp] [--turf-home DIR] [--only NAME]

--turf-home DIR      set TURF_HOME for the server (a compiler checkout or an
                     installed toolchain). Without it TURF_HOME is *unset*, so
                     the server must find the standard library from where its
                     own binary lives -- the "install it and it works" case.
--only NAME          run just the tests whose name contains NAME.

Exit status 0 when every test passes. Each failure prints the test and why.
"""

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent


# ── A minimal LSP client ────────────────────────────────────────────────────

class ServerDied(Exception):
    """The server process is gone; no later test can mean anything."""


class Client:
    def __init__(self, server, env, log_path):
        self.log = open(log_path, "wb")
        self.proc = subprocess.Popen(
            [server], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.log, env=env)
        self.messages = []          # every message from the server, in order
        self.cv = threading.Condition()
        self.next_id = 0
        self.closed = False
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        out = self.proc.stdout
        while True:
            length = None
            while True:
                line = out.readline()
                if not line:
                    with self.cv:
                        self.closed = True
                        self.cv.notify_all()
                    return
                line = line.strip()
                if not line:
                    break
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            if length is None:
                continue
            msg = json.loads(out.read(length).decode("utf-8"))
            # Requests the server makes of the client get an empty answer.
            if "method" in msg and "id" in msg:
                self._send({"jsonrpc": "2.0", "id": msg["id"], "result": None})
            with self.cv:
                self.messages.append(msg)
                self.cv.notify_all()

    def _send(self, obj):
        body = json.dumps(obj).encode("utf-8")
        try:
            self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            raise ServerDied("the server exited (status %s)" % self.proc.poll())

    def notify(self, method, params):
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method, params, timeout=30):
        self.next_id += 1
        rid = self.next_id
        self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        deadline = time.time() + timeout
        with self.cv:
            while True:
                for m in self.messages:
                    if m.get("id") == rid and "method" not in m:
                        return m
                if self.closed:
                    raise ServerDied("the server exited while waiting for " + method)
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError("no response to " + method)
                self.cv.wait(left)

    def result(self, method, params, timeout=30):
        resp = self.request(method, params, timeout)
        if "error" in resp:
            raise RuntimeError("%s failed: %s" % (method, resp["error"]))
        return resp.get("result")

    def wait_for(self, method, pred=lambda p: True, timeout=15, after=0):
        """The first notification `method` (index >= after) whose params satisfy pred."""
        deadline = time.time() + timeout
        with self.cv:
            while True:
                for i, m in enumerate(self.messages):
                    if i >= after and m.get("method") == method and "id" not in m \
                            and pred(m.get("params")):
                        return m["params"]
                if self.closed:
                    raise ServerDied("the server exited while waiting for " + method)
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError("no %s notification" % method)
                self.cv.wait(left)

    def mark(self):
        with self.cv:
            return len(self.messages)

    def handshake(self, root_uri):
        self.init = self.result("initialize", {
            "processId": os.getpid(), "rootUri": root_uri,
            "capabilities": {"textDocument": {"completion": {"completionItem": {"snippetSupport": True}}},
                             "general": {"positionEncodings": ["utf-16"]}}})
        self.notify("initialized", {})
        return self

    def stop(self):
        try:
            self.request("shutdown", None, timeout=10)
            self.notify("exit", None)
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
        for stream in (self.proc.stdin, self.proc.stdout):
            try:
                stream.close()
            except Exception:  # noqa: BLE001 - the server may already be gone
                pass
        self.log.close()


def uri_of(path):
    return "file://" + urllib.parse.quote(str(path))


class Doc:
    """An open document: its text, uri, and 0-based positions of things in it."""

    def __init__(self, client, path, text):
        self.client, self.path, self.text, self.version = client, path, text, 1
        self.uri = uri_of(path)
        pathlib.Path(path).write_text(text)
        mark = client.mark()
        client.notify("textDocument/didOpen", {"textDocument": {
            "uri": self.uri, "languageId": "turf", "version": 1, "text": text}})
        self.opened_at = mark

    def td(self):
        return {"uri": self.uri}

    def pos(self, needle, nth=0, offset=0):
        """0-based LSP position of the nth occurrence of needle, plus offset chars."""
        idx = -1
        for _ in range(nth + 1):
            idx = self.text.index(needle, idx + 1)
        line = self.text.count("\n", 0, idx)
        col = idx - (self.text.rfind("\n", 0, idx) + 1)
        return {"line": line, "character": col + offset}

    def diagnostics(self, timeout=15):
        return self.client.wait_for(
            "textDocument/publishDiagnostics",
            lambda p: p["uri"] == self.uri, timeout, self.opened_at)["diagnostics"]


# ── Checks ──────────────────────────────────────────────────────────────────

class Failure(Exception):
    pass


def check(cond, why):
    if not cond:
        raise Failure(why)


def labels(items):
    if isinstance(items, dict):
        items = items.get("items", [])
    return [i["label"] for i in (items or [])]


FEATURES = """import io;

struct Point {
    x: int,
    y: int
}

fn Point.sum(): int {
    return self.x + self.y;
}

fn add(a: int, b: int): int {
    return a + b;
}

fn main(): int {
    mut p: Point = Point::default();
    p.x = add(1, 2);
    io::printline("hi");
    return p.sum();
}
"""

WITH_ERROR = """fn main(): int {
    return missing;
}
"""

MESSY = """fn main(): int {
        mut x: int = 1;
    if (x == 1) {
  x = 2;
    }


    return x;
}
"""

FORMATTED = """fn main(): int {
    mut x: int = 1;
    if (x == 1) {
        x = 2;
    }

    return x;
}
"""


def apply_edits(text, edits):
    """Apply LSP TextEdits (0-based line/character, ASCII fixtures) to text."""
    lines = text.split("\n")
    offs = [0]
    for ln in lines:
        offs.append(offs[-1] + len(ln) + 1)

    def at(p):
        return offs[p["line"]] + p["character"]
    for e in sorted(edits, key=lambda e: at(e["range"]["start"]), reverse=True):
        text = text[:at(e["range"]["start"])] + e["newText"] + text[at(e["range"]["end"]):]
    return text


# ── The tests ───────────────────────────────────────────────────────────────

def t_initialize(c, d):
    caps = c.init["capabilities"]
    for k in ("hoverProvider", "definitionProvider", "referencesProvider",
              "documentSymbolProvider", "documentFormattingProvider",
              "renameProvider", "completionProvider", "semanticTokensProvider"):
        check(caps.get(k), "server does not advertise " + k)


def t_diagnostics_error_width(c, d):
    doc = Doc(c, d / "err.tr", WITH_ERROR)
    diags = doc.diagnostics()
    check(len(diags) >= 1, "no diagnostic for an undefined variable")
    dg = diags[0]
    check(dg["severity"] == 1, "an undefined variable is not an error: %r" % dg)
    check("missing" in dg["message"], "message does not name the variable: %r" % dg["message"])
    check(dg.get("code"), "diagnostic has no code: %r" % dg)
    r = dg["range"]
    width = r["end"]["character"] - r["start"]["character"]
    check(r["start"]["line"] == 1 and r["end"]["line"] == 1, "wrong line: %r" % r)
    # The squiggle must cover the whole identifier, not a single byte.
    check(width == len("missing"), "squiggle is %d wide, want %d" % (width, len("missing")))


def t_diagnostics_clean(c, d):
    doc = Doc(c, d / "clean.tr", FEATURES)
    diags = doc.diagnostics()
    check(diags == [], "a valid program has diagnostics: %r" % diags)


def t_diagnostics_update_on_change(c, d):
    doc = Doc(c, d / "chg.tr", FEATURES)
    check(doc.diagnostics() == [], "valid program has diagnostics")
    mark = c.mark()
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": doc.uri, "version": 2},
        "contentChanges": [{"text": WITH_ERROR}]})
    got = c.wait_for("textDocument/publishDiagnostics",
                     lambda p: p["uri"] == doc.uri and len(p["diagnostics"]) > 0, 15, mark)
    check("missing" in got["diagnostics"][0]["message"], "stale or wrong diagnostic after an edit")


def t_hover(c, d):
    doc = Doc(c, d / "hover.tr", FEATURES)
    doc.diagnostics()
    h = c.result("textDocument/hover", {"textDocument": doc.td(), "position": doc.pos("add(1", offset=1)})
    check(h and "add" in json.dumps(h), "hover on a function call names nothing: %r" % h)
    h = c.result("textDocument/hover", {"textDocument": doc.td(), "position": doc.pos("p.x", offset=0)})
    check(h and "Point" in json.dumps(h), "hover on a struct variable lacks its type: %r" % h)


def t_definition_same_file(c, d):
    doc = Doc(c, d / "def.tr", FEATURES)
    doc.diagnostics()
    loc = c.result("textDocument/definition", {"textDocument": doc.td(), "position": doc.pos("add(1", offset=1)})
    loc = loc[0] if isinstance(loc, list) else loc
    check(loc and loc["uri"] == doc.uri, "definition left the file: %r" % loc)
    check(loc["range"]["start"]["line"] == doc.pos("fn add")["line"], "wrong definition line: %r" % loc)


def t_definition_into_stdlib(c, d):
    doc = Doc(c, d / "defstd.tr", FEATURES)
    doc.diagnostics()
    loc = c.result("textDocument/definition", {"textDocument": doc.td(), "position": doc.pos("printline", offset=2)})
    loc = loc[0] if isinstance(loc, list) else loc
    check(loc and loc["uri"].endswith("/io.tr"), "definition of io::printline did not reach the stdlib: %r" % loc)


def t_completion_module_members(c, d):
    doc = Doc(c, d / "cmod.tr", FEATURES.replace('io::printline("hi");', 'io::;'))
    c.wait_for("textDocument/publishDiagnostics", lambda p: p["uri"] == doc.uri, 15, doc.opened_at)
    got = labels(c.result("textDocument/completion", {
        "textDocument": doc.td(), "position": doc.pos("io::;", offset=4),
        "context": {"triggerKind": 2, "triggerCharacter": ":"}}))
    check("printline" in got and "print" in got, "io:: completion lacks printline/print: %r" % got[:15])


def t_completion_instance_members(c, d):
    doc = Doc(c, d / "cinst.tr", FEATURES.replace("return p.sum();", "p.;\n    return 0;"))
    c.wait_for("textDocument/publishDiagnostics", lambda p: p["uri"] == doc.uri, 15, doc.opened_at)
    got = labels(c.result("textDocument/completion", {
        "textDocument": doc.td(), "position": doc.pos("p.;", offset=2),
        "context": {"triggerKind": 2, "triggerCharacter": "."}}))
    for want in ("x", "y", "sum"):
        check(want in got, "p. completion lacks %r: %r" % (want, got[:15]))


def t_completion_import_names(c, d):
    doc = Doc(c, d / "cimp.tr", "import \n\nfn main(): int {\n    return 0;\n}\n")
    c.wait_for("textDocument/publishDiagnostics", lambda p: p["uri"] == doc.uri, 15, doc.opened_at)
    got = labels(c.result("textDocument/completion", {
        "textDocument": doc.td(), "position": {"line": 0, "character": 7}}))
    for want in ("io", "json", "math", "net"):
        check(want in got, "import completion lacks the stdlib module %r: %r" % (want, got[:15]))


def t_document_symbols(c, d):
    doc = Doc(c, d / "sym.tr", FEATURES)
    doc.diagnostics()
    syms = c.result("textDocument/documentSymbol", {"textDocument": doc.td()})
    names = json.dumps(syms)
    for want in ("Point", "add", "main"):
        check(want in names, "document symbols lack %r" % want)


def t_semantic_tokens(c, d):
    doc = Doc(c, d / "tok.tr", FEATURES)
    doc.diagnostics()
    toks = c.result("textDocument/semanticTokens/full", {"textDocument": doc.td()})
    check(toks and len(toks["data"]) >= 5 * 10, "too few semantic tokens: %r" % (toks and len(toks["data"]) // 5))


def t_references(c, d):
    doc = Doc(c, d / "ref.tr", FEATURES)
    doc.diagnostics()
    refs = c.result("textDocument/references", {
        "textDocument": doc.td(), "position": doc.pos("add(1", offset=1),
        "context": {"includeDeclaration": True}})
    check(refs and len(refs) >= 2, "references of add: %r" % refs)


def t_rename_local_and_refuse_stdlib(c, d):
    doc = Doc(c, d / "ren.tr", FEATURES)
    doc.diagnostics()
    ws = c.result("textDocument/rename", {
        "textDocument": doc.td(), "position": doc.pos("add(1", offset=1), "newName": "plus"})
    edits = [e for es in (ws.get("changes") or {}).values() for e in es]
    check(len(edits) >= 2 and all(e["newText"] == "plus" for e in edits), "rename edits: %r" % ws)
    resp = c.request("textDocument/rename", {
        "textDocument": doc.td(), "position": doc.pos("printline", offset=2), "newName": "shout"})
    check("error" in resp, "renaming a stdlib function was allowed: %r" % resp)


def t_formatting(c, d):
    doc = Doc(c, d / "fmt.tr", MESSY)
    doc.diagnostics()
    edits = c.result("textDocument/formatting", {
        "textDocument": doc.td(), "options": {"tabSize": 4, "insertSpaces": True}})
    check(edits, "no edits for an unformatted file")
    check(apply_edits(MESSY, edits) == FORMATTED, "formatted text is wrong:\n" + apply_edits(MESSY, edits))
    doc2 = Doc(c, d / "fmt2.tr", FORMATTED)
    doc2.diagnostics()
    check(not c.result("textDocument/formatting", {
        "textDocument": doc2.td(), "options": {"tabSize": 4, "insertSpaces": True}}),
        "edits for an already formatted file")


class Skipped(Exception):
    pass


def t_version_reports_the_stdlib(c, d):
    if H.stdlib_from is None:
        raise Skipped("needs --stdlib-from")
    staged = H.stage("version", with_stdlib=True)
    out = subprocess.run([str(staged), "--version"], capture_output=True, text=True, env=H.clean_env())
    check(out.returncode == 0, "--version failed: %r" % out.stderr)
    lines = out.stdout.splitlines()
    check(lines[0].startswith("turf-lsp "), "first line is not the version: %r" % lines[:1])
    check(any(l.startswith("compiler ") for l in lines), "no compiler revision line: %r" % lines)
    root = str(staged.parent.parent)
    check(any(l.startswith("stdlib " + root) for l in lines),
          "--version does not report the stdlib next to the server (%s): %r" % (root, lines))


def t_stdlib_missing_is_reported_not_fatal(c, d):
    """No standard library anywhere near the server: it must say so in the
    editor, stay up, and answer -- not exit, which is what it used to do."""
    if H.stdlib_from is None:
        raise Skipped("needs --stdlib-from")
    cl = H.start(H.stage("bare", with_stdlib=False), "bare")
    try:
        msg = cl.wait_for("window/showMessage", lambda p: "standard library" in p["message"], 10)
        check(msg["type"] == 1, "the missing stdlib is not an error message: %r" % msg)
        doc = Doc(cl, d / "nostd.tr", "import io;\n\nfn main(): int {\n    return 0;\n}\n")
        diags = doc.diagnostics()
        check(any(x.get("code") == "E027" for x in diags), "no 'module not found' diagnostic: %r" % diags)
        syms = cl.result("textDocument/documentSymbol", {"textDocument": doc.td()})
        check("main" in json.dumps(syms), "the server stopped answering requests")
    finally:
        cl.stop()


def t_bad_turf_home_is_ignored(c, d):
    """A stale TURF_HOME in the user's environment must not break imports."""
    if H.stdlib_from is None:
        raise Skipped("needs --stdlib-from")
    cl = H.start(H.stage("badhome", with_stdlib=True), "badhome",
                 env_extra={"TURF_HOME": "/nonexistent-turf-home"})
    try:
        msg = cl.wait_for("window/showMessage", lambda p: "TURF_HOME" in p["message"], 10)
        check(msg["type"] == 2, "a bad TURF_HOME is not reported as a warning: %r" % msg)
        doc = Doc(cl, d / "badhome.tr", FEATURES)
        check(doc.diagnostics() == [], "imports fail although the stdlib is next to the server")
        got = labels(cl.result("textDocument/completion", {
            "textDocument": doc.td(), "position": doc.pos("printline", offset=0)}))
        check(got != [], "no completion")
    finally:
        cl.stop()


TESTS = [(n[2:], f) for n, f in sorted(globals().items()) if n.startswith("t_")]
H = None


class Harness:
    """Where the servers under test come from, and what they run next to."""

    def __init__(self, server, stdlib_from, work):
        self.server, self.stdlib_from, self.work = server, stdlib_from, work

    @staticmethod
    def clean_env():
        env = dict(os.environ)
        env.pop("TURF_HOME", None)
        env.pop("TURF_COMPILER", None)
        return env

    def stage(self, name, with_stdlib):
        """An install laid out as the extension bundles it: <root>/bin/turf-lsp
        and <root>/lib/std. The binary is copied, not linked: the server finds
        its stdlib from where its real executable lives."""
        root = self.work / ("install-" + name)
        (root / "bin").mkdir(parents=True)
        target = root / "bin" / self.server.name
        shutil.copy2(self.server, target)
        if with_stdlib:
            shutil.copytree(self.stdlib_from / "lib" / "std", root / "lib" / "std")
        return target

    def start(self, server_path, name, env_extra=None):
        env = self.clean_env()
        env.update(env_extra or {})
        return Client(str(server_path), env, self.work / (name + ".log")).handshake(uri_of(self.work))


def main():
    global H
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default=str(ROOT / "build" / "turf-lsp"))
    ap.add_argument("--turf-home")
    ap.add_argument("--stdlib-from", help="a compiler checkout or Turf install whose lib/std is "
                    "copied next to a staged copy of the server, which then runs with TURF_HOME unset")
    ap.add_argument("--only")
    args = ap.parse_args()
    server = pathlib.Path(args.server)
    if not server.exists():
        print("FAIL: %s not built" % server)
        return 1

    work = pathlib.Path(tempfile.mkdtemp(prefix="turf-lsp-smoke-"))
    H = Harness(server.resolve(), pathlib.Path(args.stdlib_from).resolve() if args.stdlib_from else None, work)
    if args.stdlib_from and not (H.stdlib_from / "lib" / "std").is_dir():
        print("FAIL: %s has no lib/std" % args.stdlib_from)
        return 1

    # Which server the feature tests run against:
    #   --stdlib-from  a staged install, TURF_HOME unset ("install it and it works")
    #   --turf-home    the server where it is built, with that TURF_HOME
    #   neither        the server where it is built, TURF_HOME unset
    env = H.clean_env()
    if args.turf_home:
        env["TURF_HOME"] = args.turf_home
    main_server = H.stage("main", with_stdlib=True) if H.stdlib_from else server
    log = work / "server.log"
    client = Client(str(main_server), env, log)
    failed = skipped = 0
    try:
        client.handshake(uri_of(work))
        for name, fn in TESTS:
            if args.only and args.only not in name:
                continue
            try:
                fn(client, work)
                print("ok   %s" % name)
            except Skipped as e:
                skipped += 1
                print("skip %s: %s" % (name, e))
            except ServerDied as e:
                failed += 1
                print("FAIL %s: %s -- the remaining tests did not run; see the server log" % (name, e))
                break
            except Exception as e:  # noqa: BLE001 - report every kind of failure
                failed += 1
                why = str(e) if isinstance(e, Failure) else "".join(
                    traceback.format_exception_only(type(e), e)).strip()
                print("FAIL %s: %s" % (name, why))
    finally:
        client.stop()
    note = " (%d skipped)" % skipped if skipped else ""
    print("smoke: %s%s (server log: %s)" % ("%d failed" % failed if failed else "all passed", note, log))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
