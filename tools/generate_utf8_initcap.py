#!/usr/bin/env python3
"""Generate Unicode 17 simple case mappings and Letter/Decimal_Number ranges."""

import argparse
import collections
import hashlib
import subprocess
from pathlib import Path

from generate_utf8_case import FAMILIES, SIMD


def generate(ucd):
    assert "SpecialCasing-17.0.0.txt" in (ucd / "SpecialCasing.txt").read_text()
    text = (ucd / "UnicodeData.txt").read_text()
    maps = {"lower": [], "upper": []}
    ranges = []
    first = None
    for line in text.splitlines():
        f = line.split(";")
        cp = int(f[0], 16)
        if f[1].endswith(", First>"):
            first = cp
            continue
        lo = first if f[1].endswith(", Last>") else cp
        first = None
        if f[2].startswith("L") or f[2] == "Nd":
            if ranges and ranges[-1][1] + 1 == lo:
                ranges[-1] = (ranges[-1][0], cp)
            else:
                ranges.append((lo, cp))
        for mode, col in [("lower", 13), ("upper", 12)]:
            if f[col]:
                target = int(f[col], 16)
                assert len(chr(target).encode()) <= 3 * len(chr(cp).encode())
                maps[mode].append((cp, target - cp))
    out = [
        "/* Generated from Unicode 17.0.0 by tools/generate_utf8_initcap.py. See UNICODE-LICENSE.txt. */",
        "/* UnicodeData.txt SHA256: " + hashlib.sha256(text.encode()).hexdigest() + " */",
        "#ifndef STRINGZILLA_UTF8_INITCAP_TABLES_H_",
        "#define STRINGZILLA_UTF8_INITCAP_TABLES_H_",
        '#include "stringzilla/utf8_case/tables.h"',
        "typedef struct sz_unicode_simple_case_t { sz_rune_t first, last; sz_i32_t delta; sz_u8_t step; } sz_unicode_simple_case_t;",
    ]
    for mode, entries in maps.items():
        out.append(f"static sz_unicode_simple_case_t const sz_unicode_simple_{mode}_[] = {{")
        i = 0
        while i < len(entries):
            lo, delta = entries[i]
            hi = lo
            step = entries[i + 1][0] - lo if i + 1 < len(entries) else 1
            if step not in (1, 2):
                step = 1
            i += 1
            while i < len(entries) and entries[i] == (hi + step, delta):
                hi += step
                i += 1
            out.append(f"    {{0x{lo:X}, 0x{hi:X}, {delta}, {step}}},")
        out.append("};")
    out.append("static sz_unicode_case_range_t const sz_unicode_alnum_ranges_[] = {")
    out.extend(f"    {{0x{lo:X}, 0x{hi:X}}}," for lo, hi in ranges)
    out.extend(["};", "#endif", ""])
    return "\n".join(out)


def simd_family(simd, family, prefixes, maps, alnum):
    o = [
        f"SZ_HELPER_AUTO sz_size_t sz_utf8_initcap_{simd.isa}_{family}_({simd.v} v, sz_ptr_t target, sz_bool_t *word_start) {{"
    ]
    if simd.avx2:
        o += ["    __m256i prev1 = sz_haswell_previous_bytes_(v, 1);"]
        if any(len(p) == 2 for p in prefixes):
            o += ["    __m256i prev2 = sz_haswell_previous_bytes_(v, 2);"]
        o += [
            "    __m256i continuations = sz_haswell_in_byte_range_(v, 0x80, 0x40);",
            "    sz_u32_t allowed = ~(sz_u32_t)_mm256_movemask_epi8(v);",
        ]
    else:
        o += [
            "    sz_u64_t continuations = _mm512_cmplt_epu8_mask(_mm512_sub_epi8(v, _mm512_set1_epi8((char)0x80)), _mm512_set1_epi8(0x40));",
            "    sz_u64_t allowed = ~(sz_u64_t)_mm512_movepi8_mask(v);",
        ]
    ascii_letters = simd.select(list(range(65, 91)) + list(range(97, 123)))
    ascii_alnum = simd.either([ascii_letters, simd.select(list(range(48, 58)))])
    o += [f"    {simd.bits} alnum = {simd.mask(ascii_alnum)};", f"    {simd.bits} stop = 0;"]
    groups = collections.defaultdict(list)
    for prefix in prefixes:
        name, expr = simd.prefix(prefix)
        o += [
            f"    {simd.m} {name} = {expr};",
            f"    {simd.bits} {name}_bits = {simd.mask(name)};",
            "    allowed |= " + " | ".join(f"({name}_bits >> {i})" for i in range(len(prefix) + 1)) + ";",
        ]
        words = []
        specials = []
        deltas = collections.defaultdict(list)
        for last in range(0x80, 0xC0):
            raw = prefix + bytes([last])
            cp = ord(raw.decode())
            if cp in alnum:
                words.append(last)
            for mode in ("lower", "upper"):
                mapped = chr(maps[mode].get(cp, cp) if cp in alnum else cp).encode()
                if len(mapped) != len(raw):
                    specials.append(last)
                elif mapped != raw:
                    deltas[(mode, tuple((b - a) % 256 for a, b in zip(raw, mapped)))].append(last)
        if words:
            word = name if len(words) == 64 else simd.both(name, simd.select(words))
            o += [
                f"    {simd.bits} {name}_word = {simd.mask(word)};",
                "    alnum |= " + " | ".join(f"({name}_word >> {i})" for i in range(len(prefix) + 1)) + ";",
            ]
        if specials:
            o += [f"    stop |= ({simd.mask(simd.both(name, simd.select(sorted(set(specials)))))}) >> {len(prefix)};"]
        for key, values in deltas.items():
            groups[key].append(simd.both(name, simd.select(values)))
    o += [
        "    stop |= ~allowed;",
        f"    sz_size_t length = stop ? (sz_size_t)_tzcnt_u{simd.width}(stop) : {simd.width};",
        "    if (!length) return 0;",
        f"    {simd.bits} starts = alnum & ~((alnum << 1) | (*word_start ? 0 : 1));",
        "    *word_start = (sz_bool_t)!((alnum >> (length - 1)) & 1);",
    ]
    if simd.avx2:
        o += [
            "    __m256i capitalize = sz_utf8_initcap_haswell_expand_mask_(starts);",
            f"    __m256i letters = {ascii_letters};",
            "    __m256i result = _mm256_or_si256(v, _mm256_and_si256(letters, _mm256_set1_epi8(32)));",
            "    result = _mm256_sub_epi8(result, _mm256_and_si256(_mm256_and_si256(capitalize, letters), _mm256_set1_epi8(32)));",
        ]
    else:
        o += [
            f"    sz_u64_t letters = {ascii_letters};",
            "    __m512i result = _mm512_mask_mov_epi8(v, letters, _mm512_or_si512(v, _mm512_set1_epi8(32)));",
            "    result = _mm512_mask_sub_epi8(result, starts & letters, result, _mm512_set1_epi8(32));",
        ]
    for idx, ((mode, delta), masks) in enumerate(groups.items()):
        offset = len(delta) - 1
        match = simd.either(masks)
        if simd.avx2:
            upper = f"sz_haswell_previous_bytes_(capitalize, {offset})"
            select = simd.both(upper, match) if mode == "upper" else f"_mm256_andnot_si256({upper}, {match})"
        else:
            upper = f"(starts << {offset})"
            select = f"({upper} & ({match}))" if mode == "upper" else f"(~{upper} & ({match}))"
        o += [f"    {simd.m} match{idx} = {select};"]
        for back, d in enumerate(reversed(delta)):
            if not d:
                continue
            if simd.avx2:
                mask = f"match{idx}"
                for _ in range(back):
                    mask = f"sz_haswell_next_bytes_({mask})"
                o += [f"    result = _mm256_add_epi8(result, _mm256_and_si256({mask}, _mm256_set1_epi8((char){d})));"]
            else:
                o += [
                    f"    result = _mm512_mask_add_epi8(result, match{idx} >> {back}, result, _mm512_set1_epi8((char){d}));"
                ]
    o += [
        "    _mm256_storeu_si256((__m256i *)target, result);"
        if simd.avx2
        else "    _mm512_mask_storeu_epi8(target, sz_u64_mask_until_(length), result);",
        "    return length;",
        "}",
    ]
    return "\n".join(o)


def generate_simd(ucd):
    maps = {"lower": {}, "upper": {}}
    alnum = set()
    first = None
    for line in (ucd / "UnicodeData.txt").read_text().splitlines():
        f = line.split(";")
        cp = int(f[0], 16)
        if f[1].endswith(", First>"):
            first = cp
            continue
        lo = first if f[1].endswith(", Last>") else cp
        first = None
        if f[2].startswith("L") or f[2] == "Nd":
            alnum.update(range(lo, cp + 1))
        for mode, col in [("lower", 13), ("upper", 12)]:
            if f[col]:
                maps[mode][cp] = int(f[col], 16)
    dest = Path(__file__).resolve().parents[1] / "include/stringzilla/utf8_case"
    for isa in ("haswell", "icelake"):
        out = [
            "/* Generated Unicode 17 simple INITCAP transforms. See UNICODE-LICENSE.txt. */",
            f"#ifndef STRINGZILLA_UTF8_INITCAP_{isa.upper()}_MAPPINGS_H_",
            f"#define STRINGZILLA_UTF8_INITCAP_{isa.upper()}_MAPPINGS_H_",
        ]
        for family, prefixes in FAMILIES.items():
            out.append(simd_family(SIMD(isa), family, prefixes, maps, alnum))
        out += ["#endif", ""]
        (dest / f"initcap_{isa}_mappings.h").write_text("\n".join(out))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--ucd-dir", type=Path, required=True)
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    target = Path(__file__).resolve().parents[1] / "include/stringzilla/utf8_case/initcap_tables.h"
    target.write_text(generate(args.ucd_dir))
    generate_simd(args.ucd_dir)
    subprocess.run(
        [
            args.clang_format,
            "--Wno-error=unknown",
            "-i",
            str(target),
            str(target.with_name("initcap_haswell_mappings.h")),
            str(target.with_name("initcap_icelake_mappings.h")),
        ],
        check=True,
    )
