#!/usr/bin/env python3
import re
import sys

DURATION_RE = re.compile(r"^(\d+(?:\.\d+)?)s$")

rows = []
passthrough = []
for raw in sys.stdin:
    line = raw.rstrip("\n")
    if not line:
        continue
    parts = line.split("\t")
    if len(parts) == 3:
        t, subj, payload = (p.strip() for p in parts)
        rows.append((subj, payload))
    else:
        passthrough.append(line)

for line in passthrough:
    print(line)

if not rows:
    sys.exit(0)

HEADERS = ("subject", "payload")
widths = [len(h) for h in HEADERS]
for row in rows:
    for i, cell in enumerate(row):
        if len(cell) > widths[i]:
            widths[i] = len(cell)

def fmt(cells):
    return "  ".join(c.ljust(widths[i]) for i, c in enumerate(cells))

print(fmt(HEADERS))
print("  ".join("-" * w for w in widths))
for row in rows:
    print(fmt(row))
