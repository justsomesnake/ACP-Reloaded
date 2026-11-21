#pragma once
// Minimal test framework (run via CTest).
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

namespace acptest {
inline int g_failures = 0;
inline std::vector<std::pair<const char*, std::function<void()>>>& Registry() {
    static std::vector<std::pair<const char*, std::function<void()>>> r;
    return r;
}
struct Register {
    Register(const char* name, std::function<void()> fn) { Registry().emplace_back(name, std::move(fn)); }
};
}  // namespace acptest

#define TEST(name)                                           \
    static void name();                                      \
    static const acptest::Register reg_##name{#name, name};  \
    static void name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++acptest::g_failures;                                              \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)
