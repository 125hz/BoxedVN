/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  A dependency-free test harness.  BoxedVN's tests must run on a clean CI
 *  runner with nothing installed beyond Xcode and CMake, so no external test
 *  framework is used.
 */

#include "boxedvn_test.h"

#include <cstdio>
#include <string>
#include <vector>

namespace boxedvn_test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

int failureCount = 0;
const char* currentTest = "";

void reportFailure(const char* file, int line, const std::string& message) {
    std::fprintf(stderr, "  FAIL %s\n    %s:%d\n    %s\n", currentTest, file,
                 line, message.c_str());
    failureCount++;
}

}  // namespace boxedvn_test

int main(int argc, char** argv) {
    using namespace boxedvn_test;

    const std::string filter = (argc > 1) ? argv[1] : std::string();

    int run = 0;
    int failedTests = 0;
    for (const TestCase& testCase : registry()) {
        const std::string name = testCase.name;
        if (!filter.empty() && name.find(filter) == std::string::npos) {
            continue;
        }
        currentTest = testCase.name;
        const int before = failureCount;
        run++;
        testCase.body();
        if (failureCount != before) {
            failedTests++;
        } else {
            std::printf("  ok   %s\n", testCase.name);
        }
    }

    std::printf("\n%d test(s) run, %d failed, %d assertion failure(s)\n", run,
                failedTests, failureCount);
    return failedTests == 0 ? 0 : 1;
}
