/** @brief INITCAP throughput by SIMD backend on 4 KiB strings. */
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include "stringzilla/utf8_case.h"
int main() {
    struct Backend {
        char const *name;
        sz_utf8_case_initcap_t convert;
    };
    std::vector<Backend> backends = {{"scalar", sz_utf8_case_initcap_serial}};
#if SZ_USE_HASWELL
    backends.push_back({"AVX2", sz_utf8_case_initcap_haswell});
#endif
#if SZ_USE_ICELAKE
    backends.push_back({"AVX512", sz_utf8_case_initcap_icelake});
#endif
    struct Corpus {
        char const *name, *text;
    };
    Corpus corpora[] = {{"ASCII", "HELLO_world 123ABC example! "},
                        {"Latin", "ÉCOLE Größe İSTANBUL café "},
                        {"Cyrillic", "ПРИВЕТ_МИР это СТРОКА 123тест "},
                        {"Greek", "ΑΛΦΑ ΒΗΤΑ ΟΣΑ κόσμος "},
                        {"Mixed", "HELLO привет ÉCOLE 世界 😀 "}};
    std::cout << "corpus,backend,bytes_per_second\n";
    for (auto corpus : corpora) {
        std::string input;
        while (input.size() < 4096) input += corpus.text;
        std::string output(input.size() * 3, '\0');
        for (auto backend : backends) {
            double best = 1e100;
            size_t checksum = 0;
            for (int round = 0; round < 3; ++round) {
                auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < 10000; ++i) {
                    checksum += backend.convert(input.data(), input.size(), output.data(), nullptr);
                    asm volatile("" : : "r"(output.data()) : "memory");
                }
                auto end = std::chrono::steady_clock::now();
                best = std::min(best, std::chrono::duration<double>(end - start).count());
            }
            if (checksum != input.size() * 30000) return 1;
            std::cout << corpus.name << ',' << backend.name << ',' << input.size() * 10000 / best << '\n';
        }
    }
}
