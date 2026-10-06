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
import re
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

    def __init__(self, client, path, text, save=True):
        self.client, self.path, self.text, self.version = client, path, text, 1
        self.uri = uri_of(path)
        if save:  # False: the editor has it open with edits that are not on disk
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


COMPLETION_BASE = ("import io;\n\nstruct Point {\n    x: int,\n}\n\nfn helper(a: int): int {\n"
                   "    return a;\n}\n\nfn main(): int {\n    mut count: int = 1;\n    PLACE\n    return 0;\n}\n")


def complete_at(c, d, name, text, marker, offset=0, trigger=None):
    doc = Doc(c, d / name, text)
    try:
        doc.diagnostics(timeout=20)
    except TimeoutError:
        pass  # a half-typed buffer may not publish; the request itself must still work
    params = {"textDocument": doc.td(), "position": doc.pos(marker, offset=offset)}
    if trigger:
        params["context"] = {"triggerKind": 2, "triggerCharacter": trigger}
    res = c.result("textDocument/completion", params)
    return res.get("items", []) if isinstance(res, dict) else (res or [])


def t_completion_bare_word_scope(c, d):
    """What is offered for a bare word is what this file can use there: its
    locals, functions, types and imported modules -- not the locals of every
    stdlib function it pulls in, and not modules only `io` imports."""
    items = complete_at(c, d, "bare.tr", COMPLETION_BASE.replace("PLACE", "co"), "co\n", offset=2)
    got = labels(items)
    for want in ("count", "helper", "main", "Point", "io", "while", "int", "true"):
        check(want in got, "bare-word completion lacks %r" % want)
    for leaked in ("frac_div", "saw_digit", "readCount", "int_part", "list::List", "strings::StringBuilder"):
        check(leaked not in got, "bare-word completion offers %r, which is not usable here" % leaked)
    # `sys` and `list` are modules only `io` imports. They may be offered as
    # something to import, but never as if this file could already use them.
    for module in ("sys", "list"):
        for it in items:
            if it["label"] == module:
                check(it.get("additionalTextEdits"), "%r is offered as usable, but this file does not import it" % module)
    check(len(got) == len(set(got)), "duplicate completion labels: %r" % sorted(x for x in got if got.count(x) > 1)[:5])
    # Real symbols outrank keywords: `count` sorts before `while`.
    order = {i["label"]: i.get("sortText", i["label"]) for i in items}
    check(order["count"] < order["while"], "a local does not outrank a keyword")


def t_completion_auto_import(c, d):
    """Typing the name of a module the file has not imported offers it, and
    accepting it adds the `import` line (after the last import, or after the
    header comment when there are none)."""
    text = COMPLETION_BASE.replace("PLACE", "js")
    items = complete_at(c, d, "auto1.tr", text, "js\n", offset=2)
    pick = [i for i in items if i["label"] == "json"]
    check(len(pick) == 1, "json is not offered for auto-import: %r" % [i["label"] for i in items if i.get("kind") == 9])
    edits = pick[0].get("additionalTextEdits")
    check(edits and len(edits) == 1, "the item carries no import edit: %r" % pick[0])
    check(apply_edits(text, edits).startswith("import io;\nimport json;\n"),
          "the import is not added after the existing one:\n" + apply_edits(text, edits)[:80])
    # Already imported: offered as the module itself, with no edit.
    io = [i for i in complete_at(c, d, "auto2.tr", COMPLETION_BASE.replace("PLACE", "i"), "i\n", offset=1)
          if i["label"] == "io"]
    check(io and "additionalTextEdits" not in io[0], "an imported module should not re-import: %r" % io)
    # No imports: after the header comment, with a blank line.
    bare = "// the header\n\nfn main(): int {\n    js\n    return 0;\n}\n"
    pick = [i for i in complete_at(c, d, "auto3.tr", bare, "js\n", offset=2) if i["label"] == "json"]
    check(pick, "no auto-import in a file with no imports")
    check(apply_edits(bare, pick[0]["additionalTextEdits"]).startswith("// the header\n\nimport json;\n\nfn main"),
          "the import is not put after the header:\n" + apply_edits(bare, pick[0]["additionalTextEdits"])[:80])


def t_completion_regions(c, d):
    """Declarations at the top level, statements in a function, only types in
    a struct body."""
    top = labels(complete_at(c, d, "top.tr", "import io;\n\nf\n\nfn main(): int {\n    return 0;\n}\n", "f\n", offset=1))
    for want in ("fn", "struct", "enum", "import", "pub", "extern"):
        check(want in top, "top-level completion lacks %r" % want)
    for nope in ("while", "return", "compare", "true"):
        check(nope not in top, "top-level completion offers the statement keyword %r" % nope)
    body = labels(complete_at(c, d, "body.tr", COMPLETION_BASE.replace("PLACE", "w"), "w\n", offset=1))
    for want in ("while", "return", "compare", "if", "for", "defer", "recover", "try"):
        check(want in body, "statement completion lacks %r" % want)
    for nope in ("fn", "import", "extern", "pub"):
        check(nope not in body, "statement completion offers the declaration keyword %r" % nope)
    fields = labels(complete_at(c, d, "fields.tr", "struct P {\n    x: i\n}\n\nfn main(): int {\n    return 0;\n}\n", "i\n", offset=1))
    check("int" in fields and "string" in fields, "a field's type is not completed: %r" % fields)
    check("while" not in fields and "fn" not in fields, "a struct body offers statements: %r" % fields)


SYNTAX_COMPLAINT = ("Expected", "Unexpected", "Unclosed", "Statements are not allowed", "Missing")


def fill_placeholders(snippet):
    snippet = re.sub(r"\$\{\d+:([^}]*)\}", r"\1", snippet)
    snippet = re.sub(r"\$\{\d+\}", "", snippet)
    return re.sub(r"\$\d+", "", snippet)  # a bare tab stop: $0, $2, ...


def t_completion_snippets_parse(c, d):
    """Every snippet the server offers must be valid TurfLang once its
    placeholders are filled in: a snippet that inserts a syntax error is worse
    than none."""
    top = complete_at(c, d, "stop.tr", "import io;\n\nf\n\nfn main(): int {\n    return 0;\n}\n", "f\n", offset=1)
    stmt = complete_at(c, d, "sstmt.tr", COMPLETION_BASE.replace("PLACE", "w"), "w\n", offset=1)
    seen = 0
    for region, items in (("top", top), ("stmt", stmt)):
        for it in items:
            snippet = it.get("insertText")
            if not snippet or it.get("insertTextFormat") != 2 or it["label"] == "assert":
                continue
            body = fill_placeholders(snippet)
            if it["label"] in ("else", "elseif"):  # they continue an `if`, so they are only valid after one
                body = "if true {\n    }\n    " + body
            if region == "top":
                text = "import io;\n\n" + body + "\n\nfn main(): int {\n    return 0;\n}\n"
            else:
                text = COMPLETION_BASE.replace("PLACE", body)
            doc = Doc(c, d / ("snip_%s_%s.tr" % (region, re.sub(r"\W", "_", it["label"]))), text)
            diags = doc.diagnostics(timeout=20)
            # A call such as `helper(a)` is an expression the user ends with
            # `;` themselves; that alone is not a defect in the snippet.
            bad = [x["message"].split("\n")[0] for x in diags
                   if x["message"].startswith(SYNTAX_COMPLAINT) and not x["message"].startswith("Expected ';'")]
            check(not bad, "snippet %r (%s) is not valid TurfLang: %s\n%s" % (it["label"], region, bad[:2], body))
            seen += 1
    check(seen >= 12, "only %d snippets were checked; the server offers fewer than expected" % seen)


def signature_at(c, d, name, text, marker, offset):
    doc = Doc(c, d / name, text)
    try:
        doc.diagnostics(timeout=20)
    except TimeoutError:
        pass
    return c.result("textDocument/signatureHelp", {"textDocument": doc.td(), "position": doc.pos(marker, offset=offset)})


SIGNATURE_SRC = """import io;

struct Point {
    x: int,
}

fn Point.scaled(factor: int, offset: int): int {
    return self.x * factor + offset;
}

fn add(a: int, b: int): int {
    return a + b;
}

fn main(): int {
    mut p: Point = Point::default();
    mut r: int = add(1, 2);
    io::printline("x");
    r = p.scaled(2, 3);
    return r;
}
"""


def t_signature_help(c, d):
    """Typing a call's arguments shows its parameters and which one is next."""
    check(c.init["capabilities"].get("signatureHelpProvider"), "signatureHelpProvider is not advertised")
    # Cursor just after `add(` and just after the comma: parameter 0, then 1.
    sig = signature_at(c, d, "sig1.tr", SIGNATURE_SRC, "add(1, 2)", 4)
    check(sig and sig["signatures"], "no signature help inside add(...): %r" % sig)
    check(sig["signatures"][0]["label"].startswith("add(int a, int b)"), "label: %r" % sig["signatures"][0]["label"])
    check(sig["activeParameter"] == 0, "active parameter after '(' is %r" % sig["activeParameter"])
    sig = signature_at(c, d, "sig2.tr", SIGNATURE_SRC, "add(1, 2)", 7)
    check(sig["activeParameter"] == 1, "active parameter after the comma is %r" % sig["activeParameter"])
    check([p["label"] for p in sig["signatures"][0]["parameters"]] == ["int a", "int b"], "parameters: %r" % sig["signatures"][0])
    # A method called on a value: `self` is the receiver, not an argument.
    sig = signature_at(c, d, "sig3.tr", SIGNATURE_SRC, "scaled(2, 3)", 10)
    check(sig and [p["label"] for p in sig["signatures"][0]["parameters"]] == ["int factor", "int offset"],
          "method parameters should omit self: %r" % sig)
    check(sig["activeParameter"] == 1, "method active parameter %r" % sig["activeParameter"])
    # A module function.
    sig = signature_at(c, d, "sig4.tr", SIGNATURE_SRC, 'printline("x")', 10)
    check(sig and sig["signatures"][0]["label"].startswith("printline("), "module function: %r" % sig)
    # Outside any call there is nothing to show.
    check(signature_at(c, d, "sig5.tr", SIGNATURE_SRC, "return r;", 3) is None, "signature help outside a call")


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


MANIFEST = '[package]\nname = "%s"\nversion = "0.1.0"\nturf = "2.0"\n%s'


def make_project(d, dirname="proj"):
    """proj depends on dep (a path dependency); proj has src/lib/util.tr, which
    sits in a subdirectory, so only the package graph makes `import util;` work."""
    proj, dep = d / dirname, d / "dep"
    for p in (proj / "src" / "lib", proj / "tests", dep / "src"):
        p.mkdir(parents=True, exist_ok=True)
    (proj / "turf.toml").write_text(MANIFEST % ("proj", '\n[dependencies]\ndep = { path = "../dep" }\n'))
    (dep / "turf.toml").write_text(MANIFEST % ("dep", ""))
    (dep / "src" / "dep.tr").write_text("pub fn answer(): int {\n    return 21;\n}\n")
    (proj / "src" / "lib" / "util.tr").write_text(
        "pub fn double_it(x: int): int {\n    return x * 2;\n}\n\n"
        "fn hidden_helper(): int {\n    return 0;\n}\n")  # private: not for other modules
    return proj


PROJECT_MAIN = """import io;
import util;
import dep;

fn main(): int {
    io::printline("x");
    return util::double_it(dep::answer());
}
"""


def need_turf():
    if H.turf is None:
        raise Skipped("needs a `turf` binary (--turf, or --stdlib-from a checkout with build/turf)")


def t_project_imports_resolve(c, d):
    """A file in a `turf new` project sees its own modules (anywhere under
    src/) and its dependencies, exactly as `turf build` does."""
    need_turf()
    proj = make_project(d)
    doc = Doc(c, proj / "src" / "main.tr", PROJECT_MAIN)
    diags = doc.diagnostics(timeout=30)
    check(diags == [], "a valid project file has diagnostics: %s" % json.dumps(diags)[:600])
    loc = c.result("textDocument/definition", {"textDocument": doc.td(), "position": doc.pos("double_it", offset=2)})
    loc = loc[0] if isinstance(loc, list) else loc
    check(loc and loc["uri"].endswith("/src/lib/util.tr"), "definition into a project module: %r" % loc)
    got = labels(c.result("textDocument/completion", {
        "textDocument": doc.td(), "position": doc.pos("util::double_it", offset=6),
        "context": {"triggerKind": 2, "triggerCharacter": ":"}}))
    check("double_it" in got, "util:: completion lacks double_it: %r" % got[:15])
    check("hidden_helper" not in got, "util:: offers a private function another module may not call")


def t_paths_with_spaces_and_unicode(c, d):
    """A project in a directory like `My Projects/café` is the ordinary case on
    a desktop. The editor percent-encodes it in URIs; the server must decode it
    (it once did not), or nothing resolves."""
    need_turf()
    proj = make_project(d / "My Projects", "café app")
    doc = Doc(c, proj / "src" / "main.tr", PROJECT_MAIN)
    check("%20" in doc.uri and "%C3%A9" in doc.uri, "the fixture is not percent-encoded: %s" % doc.uri)
    diags = doc.diagnostics(timeout=30)
    check(diags == [], "a project under a path with spaces and accents has diagnostics: %s" % json.dumps(diags)[:500])
    loc = c.result("textDocument/definition", {"textDocument": doc.td(), "position": doc.pos("double_it", offset=2)})
    loc = loc[0] if isinstance(loc, list) else loc
    check(loc and loc["uri"].endswith("/src/lib/util.tr") and "%20" in loc["uri"],
          "definition does not return a properly encoded URI: %r" % loc)


def t_project_import_names(c, d):
    """`import <cursor>` offers the project's own modules and its dependency,
    not just the standard library and the files next to this one."""
    need_turf()
    proj = make_project(d)
    other = Doc(c, proj / "src" / "other.tr", "import \n\nfn f(): int {\n    return 0;\n}\n")
    c.wait_for("textDocument/publishDiagnostics", lambda p: p["uri"] == other.uri, 30, other.opened_at)
    got = labels(c.result("textDocument/completion", {
        "textDocument": other.td(), "position": {"line": 0, "character": 7}}))
    for want in ("util", "dep", "io", "json"):
        check(want in got, "import completion lacks %r: %r" % (want, got[:20]))
    check("other" not in got, "a file is offered its own name")


def t_project_new_module(c, d):
    """A module created while the server is running is importable by the
    very next analysis (no restart, no stale directory listing)."""
    need_turf()
    proj = make_project(d)
    doc = Doc(c, proj / "src" / "main.tr", PROJECT_MAIN)
    check(doc.diagnostics(timeout=30) == [], "the project does not start clean")
    (proj / "src" / "late.tr").write_text("pub fn late(): int {\n    return 7;\n}\n")
    text = "import late;\n\nfn main(): int {\n    return late::late();\n}\n"
    mark = c.mark()
    c.notify("textDocument/didChange", {"textDocument": {"uri": doc.uri, "version": 2},
                                        "contentChanges": [{"text": text}]})
    got = c.wait_for("textDocument/publishDiagnostics", lambda p: p["uri"] == doc.uri, 30, mark)
    check(got["diagnostics"] == [], "a module created after startup is not found: %s" % json.dumps(got["diagnostics"])[:400])


def t_unsaved_edit_of_another_file_is_seen(c, d):
    """An import sees the text of a module the user is editing, saved or not."""
    need_turf()
    proj = make_project(d)
    util = proj / "src" / "lib" / "util.tr"
    edited = util.read_text() + "\npub fn triple(x: int): int {\n    return x * 3;\n}\n"
    Doc(c, util, edited, save=False)  # open in the editor, edits NOT saved
    check("triple" not in util.read_text(), "the fixture saved the edit; the test would mean nothing")
    main = Doc(c, proj / "src" / "main.tr",
               "import util;\n\nfn main(): int {\n    return util::triple(2);\n}\n")
    diags = main.diagnostics(timeout=30)
    check(diags == [], "an unsaved function in an open module is not seen: %s" % json.dumps(diags)[:400])


def t_project_broken_manifest_is_reported(c, d):
    """A turf.toml that does not parse is the user's to fix: say so on the file
    instead of failing every import with nothing to go on."""
    need_turf()
    proj = d / "badproj"
    (proj / "src").mkdir(parents=True, exist_ok=True)
    (proj / "turf.toml").write_text("[package\nname = \n")
    doc = Doc(c, proj / "src" / "main.tr", "fn main(): int {\n    return 0;\n}\n")
    diags = doc.diagnostics(timeout=30)
    check(any("turf graph failed" in x["message"] for x in diags),
          "a broken turf.toml is not reported: %s" % json.dumps(diags)[:400])
    syms = c.result("textDocument/documentSymbol", {"textDocument": doc.td()})
    check("main" in json.dumps(syms), "the file is not analysed on its own after a manifest error")


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


def t_code_action_add_missing_import(c, d):
    """`io::printline(...)` without `import io;` is the commonest first mistake;
    the lightbulb must offer the import, placed with the other imports."""
    text = "// a comment\nimport math;\n\nfn main(): int {\n    io::printline(\"hi\");\n    return 0;\n}\n"
    doc = Doc(c, d / "noimport.tr", text)
    diags = [x for x in doc.diagnostics() if x.get("code") == "E031" and "'io'" in x["message"]]
    check(diags, "no E031 for the missing import")
    actions = c.result("textDocument/codeAction", {
        "textDocument": doc.td(), "range": diags[0]["range"], "context": {"diagnostics": diags}})
    fix = [a for a in actions if a["title"] == "Add `import io;`"]
    check(fix, "no 'Add import io' quick fix: %r" % [a["title"] for a in actions])
    edited = apply_edits(text, fix[0]["edit"]["changes"][doc.uri])
    check(edited.startswith("// a comment\nimport math;\nimport io;\n"), "the import is not placed after the others:\n" + edited[:80])
    # A name that is no module offers nothing.
    text2 = "fn main(): int {\n    nosuchthing::f();\n    return 0;\n}\n"
    doc2 = Doc(c, d / "noimport2.tr", text2)
    diags2 = [x for x in doc2.diagnostics() if x.get("code") == "E031"]
    if diags2:
        acts = c.result("textDocument/codeAction", {"textDocument": doc2.td(), "range": diags2[0]["range"],
                                                    "context": {"diagnostics": diags2}})
        check(not [a for a in acts if a["title"].startswith("Add `import")], "an import is offered for a name that is no module")


def t_code_action_organize_imports(c, d):
    text = "import net;\nimport io;\nimport io;\n\nfn main(): int {\n    return 0;\n}\n"
    doc = Doc(c, d / "organize.tr", text)
    doc.diagnostics()
    actions = c.result("textDocument/codeAction", {
        "textDocument": doc.td(), "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}},
        "context": {"diagnostics": []}})
    org = [a for a in actions if a["kind"] == "source.organizeImports"]
    check(org, "no organize-imports action for unsorted imports: %r" % [a["title"] for a in actions])
    check(apply_edits(text, org[0]["edit"]["changes"][doc.uri]).startswith("import io;\nimport net;\n\nfn main"),
          "imports not sorted and de-duplicated")
    clean = Doc(c, d / "organized.tr", "import io;\nimport net;\n\nfn main(): int {\n    return 0;\n}\n")
    clean.diagnostics()
    actions = c.result("textDocument/codeAction", {
        "textDocument": clean.td(), "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}},
        "context": {"diagnostics": []}})
    check(not [a for a in actions if a["kind"] == "source.organizeImports"], "organize offered for sorted imports")


def t_formatting_touches_only_the_lines_that_differ(c, d):
    """Replacing the whole document moves the cursor and fills the undo
    history; the edits must be just the lines that change."""
    text = "fn a(): int {\n    return 1;\n}\n\nfn b(): int {\nreturn 2;\n}\n\nfn c(): int {\n    return 3;\n}\n"
    doc = Doc(c, d / "min.tr", text)
    doc.diagnostics()
    edits = c.result("textDocument/formatting", {"textDocument": doc.td(), "options": {"tabSize": 4, "insertSpaces": True}})
    check(len(edits) == 1, "expected one edit for one mis-indented line, got %d: %r" % (len(edits), edits))
    r = edits[0]["range"]
    check(r["start"]["line"] == 5 and r["end"]["line"] == 6, "the edit does not target just line 5: %r" % r)
    check(edits[0]["newText"] == "        return 2;\n".replace("        ", "    "), "newText: %r" % edits[0]["newText"])


def t_range_formatting(c, d):
    """Only the selected lines are reformatted."""
    check(c.init["capabilities"].get("documentRangeFormattingProvider"), "documentRangeFormattingProvider is not advertised")
    text = "fn a(): int {\nreturn 1;\n}\n\nfn b(): int {\nreturn 2;\n}\n"
    doc = Doc(c, d / "rng.tr", text)
    doc.diagnostics()
    edits = c.result("textDocument/rangeFormatting", {
        "textDocument": doc.td(), "options": {"tabSize": 4, "insertSpaces": True},
        "range": {"start": {"line": 4, "character": 0}, "end": {"line": 6, "character": 1}}})
    out = apply_edits(text, edits)
    check("fn a(): int {\nreturn 1;\n}" in out, "a line outside the range was reformatted:\n" + out)
    check("fn b(): int {\n    return 2;\n}" in out, "the selected function was not reformatted:\n" + out)


def t_formatting_refuses_a_file_that_does_not_lex(c, d):
    """A source the lexer rejects is not formatted -- not mangled."""
    text = 'fn main(): int {\nmut s: string = "open;\nreturn 0;\n}\n'
    doc = Doc(c, d / "nolex.tr", text)
    try:
        doc.diagnostics(timeout=10)
    except TimeoutError:
        pass
    check(not c.result("textDocument/formatting", {"textDocument": doc.td(), "options": {"tabSize": 4, "insertSpaces": True}}),
          "edits were returned for a file with a lexical error")


def t_folding_ranges(c, d):
    text = "// a\n// b\nimport io;\nimport json;\n\nfn main(): int {\n    if (true) {\n        return 1;\n    }\n    return 0;\n}\n"
    doc = Doc(c, d / "fold.tr", text)
    check(c.init["capabilities"].get("foldingRangeProvider"), "foldingRangeProvider is not advertised")
    got = c.result("textDocument/foldingRange", {"textDocument": doc.td()})
    have = {(r["startLine"], r["endLine"], r.get("kind", "")) for r in got}
    for want in ((0, 1, "comment"), (2, 3, "imports"), (5, 9, ""), (6, 7, "")):
        check(want in have, "folding range %r missing; got %r" % (want, sorted(have)))


def t_document_links(c, d):
    """An import's module name opens the module's file."""
    doc = Doc(c, d / "links.tr", "import io;\nimport nosuchmodule;\n\nfn main(): int {\n    return 0;\n}\n")
    check(c.init["capabilities"].get("documentLinkProvider"), "documentLinkProvider is not advertised")
    links = c.result("textDocument/documentLink", {"textDocument": doc.td()})
    check(len(links) == 1, "expected a link for io only (nosuchmodule resolves to nothing): %r" % links)
    check(links[0]["target"].endswith("/io.tr"), "the link does not open io.tr: %r" % links[0])
    r = links[0]["range"]
    check(r["start"] == {"line": 0, "character": 7} and r["end"] == {"line": 0, "character": 9}, "link range: %r" % r)


TESTS = [(n[2:], f) for n, f in sorted(globals().items()) if n.startswith("t_")]
H = None


class Harness:
    """Where the servers under test come from, and what they run next to."""

    def __init__(self, server, stdlib_from, work, turf=None):
        self.server, self.stdlib_from, self.work, self.turf = server, stdlib_from, work, turf

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
        if self.turf and with_stdlib:
            shutil.copy2(self.turf, root / "bin" / self.turf.name)
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
    ap.add_argument("--turf", help="the `turf` command line tool to stage beside the server (default: "
                    "build/turf in the --stdlib-from checkout, if there is one)")
    ap.add_argument("--only")
    args = ap.parse_args()
    server = pathlib.Path(args.server)
    if not server.exists():
        print("FAIL: %s not built" % server)
        return 1

    work = pathlib.Path(tempfile.mkdtemp(prefix="turf-lsp-smoke-"))
    stdlib_from = pathlib.Path(args.stdlib_from).resolve() if args.stdlib_from else None
    turf = pathlib.Path(args.turf) if args.turf else (stdlib_from / "build" / "turf" if stdlib_from else None)
    if turf is not None and not turf.is_file():
        turf = None
    H = Harness(server.resolve(), stdlib_from, work, turf.resolve() if turf else None)
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
