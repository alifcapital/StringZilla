# Unicode case conversion

`sz_utf8_case_lower` and `sz_utf8_case_upper` implement Unicode 17 full default case conversion.
They follow CPython 3.15 `str.lower()` and `str.upper()`, without locale tailoring or normalization.
Case folding remains a separate operation, `sz_utf8_uncased_fold`.

| Input | Lower | Upper | Fold |
| --- | --- | --- | --- |
| Straße | straße | STRASSE | strasse |
| ẞ | ß | ẞ | ss |
| ΟΣ | ος | ΟΣ | οσ |
| İ | i + U+0307 | İ | i + U+0307 |

The caller supplies an output buffer of at least three times the input byte length.
The return value is the output byte length. Input and output must not overlap.
Invalid UTF-8 bytes are preserved, and each invalid byte breaks the casing context.
Valid sequences around them are converted normally.

The serial implementation uses generated Unicode mappings. The AVX2 and AVX-512 implementations
use the folding kernels' byte-family approach: transform ASCII and supported UTF-8 families in
vectors, then process expansions and contextual mappings with the serial mapper. Each vector
handler stops before a foreign or incomplete sequence. Final sigma inspects the original input
across vector boundaries. Other architectures use the serial implementation.

Tables and byte transforms are generated from the official Unicode 17 `UnicodeData.txt`,
`SpecialCasing.txt` and `DerivedCoreProperties.txt`. Their SHA256 values are recorded in `tables.h`.
The Unicode data license is in `UNICODE-LICENSE.txt`.

```sh
python3 tools/generate_utf8_case.py --ucd-dir /path/to/ucd-17.0.0
```

The normal CMake test target `stringzilla_test_utf8_case_cpp17` runs literal regression cases.
To check every Unicode scalar value, context rules, SIMD boundaries, mixed scripts and invalid
input against an independent oracle, generate a corpus using **CPython with Unicode 17.0.0**:

```sh
python3.15 test/utf8_case_python.py /tmp/case-python17.bin
build_release/stringzilla_test_utf8_case_cpp17 /tmp/case-python17.bin
```

The test also checks destination canaries and inaccessible input/output pages on Unix systems.
Compile with AddressSanitizer and UndefinedBehaviorSanitizer for additional checks. ISA-specific
entry points require the corresponding CPU support, as do the other native StringZilla tests.

`bench/utf8_uncased.cpp` benchmarks fold, lower and upper against their own serial references.
`bench/utf8_case_matrix.cpp` measures the same nine x86 combinations with randomized backend order
and seven trials, reporting median, minimum and maximum input MB/s. Its binary input consists of
repeated records: two little-endian uint32 lengths followed by dataset-name bytes and input bytes.
