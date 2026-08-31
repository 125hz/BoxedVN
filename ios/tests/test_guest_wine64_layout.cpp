/*
 * BoxedVN - Wine64 layout and bounded module-search diagnostics tests.
 * GPLv2; see license.txt.
 */

#include "boxedvn_test.h"

#include "dll_search_trace.h"
#include "guest_wine64_layout.h"

#include <string>
#include <vector>

BOXEDVN_TEST(wine64_layout_keeps_loader_and_modules_under_one_root) {
    CHECK_EQ(std::string(K_X64_WINE_MODULE_ROOT),
             std::string("/usr/lib/x86_64-linux-gnu/wine"));
    CHECK_EQ(std::string(K_X64_WINE_LOADER),
             std::string(K_X64_WINE_MODULE_ROOT) + "/wine64");
    CHECK_EQ(std::string(K_X64_WINE_PE_DIR),
             std::string(K_X64_WINE_MODULE_ROOT) + "/x86_64-windows");
    CHECK_EQ(std::string(K_X64_WINE_BUILTIN_PROBE),
             std::string(K_X64_WINE_PE_DIR) + "/kernel32.dll");
    CHECK_EQ(std::string(K_X64_WINE_DERIVED_DATA_ROOT),
             std::string("/usr/lib/share/wine"));
    CHECK_EQ(std::string(K_X64_WINE_DATA_ROOT),
             std::string("/usr/share/wine"));
}

BOXEDVN_TEST(wine64_layout_supplies_only_a_missing_dll_path) {
    CHECK(!boxedvn::environmentSetsWineDllPath({"WINEPREFIX=/tmp/prefix"}));
    CHECK(boxedvn::environmentSetsWineDllPath(
        {"WINEPREFIX=/tmp/prefix", "WINEDLLPATH=/opt/wine"}));
    CHECK_EQ(boxedvn::wineDllPathAssignment(),
             std::string("WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine"));
}

BOXEDVN_TEST(wine64_preflight_accepts_only_a_complete_mz_signature) {
    const unsigned char valid[] = {'M', 'Z'};
    const unsigned char invalid[] = {'M', 'X'};
    CHECK(boxedvn::looksLikePeImage(valid, sizeof(valid)));
    CHECK(!boxedvn::looksLikePeImage(valid, 1));
    CHECK(!boxedvn::looksLikePeImage(invalid, sizeof(invalid)));
    CHECK(!boxedvn::looksLikePeImage(nullptr, 2));
}

BOXEDVN_TEST(dll_search_trace_ignores_unrelated_paths_and_is_opt_in) {
    boxedvn::setDllSearchTraceEnabled(false);
    boxedvn::DllSearchTrace trace;
    CHECK(trace.record("/usr/lib/x86_64-linux-gnu/wine/kernel32.dll") ==
          boxedvn::DllSearchTrace::Decision::Silent);

    boxedvn::setDllSearchTraceEnabled(true);
    CHECK(trace.record("/home/username/.wine64/user.reg") ==
          boxedvn::DllSearchTrace::Decision::Silent);
    CHECK(trace.record("/usr/lib/x86_64-linux-gnu/wine/kernel32.dll") ==
          boxedvn::DllSearchTrace::Decision::Report);
    boxedvn::setDllSearchTraceEnabled(false);
}

BOXEDVN_TEST(dll_search_trace_has_a_hard_per_process_budget) {
    boxedvn::setDllSearchTraceEnabled(true);
    boxedvn::DllSearchTrace first;
    boxedvn::DllSearchTrace second;

    for (unsigned i = 1; i < K_DLL_SEARCH_TRACE_BUDGET; ++i) {
        CHECK(first.record("/usr/lib/wine/kernel32.dll") ==
              boxedvn::DllSearchTrace::Decision::Report);
    }
    CHECK(first.record("/usr/lib/wine/kernel32.dll") ==
          boxedvn::DllSearchTrace::Decision::Exhausted);
    CHECK(first.record("/usr/lib/wine/kernel32.dll") ==
          boxedvn::DllSearchTrace::Decision::Silent);

    CHECK(second.record("/usr/lib/wine/kernel32.dll") ==
          boxedvn::DllSearchTrace::Decision::Report);
    CHECK(K_DLL_SEARCH_TRACE_BUDGET <= 128);
    boxedvn::setDllSearchTraceEnabled(false);
}
