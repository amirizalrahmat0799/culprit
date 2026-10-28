#pragma once
// A tiny test framework: TEST("name") { CHECK(...); CHECK_EQ(a, b); }

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace check {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> all;
    return all;
}
inline int& failures() {
    static int n = 0;
    return n;
}
struct Register {
    Register(const char* name, std::function<void()> fn) { cases().push_back({name, std::move(fn)}); }
};

template <typename A, typename B>
void equal(const A& a, const B& b, const char* expr, const char* file, int line) {
    if (!(a == b)) {
        ++failures();
        std::cerr << file << ":" << line << ": CHECK_EQ(" << expr << ")\n  left:  " << a << "\n  right: " << b
                  << "\n";
    }
}

}  // namespace check

#define CHECK_CONCAT2(a, b) a##b
#define CHECK_CONCAT(a, b) CHECK_CONCAT2(a, b)
#define TEST(name)                                                                       \
    static void CHECK_CONCAT(test_fn_, __LINE__)();                                      \
    static check::Register CHECK_CONCAT(test_reg_, __LINE__)(name, CHECK_CONCAT(test_fn_, __LINE__)); \
    static void CHECK_CONCAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ++check::failures();                                                     \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed\n"; \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b) check::equal((a), (b), #a ", " #b, __FILE__, __LINE__)
