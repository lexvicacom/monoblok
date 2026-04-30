#!/usr/bin/env python3
import re
import sys

HEADERS = ("time", "subject", "payload")

DURATION_RE = re.compile(r"^(\d+(?:\.\d+)?)s$")

def fmt_time(cell):
    m = DURATION_RE.match(cell)
    if not m:
        return cell
    return "T+{:.2f}s".format(float(m.group(1)))

rows = []
passthrough = []
for raw in sys.stdin:
    line = raw.rstrip("\n")
    if not line:
        continue
    parts = line.split("\t")
    if len(parts) == 3:
        t, subj, payload = (p.strip() for p in parts)
        rows.append((fmt_time(t), subj, payload))
    else:
        passthrough.append(line)

for line in passthrough:
    print(line)

if not rows:
    sys.exit(0)

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
print("(time = T+seconds since this subscriber started)")
