/** @brief Checks all case backends against externally generated CPython Unicode 17 vectors. */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "stringzilla/utf8_case.h"
#include "stringzilla/utf8_uncased_fold.h"

using converter_t = sz_size_t (*)(sz_cptr_t, sz_size_t, sz_ptr_t);
struct backend_t {
    char const *name;
    converter_t convert;
    unsigned int operation;
};
std::vector<backend_t> backends() {
    std::vector<backend_t> result = {{"lower/serial", sz_utf8_case_lower_serial, 0},
                                     {"upper/serial", sz_utf8_case_upper_serial, 1},
                                     {"fold/serial", sz_utf8_uncased_fold_serial, 2}};
#if SZ_USE_HASWELL
    result.insert(result.end(), {{"lower/AVX2", sz_utf8_case_lower_haswell, 0},
                                 {"upper/AVX2", sz_utf8_case_upper_haswell, 1},
                                 {"fold/AVX2", sz_utf8_uncased_fold_haswell, 2}});
#endif
#if SZ_USE_ICELAKE
    result.insert(result.end(), {{"lower/AVX512", sz_utf8_case_lower_icelake, 0},
                                 {"upper/AVX512", sz_utf8_case_upper_icelake, 1},
                                 {"fold/AVX512", sz_utf8_uncased_fold_icelake, 2}});
#endif
    return result;
}
int smoke() {
    struct example_t {
        char const *source, *lower, *upper, *fold;
    };
    example_t const examples[] = {{"", "", "", ""},
                                  {"Straße ẞ", "straße ß", "STRASSE ẞ", "strasse ss"},
                                  {"ΟΣ", "ος", "ΟΣ", "οσ"},
                                  {"ΟΣΑ", "οσα", "ΟΣΑ", "οσα"},
                                  {"AΣ\u0301", "aς\u0301", "AΣ\u0301", "aσ\u0301"},
                                  {"İIı", "i\u0307iı", "İII", "i\u0307iı"},
                                  {"ỹἀ", "ỹἀ", "ỸἈ", "ỹἀ"},
                                  {"ΆΊΌ", "άίό", "ΆΊΌ", "άίό"},
                                  {"և", "և", "ԵՒ", "եւ"},
                                  {"Აა", "აა", "ᲐᲐ", "აა"},
                                  {"\U00016EA0", "\U00016EBB", "\U00016EA0", "\U00016EBB"}};
    for (auto const &example : examples) {
        for (int repeat : {1, 16, 65}) {
            std::string source;
            std::array<std::string, 3> expected;
            for (int i = 0; i < repeat; ++i) {
                source += example.source;
                source += ' ';
                expected[0] += example.lower;
                expected[0] += ' ';
                expected[1] += example.upper;
                expected[1] += ' ';
                expected[2] += example.fold;
                expected[2] += ' ';
            }
            for (auto const &backend : backends()) {
                std::string output(source.size() * 3, '\0');
                output.resize(backend.convert(source.data(), source.size(), output.data()));
                if (output != expected[backend.operation]) {
                    std::cerr << "FAIL smoke " << backend.name << " input=" << example.source << '\n';
                    return 1;
                }
            }
        }
    }
    std::cout << "Unicode 17 case smoke: PASS\n";
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 1) return smoke();
    if (argc != 2) {
        std::cerr << "Usage: utf8_case_test <CPython reference corpus>\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 2;
    auto implementations = backends();
    std::size_t cases = 0, checks = 0, failures = 0;
    std::vector<std::size_t> failed(implementations.size(), 0);
    std::array<std::string, 4> values;
    std::vector<char> buffer;
    for (;;) {
        std::array<std::uint32_t, 4> sizes;
        input.read(reinterpret_cast<char *>(sizes.data()), sizeof(sizes));
        if (input.gcount() == 0 && input.eof()) break;
        if (!input) return 3;
        for (std::size_t i = 0; i != 4; ++i) {
            values[i].resize(sizes[i]);
            input.read(values[i].data(), sizes[i]);
            if (!input) return 3;
        }
        auto const &source = values[0];
        std::size_t offset = cases % 64;
        buffer.resize(offset + source.size() * 3 + 64);
        for (std::size_t b = 0; b != implementations.size(); ++b) {
            auto const &backend = implementations[b];
            std::fill(buffer.begin(), buffer.end(), '\x5A');
            char *target = buffer.data() + offset;
            std::size_t length = backend.convert(source.data(), source.size(), target);
            auto const &expected = values[backend.operation + 1];
            bool equal = length == expected.size() && std::memcmp(target, expected.data(), expected.size()) == 0;
            bool bounds = std::all_of(buffer.begin(), buffer.begin() + offset, [](char c) { return c == '\x5A'; }) &&
                          std::all_of(buffer.begin() + offset + source.size() * 3, buffer.end(),
                                      [](char c) { return c == '\x5A'; });
            if (!equal || !bounds) {
                ++failures;
                ++failed[b];
                if (failed[b] <= 3) {
                    std::cerr << "FAIL " << backend.name << " case=" << cases << " input=" << source.size()
                              << " actual=" << length << " expected=" << expected.size() << " bounds=" << bounds
                              << '\n';
                    std::ofstream dump(
                        std::string("failure-") + std::to_string(b) + "-" + std::to_string(failed[b]) + ".bin",
                        std::ios::binary);
                    dump.write(source.data(), source.size());
                }
            }
            ++checks;
        }
        ++cases;
    }
#if defined(__unix__) || defined(__APPLE__)
    // Put both input and the exact 3x destination capacity against inaccessible pages.
    std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto *src = static_cast<char *>(
        mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    auto *dst = static_cast<char *>(
        mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (src == MAP_FAILED || dst == MAP_FAILED) return 4;
    if (mprotect(src + page, page, PROT_NONE) || mprotect(dst + page, page, PROT_NONE)) return 4;
    std::string const text = "AΣ\u0301 Straße ỹἀ Ά ΐև ІЇЁя Აა 中文 😀";
    for (std::size_t n = 0; n < 194; ++n) {
        for (std::size_t i = 0; i != n; ++i) src[page - n + i] = text[i % text.size()];
        for (auto const &backend : implementations) backend.convert(src + page - n, n, dst + page - n * 3);
    }
    munmap(src, page * 2);
    munmap(dst, page * 2);
    std::cout << "guard-pages=PASS\n";
#endif
    for (std::size_t i = 0; i != implementations.size(); ++i)
        std::cout << implementations[i].name << " failures=" << failed[i] << '\n';
    std::cout << "cases=" << cases << " checks=" << checks << " failures=" << failures << "\n";
    return failures ? 1 : 0;
}
