#!/usr/bin/env python3
"""(this fork) The settings menu's translation table (runtime/src/menu_i18n*.{cpp,inc}) against the sources.

Every tr("...") literal in runtime/src must have a row (its label: the text before "##") with a Spanish, Portuguese,
Italian, German and French text; each translation keeps the English text's printf conversions in the same order; no English text has
two rows. Exits 1 with the problems listed.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "runtime", "src")
LANGUAGES = ("Spanish", "Portuguese", "Italian", "German", "French")  # the columns after English
ESCAPES = {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'", "0": "\0", "r": "\r", "a": "\a"}


def literal(text, i):
    """the C string literal starting at text[i] == '"' -> (value, index after it)"""
    out, i = [], i + 1
    while text[i] != '"':
        c = text[i]
        if c == "\\":
            n = text[i + 1]
            if n == "x":
                m = re.match(r"[0-9a-fA-F]+", text[i + 2:])
                out.append(chr(int(m.group(0), 16)))
                i += 2 + len(m.group(0))
                continue
            if n in "01234567" and re.match(r"[0-7]{1,3}", text[i + 1:]).group(0) != "0":
                m = re.match(r"[0-7]{1,3}", text[i + 1:])
                out.append(chr(int(m.group(0), 8)))
                i += 1 + len(m.group(0))
                continue
            out.append(ESCAPES.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out), i + 1


def literals(text, i):
    """adjacent literals ("a" "b", across lines and comments) from text[i] -> (joined value, end) or (None, i)"""
    parts, j = [], i
    while True:
        m = re.compile(r'(?:\s|//[^\n]*\n|/\*.*?\*/)*', re.S).match(text, j)
        k = m.end()
        if k < len(text) and text[k] == '"':
            v, j = literal(text, k)
            parts.append(v)
            continue
        if text.startswith("u8\"", k):
            v, j = literal(text, k + 2)
            parts.append(v)
            continue
        break
    return ("".join(parts), j) if parts else (None, i)


def conversions(s):
    return [m.group(0)[-1] if m.group(0) != "%%" else "%" for m in
            re.finditer(r"%%|%[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfFgGaAcspn]", s)]


def label(s):
    k = s.find("##")
    return s if k < 0 else s[:k]


def table_rows():
    rows = []
    for name in sorted(os.listdir(SRC)):
        if not (name.startswith("menu_i18n") and name.endswith(".inc")):
            continue
        text = open(os.path.join(SRC, name), encoding="utf-8").read()
        for m in re.finditer(r"^\{", text, re.M):
            cols, j = [], m.end()
            for _ in range(len(LANGUAGES) + 1):
                v, j = literals(text, j)
                if v is None:
                    break
                cols.append(v)
                j = re.compile(r"\s*,?").match(text, j).end()
            line = text.count("\n", 0, m.start()) + 1
            rows.append((name, line, cols))
    return rows


def tr_uses():
    uses = []
    for dp, _, fns in os.walk(SRC):
        for fn in fns:
            if not fn.endswith((".cpp", ".h", ".mm")) or fn.startswith("menu_i18n"):
                continue
            if fn.startswith("game_settings_"):  # upstream's own tr() (game_settings_text.h: the in-game settings page)
                continue
            path = os.path.join(dp, fn)
            text = open(path, encoding="utf-8", errors="replace").read()
            for m in re.finditer(r"\btr\(\s*", text):
                v, _ = literals(text, m.end())
                if v is not None:
                    uses.append((os.path.relpath(path, ROOT), text.count("\n", 0, m.start()) + 1, v))
    return uses


def main():
    problems = []
    rows = table_rows()
    seen = {}
    for name, line, cols in rows:
        where = "%s:%d" % (name, line)
        if len(cols) != len(LANGUAGES) + 1:
            problems.append("%s: a row needs %d texts (English, %s)" % (where, len(LANGUAGES) + 1, ", ".join(LANGUAGES)))
            continue
        en = cols[0]
        if en in seen:
            problems.append("%s: %r has another row at %s" % (where, en, seen[en]))
        seen[en] = where
        if "##" in en:
            problems.append("%s: %r: the row's English text is the label only (no ##id)" % (where, en))
        for lang, t in zip(LANGUAGES, cols[1:]):
            if not t:
                problems.append("%s: %r has no %s text" % (where, en, lang))
            elif conversions(t) != conversions(en):
                problems.append("%s: %r: the %s text's conversions %s differ from %s" %
                                (where, en, lang, conversions(t), conversions(en)))
    for path, line, v in tr_uses():
        if label(v) and label(v) not in seen:
            problems.append("%s:%d: tr(%r) has no row" % (path, line, label(v)))
    if problems:
        print("menu_i18n_check: %d problem(s)" % len(problems))
        for p in problems:
            print("  " + p)
        return 1
    print("menu_i18n_check: OK (%d rows, %d tr() texts)" % (len(rows), len(tr_uses())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
