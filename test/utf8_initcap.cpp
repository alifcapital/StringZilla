/** @brief INITCAP backend equivalence, UTF-8 validation, and word-boundary tests. */
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include "stringzilla/utf8_case.h"
#ifdef INITCAP_ICU_ORACLE
#include <unicode/uchar.h>
#include <unicode/utf8.h>
#endif

struct Backend {
    char const *name;
    sz_utf8_case_initcap_t convert;
};
std::vector<Backend> backends() {
    std::vector<Backend> result = {{"scalar", sz_utf8_case_initcap_serial}};
#if SZ_USE_HASWELL
    result.push_back({"AVX2", sz_utf8_case_initcap_haswell});
#endif
#if SZ_USE_ICELAKE
    result.push_back({"AVX512", sz_utf8_case_initcap_icelake});
#endif
    return result;
}
size_t comparisons = 0;
void check(std::string const &input, std::string const &expected, size_t expected_error = SZ_SIZE_MAX) {
    for (auto backend : backends()) {
        std::string output(input.size() * 3 + 64, '\x55');
        size_t error = 0;
        size_t length = backend.convert(input.data(), input.size(), output.data(), &error);
        if (error != expected_error ||
            (error != SZ_SIZE_MAX ? length != SZ_SIZE_MAX
                                  : length != expected.size() || output.substr(0, length) != expected)) {
            std::cerr << "FAIL " << backend.name << " bytes=" << input.size() << " error=" << error
                      << " expected_error=" << expected_error << '\n';
            std::cerr << "input:";
            for (unsigned char c : input.substr(0, 32)) std::cerr << " " << std::hex << (unsigned)c;
            std::cerr << "\nexpected:";
            for (unsigned char c : expected.substr(0, 32)) std::cerr << " " << std::hex << (unsigned)c;
            std::cerr << "\nactual:";
            for (unsigned char c : output.substr(0, std::min(length, size_t {32})))
                std::cerr << " " << std::hex << (unsigned)c;
            std::cerr << std::dec << '\n';
            std::exit(1);
        }
        for (size_t i = input.size() * 3; i != output.size(); ++i)
            if (output[i] != '\x55') std::abort();
        ++comparisons;
    }
}
std::string scalar(std::string const &s) {
    std::string result(s.size() * 3, '\0');
    size_t size = sz_utf8_case_initcap_serial(s.data(), s.size(), result.data(), nullptr);
    if (size == SZ_SIZE_MAX) std::abort();
    result.resize(size);
    return result;
}
int main(int argc, char **argv) {
    struct Example {
        char const *source, *expected;
    };
    Example examples[] = {{"", ""},
                          {"hELLO_world 123ABC", "Hello_World 123abc"},
                          {"ßETA ẞETA", "ßeta ẞeta"},
                          {"\uA7D3ABC", "\uA7D2abc"},
                          {"aİ İSTANBUL", "Ai İstanbul"},
                          {"ΟΣ ΟΣΑ", "Οσ Οσα"},
                          {"привет МИР", "Привет Мир"},
                          {"e\u0301COLE", "E\u0301Cole"},
                          {"\u2160ABC", "\u2160Abc"},
                          {"\U00016EBB\U00016EA0", "\U00016EA0\U00016EBB"}};
    for (auto e : examples) check(e.source, e.expected);
    // Every byte alignment across both SIMD widths, with and without a preceding word.
    for (size_t prefix = 0; prefix < 130; ++prefix) {
        for (auto e : examples) {
            for (char preceding : {' ', 'A', '1', '_'}) {
                std::string input(prefix, preceding);
                for (int repeat = 0; repeat < 8; ++repeat) {
                    input += e.source;
                    input += "_A";
                }
                check(input, scalar(input));
            }
        }
        for (std::string bad : {std::string("\xFF", 1), std::string("\xC2", 1), std::string("\xE0\x80\x80", 3),
                                std::string("\xED\xA0\x80", 3), std::string("\xF4\x90\x80\x80", 4)}) {
            check(std::string(prefix, 'a') + bad + std::string(65, 'b'), "", prefix);
            check(std::string(prefix, 'a') + bad, "", prefix);
        }
    }
#ifdef INITCAP_ICU_ORACLE
    // ICU provides an independent oracle for code points assigned in its Unicode version.
    std::string input, expected;
    auto flush = [&] {
        if (!input.empty()) {
            check(input, expected);
            input.clear();
            expected.clear();
        }
    };
    for (UChar32 cp = 0; cp <= 0x10FFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        if (!u_isdefined(cp)) continue;
        // New Unicode 17 partners are not available in an older ICU oracle.
        if (!u_isdefined(sz_unicode_simple_case_(cp, sz_true_k)) ||
            !u_isdefined(sz_unicode_simple_case_(cp, sz_false_k)))
            continue;
        auto append = [](std::string &s, UChar32 c) {
            char bytes[4];
            int32_t n = 0;
            UBool error = false;
            U8_APPEND(bytes, n, 4, c, error);
            if (error) std::abort();
            s.append(bytes, n);
        };
        append(input, cp);
        input += "A A";
        append(input, cp);
        input += "A ";
        bool word = u_isalnum(cp);
        append(expected, word ? u_toupper(cp) : cp);
        expected += word ? "a A" : "A A";
        append(expected, word ? u_tolower(cp) : cp);
        expected += word ? "a " : "A ";
        flush();
    }
    flush();
#endif
    if (argc == 2) {
        std::ifstream corpus(argv[1], std::ios::binary);
        if (!corpus) return 2;
        uint32_t sizes[2];
        while (corpus.read(reinterpret_cast<char *>(sizes), sizeof(sizes))) {
            std::string input(sizes[0], '\0'), expected(sizes[1], '\0');
            if (!corpus.read(input.data(), input.size()) || !corpus.read(expected.data(), expected.size())) return 2;
            check(input, expected);
        }
    }
    std::mt19937_64 rng(42);
    std::vector<std::string> atoms = {"a", "Z", "8", "_",      " ",      "é",  "ß",  "İ",
                                      "Ё", "Я", "Σ", "\u0301", "\u2160", "中", "😀", "\0"};
    for (int row = 0; row < 10000; ++row) {
        std::string input;
        for (int i = 0, n = 1 + rng() % 200; i < n; ++i) input += atoms[rng() % atoms.size()];
        check(input, scalar(input));
    }
    std::cout << "INITCAP PASS comparisons=" << comparisons << '\n';
}
