#!/usr/bin/env python3
"""Inject Lab5 grading tests into a temporary student-tree copy.

Tests are intentionally inserted only at function boundaries:
  * proc_init: function tail
  * init_main: function entry

The injector does not depend on implementation-specific statements such as
"ret = 0", comments, labels, or a particular final call in a function.
"""

import os
import re
import shutil
import sys


def die(message):
    print("[lab5 inject] ERROR: " + message)
    raise SystemExit(1)


if len(sys.argv) != 2:
    die("Usage: inject.py <temporary grading copy>")

root = os.path.abspath(sys.argv[1])
tools = os.path.dirname(os.path.abspath(__file__))


def read_text(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def find_function(text, name):
    """Return (declaration_start, opening_brace, closing_brace_end).

    The brace scan ignores comments, strings, and character literals, so the
    matching brace remains correct in functions containing nested blocks.
    """
    pattern = re.compile(
        r"(?ms)^[ \t]*"
        r"(?:static[ \t]+)?"
        r"[A-Za-z_][A-Za-z0-9_ \t\n\*]*?"
        r"\b" + re.escape(name) +
        r"[ \t\n]*\([^;{}]*\)[ \t\n]*\{"
    )
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        die("Function location is missing or ambiguous: " + name)

    match = matches[0]
    body_start = text.find("{", match.start(), match.end())
    depth = 0
    in_line_comment = False
    in_block_comment = False
    in_string = False
    in_char = False
    escaped = False

    for i in range(body_start, len(text)):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
        elif in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
        elif in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
        elif in_char:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == "'":
                in_char = False
        else:
            if ch == "/" and nxt == "/":
                in_line_comment = True
            elif ch == "/" and nxt == "*":
                in_block_comment = True
            elif ch == '"':
                in_string = True
            elif ch == "'":
                in_char = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return match.start(), body_start, i + 1

    die("Function is missing its matching closing brace: " + name)


def insert_include(path, header, anchor):
    text = read_text(path)
    pattern = re.compile(
        r"(?m)^(?P<line>[ \t]*#include[ \t]*[<\"]"
        + re.escape(anchor)
        + r"[>\"][ \t]*)$"
    )
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        die("Include anchor is missing or ambiguous: " + anchor)
    match = matches[0]
    replacement = match.group("line") + "\n#include <" + header + ">"
    write_text(path, text[:match.start()] + replacement + text[match.end():])


def indent_code(code):
    return "\n" + "\n".join("    " + line for line in code.splitlines()) + "\n"


def insert_at_function_start(path, name, code):
    text = read_text(path)
    _, body_start, _ = find_function(text, name)
    insertion = indent_code(code)
    write_text(path, text[:body_start + 1] + insertion + text[body_start + 1:])


def insert_at_function_end(path, name, code):
    text = read_text(path)
    _, _, body_end = find_function(text, name)
    insertion = indent_code(code)
    write_text(path, text[:body_end - 1] + insertion + text[body_end - 1:])


makefile = os.path.join(root, "Makefile")
make = read_text(makefile)
if "kern/test" in make or "test_lab5.h" in make:
    die("The skeleton already contains test hooks; duplicate injection is not allowed")
if "--wrap=" in make:
    die("Detected leftover --wrap configuration; the grading skeleton must use direct injection")

testdir = os.path.join(root, "kern", "test")
os.makedirs(testdir, exist_ok=True)
for name in ("test_lab5.c", "test_lab5.h"):
    shutil.copyfile(os.path.join(tools, name), os.path.join(testdir, name))

for variable in ("KINCLUDE", "KSRCDIR"):
    match = re.search(
        r"(?m)^" + variable + r"[ \t]*\+=.*(?:\n[ \t]+[^\n]*)*",
        make,
    )
    if match is None:
        die("Makefile variable not found: " + variable)
    make = make[:match.end()] + "\n" + variable + "\t+= kern/test" + make[match.end():]
write_text(makefile, make)

proc = os.path.join(root, "kern", "process", "proc.c")
insert_include(proc, "test_lab5.h", "proc.h")
insert_at_function_end(proc, "proc_init", "test_lab5_process_create();")
insert_at_function_start(proc, "init_main", "test_lab5_syscall();\ntest_lab5_process_control();")

print("lab5 injection complete")