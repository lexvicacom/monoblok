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
    if len(parts) in (3, 4):
        t, subj, payload = (p.strip() for p in parts[:3])
        headers = parts[3].strip() if len(parts) == 4 else ""
        rows.append((subj, payload, headers))
    else:
        passthrough.append(line)

for line in passthrough:
    print(line)

if not rows:
    sys.exit(0)

show_headers = any(row[2] for row in rows)
if show_headers:
    HEADERS = ("subject", "payload", "headers")
else:
    HEADERS = ("subject", "payload")
    rows = [(subject, payload) for subject, payload, headers in rows]
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
