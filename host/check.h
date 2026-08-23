#pragma once

// A ~50-line test harness, deliberately dependency-free.
//
// The repo's tooling is stdlib-only by policy (scripts/ is plain python3, nothing is vendored that a
// fetch script can reproduce), and the tests are the same shape: assertions over pure functions with
// deterministic inputs. That does not need a framework, and vendoring one would be the only third-
// party C++ in the tree.
//
// Usage:
//     TEST(name_of_the_case) { CHECK(cond); CHECK_EQ(a, b); }
//     int main() { return daisyapps::test::run_all(); }
//
// A failing CHECK prints file:line, the expression, and (for CHECK_EQ) both values, then marks the
// case failed and RETURNS from it - so one broken case does not hide the rest of the file.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace daisyapps::test {

struct Case {
    const char* name;
    void (*fn)(bool&);
};

inline std::vector<Case>& registry()
{
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)(bool&)) { registry().push_back({name, fn}); }
};

// Value printing for CHECK_EQ. Overloads rather than iostreams, so the harness pulls in <cstdio>
// and nothing else; anything without an overload falls back to "?" and the expression text.
inline void print_val(long long v)         { std::printf("%lld", v); }
inline void print_val(unsigned long long v){ std::printf("%llu", v); }
inline void print_val(int v)               { std::printf("%d", v); }
inline void print_val(unsigned v)          { std::printf("%u", v); }
inline void print_val(long v)              { std::printf("%ld", v); }
inline void print_val(unsigned long v)     { std::printf("%lu", v); }
inline void print_val(bool v)              { std::printf("%s", v ? "true" : "false"); }
inline void print_val(double v)            { std::printf("%.9g", v); }
inline void print_val(float v)             { std::printf("%.9g", static_cast<double>(v)); }
inline void print_val(const char* v)       { std::printf("\"%s\"", v ? v : "(null)"); }
inline void print_val(const std::string& v){ std::printf("\"%s\"", v.c_str()); }
template <class T> void print_val(const T&) { std::printf("?"); }

inline int run_all()
{
    int failed = 0;
    for (const Case& c : registry()) {
        bool ok = true;
        c.fn(ok);
        if (!ok) { ++failed; std::printf("FAIL  %s\n", c.name); }
        else                 std::printf("ok    %s\n", c.name);
    }
    std::printf("\n%zu cases, %d failed\n", registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

} // namespace daisyapps::test

#define TEST(name)                                                                   \
    static void name(bool& _spk_ok);                                                 \
    static ::daisyapps::test::Registrar _spk_reg_##name(#name, &name);               \
    static void name([[maybe_unused]] bool& _spk_ok)

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            _spk_ok = false;                                                         \
            return;                                                                  \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto _a = (a);                                                         \
        const auto _b = (b);                                                         \
        if (!(_a == _b)) {                                                           \
            std::printf("  %s:%d: CHECK_EQ failed: %s == %s\n    left:  ",           \
                        __FILE__, __LINE__, #a, #b);                                 \
            ::daisyapps::test::print_val(_a);                                        \
            std::printf("\n    right: ");                                            \
            ::daisyapps::test::print_val(_b);                                        \
            std::printf("\n");                                                       \
            _spk_ok = false;                                                         \
            return;                                                                  \
        }                                                                            \
    } while (0)

// Floating-point compare with an explicit tolerance. There is no default: a test that does not know
// its own tolerance is not asserting anything.
#define CHECK_NEAR(a, b, tol)                                                        \
    do {                                                                             \
        const double _a = (a), _b = (b), _t = (tol);                                 \
        if (!(((_a - _b) < _t) && ((_b - _a) < _t))) {                               \
            std::printf("  %s:%d: CHECK_NEAR failed: |%s - %s| > %g\n"               \
                        "    left: %.9g  right: %.9g\n",                             \
                        __FILE__, __LINE__, #a, #b, _t, _a, _b);                     \
            _spk_ok = false;                                                         \
            return;                                                                  \
        }                                                                            \
    } while (0)
