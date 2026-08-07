/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#ifndef BOXEDVN_TEST_H
#define BOXEDVN_TEST_H

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace boxedvn_test {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

std::vector<TestCase>& registry();
void reportFailure(const char* file, int line, const std::string& message);

extern int failureCount;
extern const char* currentTest;

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back(TestCase{name, std::move(body)});
    }
};

template <typename T>
std::string describe(const T& value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

inline std::string describe(bool value) { return value ? "true" : "false"; }

}  // namespace boxedvn_test

#define BOXEDVN_TEST(name)                                                  \
    static void name();                                                     \
    static ::boxedvn_test::Registrar registrar_##name(#name, name);         \
    static void name()

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            ::boxedvn_test::reportFailure(__FILE__, __LINE__,               \
                                          "CHECK(" #condition ") failed");  \
        }                                                                   \
    } while (false)

#define CHECK_EQ(actual, expected)                                          \
    do {                                                                    \
        const auto& boxedvn_actual = (actual);                              \
        const auto& boxedvn_expected = (expected);                          \
        if (!(boxedvn_actual == boxedvn_expected)) {                        \
            ::boxedvn_test::reportFailure(                                  \
                __FILE__, __LINE__,                                         \
                std::string("CHECK_EQ(" #actual ", " #expected ")\n"        \
                            "      actual:   ") +                           \
                    ::boxedvn_test::describe(boxedvn_actual) +              \
                    "\n      expected: " +                                  \
                    ::boxedvn_test::describe(boxedvn_expected));            \
        }                                                                   \
    } while (false)

// Asserts that `haystack` contains `needle`, so diagnostics can be tested for
// the information they must carry without pinning their exact wording.
#define CHECK_CONTAINS(haystack, needle)                                    \
    do {                                                                    \
        const std::string boxedvn_haystack = (haystack);                    \
        const std::string boxedvn_needle = (needle);                        \
        if (boxedvn_haystack.find(boxedvn_needle) == std::string::npos) {   \
            ::boxedvn_test::reportFailure(                                  \
                __FILE__, __LINE__,                                         \
                std::string("CHECK_CONTAINS failed\n      haystack: ") +    \
                    boxedvn_haystack + "\n      needle:   " +               \
                    boxedvn_needle);                                        \
        }                                                                   \
    } while (false)

#endif  // BOXEDVN_TEST_H
