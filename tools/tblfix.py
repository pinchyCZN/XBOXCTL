#!/usr/bin/env python3
"""Normalise +---+ / | tables so every row in a block is the same width.

A block is a maximal run of lines whose first non-space char is '+' or '|'.
Border lines (+---+ / +===+) are stretched by widening their LAST segment;
data rows (| ... |) by padding before the final '|'.
"""
import sys


def width(s):
    return len(s.rstrip("\n"))


def fix_border(line, target):
    line = line.rstrip("\n")
    pad = target - len(line)
    if pad <= 0:
        return line
    # widen the last segment: insert before the trailing '+'
    fill = line[-2] if len(line) >= 2 and line[-2] in "-=" else "-"
    return line[:-1] + fill * pad + "+"


def fix_row(line, target):
    line = line.rstrip("\n")
    pad = target - len(line)
    if pad <= 0:
        return line
    return line[:-1] + " " * pad + "|"


def process(path):
    with open(path, "r", encoding="ascii") as f:
        lines = f.read().split("\n")

    out = []
    i = 0
    changed = 0
    while i < len(lines):
        if lines[i].lstrip()[:1] in ("+", "|") and lines[i].strip():
            j = i
            while (j < len(lines) and lines[j].strip()
                   and lines[j].lstrip()[:1] in ("+", "|")):
                j += 1
            block = lines[i:j]
            target = max(width(b) for b in block)
            for b in block:
                if width(b) == target:
                    out.append(b.rstrip())
                    continue
                changed += 1
                s = b.lstrip()
                if s.startswith("+"):
                    out.append(fix_border(b, target))
                else:
                    out.append(fix_row(b, target))
            i = j
        else:
            out.append(lines[i])
            i += 1

    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out))
    print("%s: %d rows normalised" % (path, changed))


for p in sys.argv[1:]:
    process(p)
