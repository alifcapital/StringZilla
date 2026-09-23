#!/usr/bin/env python3
"""Build INITCAP vectors with Python Unicode 17 categories and UCD simple mappings."""

import argparse
import struct
import unicodedata
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--ucd-dir", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
args = p.parse_args()
assert unicodedata.unidata_version == "17.0.0"
upper, lower = {}, {}
for line in (args.ucd_dir / "UnicodeData.txt").read_text().splitlines():
    f = line.split(";")
    cp = int(f[0], 16)
    if f[12]:
        upper[cp] = chr(int(f[12], 16))
    if f[13]:
        lower[cp] = chr(int(f[13], 16))
with args.output.open("wb") as out:
    source, expected = "", ""
    for cp in range(0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        c = chr(cp)
        category = unicodedata.category(c)
        word = category.startswith("L") or category == "Nd"
        source += c + "A A" + c + "A "
        expected += (upper.get(cp, c) if word else c) + ("a A" if word else "A A")
        expected += (lower.get(cp, c) if word else c) + ("a " if word else "A ")
        if len(source) >= 8192 or cp == 0x10FFFF:
            a, b = source.encode(), expected.encode()
            out.write(struct.pack("<II", len(a), len(b)))
            out.write(a)
            out.write(b)
            source, expected = "", ""
print("Unicode 17 INITCAP corpus:", args.output)
