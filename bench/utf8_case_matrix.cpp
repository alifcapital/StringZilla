/** @brief Benchmarks fold/lower/upper directly on each enabled backend with identical inputs. */
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include "stringzilla/utf8_case.h"
#include "stringzilla/utf8_uncased_fold.h"
using converter_t = sz_size_t (*)(sz_cptr_t, sz_size_t, sz_ptr_t);
struct backend_t {
    char const *name;
    converter_t fn;
    converter_t reference;
};
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: case-matrix <datasets.bin>\n";
        return 2;
    }
    std::vector<backend_t> backends = {
        {"fold/scalar", sz_utf8_uncased_fold_serial, sz_utf8_uncased_fold_serial},
        {"lower/scalar", sz_utf8_case_lower_serial, sz_utf8_case_lower_serial},
        {"upper/scalar", sz_utf8_case_upper_serial, sz_utf8_case_upper_serial},
#if SZ_USE_HASWELL
        {"fold/AVX2", sz_utf8_uncased_fold_haswell, sz_utf8_uncased_fold_serial},
        {"lower/AVX2", sz_utf8_case_lower_haswell, sz_utf8_case_lower_serial},
        {"upper/AVX2", sz_utf8_case_upper_haswell, sz_utf8_case_upper_serial},
#endif
#if SZ_USE_ICELAKE
        {"fold/AVX512", sz_utf8_uncased_fold_icelake, sz_utf8_uncased_fold_serial},
        {"lower/AVX512", sz_utf8_case_lower_icelake, sz_utf8_case_lower_serial},
        {"upper/AVX512", sz_utf8_case_upper_icelake, sz_utf8_case_upper_serial},
#endif
    };
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) return 2;
    std::mt19937 rng(17);
    std::cout << "dataset,bytes,backend,median_MBps,min_MBps,max_MBps\n" << std::flush;
    for (;;) {
        std::uint32_t name_size, input_size;
        f.read(reinterpret_cast<char *>(&name_size), 4);
        if (f.eof() && f.gcount() == 0) break;
        f.read(reinterpret_cast<char *>(&input_size), 4);
        if (!f || name_size > 1000 || input_size > 100000000) return 3;
        std::string name(name_size, '\0'), input(input_size, '\0');
        f.read(name.data(), name.size());
        f.read(input.data(), input.size());
        if (!f) return 3;
        std::vector<char> target(input.size() * 3 + 64), expected(target.size());
        std::vector<std::vector<double>> rates(backends.size());
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < backends.size(); ++i) {
            order.push_back(i);
            auto const &b = backends[i];
            auto n = b.fn(input.data(), input.size(), target.data());
            auto m = b.reference(input.data(), input.size(), expected.data());
            if (n != m || !std::equal(target.begin(), target.begin() + n, expected.begin())) {
                std::cerr << "Wrong result: " << name << " " << b.name << '\n';
                return 1;
            }
        }
        for (int trial = 0; trial != 7; ++trial) {
            std::shuffle(order.begin(), order.end(), rng);
            for (auto i : order) {
                auto fn = backends[i].fn;
                std::size_t batch = std::max<std::size_t>(1, 65536 / input.size()), calls = 0;
                auto start = std::chrono::steady_clock::now();
                double elapsed;
                do {
                    for (std::size_t j = 0; j < batch; ++j) {
                        auto size = fn(input.data(), input.size(), target.data());
#if defined(__GNUC__) || defined(__clang__)
                        asm volatile("" : : "r"(target.data()), "r"(size) : "memory");
#else
                        volatile auto result_size = size;
                        (void)result_size;
#endif
                    }
                    calls += batch;
                    elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                } while (elapsed < 0.035);
                rates[i].push_back(static_cast<double>(input.size()) * calls / elapsed / 1e6);
            }
        }
        for (std::size_t i = 0; i < backends.size(); ++i) {
            auto &r = rates[i];
            std::sort(r.begin(), r.end());
            std::cout << name << ',' << input.size() << ',' << backends[i].name << ',' << std::fixed
                      << std::setprecision(1) << r[3] << ',' << r.front() << ',' << r.back() << '\n';
        }
        std::cout << std::flush;
    }
}
