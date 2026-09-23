#!/usr/bin/env python3
"""Write independent CPython Unicode 17 lower/upper/fold reference vectors."""

import argparse
import random
import struct
import sys
import unicodedata
from pathlib import Path


def generate(output):
    if unicodedata.unidata_version != "17.0.0":
        raise RuntimeError("Use CPython with Unicode 17.0.0, e.g. Python 3.15; got " + unicodedata.unidata_version)
    rng = random.Random(170003)
    count = 0
    with output.open("wb") as f:

        def record(value):
            nonlocal count
            if isinstance(value, bytes):
                text = value.decode("utf-8", "surrogateescape")
            else:
                text = value
                value = text.encode("utf-8")
            outputs = [value] + [
                getattr(text, op)().encode("utf-8", "surrogateescape") for op in ("lower", "upper", "casefold")
            ]
            f.write(struct.pack("<4I", *map(len, outputs)))
            for data in outputs:
                f.write(data)
            count += 1

        record("")
        all_chars = [chr(cp) for cp in range(0x110000) if not 0xD800 <= cp < 0xE000]
        for char in all_chars:
            record(char)
        record("".join(all_chars))
        case_chars = [c for c in all_chars if c.lower() != c or c.upper() != c or c.casefold() != c]
        for char in case_chars:
            record(char * 65)
            record(("ỹ" + char + "ἀΣ\u0345\u0301A ") * 17)
        special = [
            "Straße ẞ",
            "Größe",
            "ΆΊΌΑΆ",
            "ỹ" + "ἀ" * 10,
            "İIıi",
            "ΐΰﬃև",
            "ΟΣ",
            "ΟΣΑ",
            "ΣΣ",
            "AΣ\u0301",
            "AΣ\u0301B",
            "A\u0345Σ",
            "AΣ\u0345B",
            "\u0345Σ",
            "AΣ'B",
            "AΣ' ",
            "AΣ\x00B",
            "AΣ" + "\u0301" * 4096 + "B",
            "АБВГДабвгдЁё",
            "ქართული ᲥᲐᲠᲗᲣᲚᲘ",
            "Հայերեն",
            "ＡａＺｚ",
            "\U00016ea0\U00016ebb\ua7ce\ua7cf",
        ]
        for value in special:
            for offset in range(65):
                for repeat in (1, 3, 17):
                    record("a" * offset + value * repeat + "Z")
        for size in range(1, 194):
            record("Ά" * size)
            record("ỹ" + "ἀ" * size)
            record(("AΣ\u0301 ") * size)
        alphabet = case_chars + list(" aAZ09-.,\x00\u0301\u0345\u200d\u2060😀中文日本語")
        for i in range(10000):
            record("".join(rng.choices(alphabet, k=rng.randrange(1, 260))))
        for _ in range(5000):
            record(bytes(rng.randrange(256) for _ in range(rng.randrange(1, 200))))
        for invalid in [
            b"\xff",
            b"\xc0\x80",
            b"\xed\xa0\x80",
            b"\xf4\x90\x80\x80",
            b"\xe1",
            b"\xe1\xb8",
            b"\xf0\x90\x80",
        ]:
            for offset in range(65):
                record(b"A" * offset + invalid + "ΟΣ".encode())
                record("AΣ".encode() + invalid + b"A" * offset)
    print(
        f"CPython {sys.version.split()[0]}, Unicode {unicodedata.unidata_version}: {count} vectors, "
        f"{output.stat().st_size} bytes"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    generate(parser.parse_args().output)
