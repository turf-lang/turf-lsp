#!/usr/bin/env python3
"""Fail if the VS Code TextMate grammar does not mention every reserved word of
the language (as `turf-lsp --print-keywords` reports them), or mentions a word
that is no longer one.

The grammar is static JSON, so it can only be kept honest by checking it against
the compiler's own table: a keyword added to the language gets no highlighting
until someone remembers the grammar, and a removed one keeps highlighting that
lies. Run by CI after the server is built.

    scripts/check_grammar.py [--server build/turf-lsp]
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GRAMMAR = ROOT / "editors" / "vscode" / "syntaxes" / "turf.tmLanguage.json"

# Words the grammar highlights that are not in the lexer's keyword table, on
# purpose: `float` is a keyword (a synonym of double) and appears there, so
# nothing is exempt today. Add a word here only with a reason.
EXEMPT = {}


def words_in(pattern):
    """The alternatives of every `\\b(a|b|c)\\b` group in a match regex."""
    out = set()
    for group in re.findall(r"\\b\(([A-Za-z0-9_|]+)\)\\b", pattern):
        out.update(group.split("|"))
    # A rule for a single word: `\bself\b`.
    out.update(re.findall(r"\\b([A-Za-z_][A-Za-z0-9_]*)\\b", pattern))
    return out


def grammar_words():
    grammar = json.loads(GRAMMAR.read_text())
    found = {}  # word -> the rule name that colours it

    def walk(node):
        if isinstance(node, dict):
            match, name = node.get("match"), node.get("name", "")
            if match and name.split(".")[0] in ("keyword", "storage", "constant", "variable"):
                for w in words_in(match):
                    found.setdefault(w, name)
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(grammar)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default=str(ROOT / "build" / "turf-lsp"))
    args = ap.parse_args()

    out = subprocess.run([args.server, "--print-keywords"], capture_output=True, text=True)
    if out.returncode != 0:
        print("FAIL: %s --print-keywords: %s" % (args.server, out.stderr.strip() or "exit %d" % out.returncode))
        return 1
    language = {}
    for line in out.stdout.splitlines():
        name, kind = line.split()
        language[name] = kind

    highlighted = grammar_words()
    failed = 0
    for word, kind in sorted(language.items()):
        if word not in highlighted:
            print("FAIL: %r (%s) is a reserved word the grammar does not highlight" % (word, kind))
            failed += 1
    for word, rule in sorted(highlighted.items()):
        if word not in language and word not in EXEMPT:
            print("FAIL: the grammar highlights %r (%s), which is not a reserved word" % (word, rule))
            failed += 1
    if failed:
        return 1
    print("check_grammar: all %d reserved words are highlighted, and nothing else is" % len(language))
    return 0


if __name__ == "__main__":
    sys.exit(main())
