#!/usr/bin/env python3
"""Align markdown tables via `column -t -o '|' -s '|'`, skipping fenced
code blocks. Usage: tablealign.py FILE [FILE...]"""
import subprocess
import sys


def align(block):
    text = "".join(block)
    proc = subprocess.run(
        ["column", "-t", "-o", "|", "-s", "|"],
        input=text.encode(), capture_output=True)
    return proc.stdout.decode()


def process(path):
    with open(path) as f:
        content = f.read()
    lines = content.splitlines(keepends=True)
    out = []
    i = 0
    in_fence = False
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if stripped.startswith("```"):
            in_fence = not in_fence
            out.append(line)
            i += 1
            continue
        if not in_fence and stripped.startswith("|"):
            block = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                block.append(lines[i])
                i += 1
            out.append(align(block))
        else:
            out.append(line)
            i += 1
    with open(path, "w") as f:
        f.write("".join(out))


for p in sys.argv[1:]:
    process(p)
