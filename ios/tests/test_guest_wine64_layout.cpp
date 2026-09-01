/*
 * BoxedVN - Wine64 layout and bounded module-search diagnostics tests.
 * GPLv2; see license.txt.
 */

#include "boxedvn_test.h"

#include "dll_search_trace.h"
#include "guest_wine64_layout.h"

#include <algorithm>
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

BOXEDVN_TEST(wine64_layout_puts_the_bridge_x11_libraries_first_on_the_library_path) {
    // WINEDLLPATH names Wine module roots and cannot redirect an ELF
    // dependency; LD_LIBRARY_PATH is what the guest's ld-linux consults first.
    CHECK_EQ(std::string(K_X64_GUEST_X11_LIB_DIR),
             std::string("/usr/lib/boxedwine64-x11"));
    CHECK_EQ(boxedvn::guestLibraryPathAssignment(),
             std::string("LD_LIBRARY_PATH=/usr/lib/boxedwine64-x11"));
    CHECK(!boxedvn::environmentSetsLibraryPath({"WINEPREFIX=/tmp/prefix"}));
    CHECK(boxedvn::environmentSetsLibraryPath(
        {"WINEPREFIX=/tmp/prefix", "LD_LIBRARY_PATH=/opt/x11"}));
    // The directory is dedicated to the shim: it is not the multiarch
    // directory the distro libraries live in, so those stay reachable.
    CHECK(std::string(K_X64_GUEST_X11_LIB_DIR).find("x86_64-linux-gnu") ==
          std::string::npos);
}

BOXEDVN_TEST(wine64_layout_projects_the_dxmt_modules_over_the_module_root) {
    const std::vector<std::string> names = boxedvn::x64DxmtModuleNames();
    CHECK_EQ(names.size(), (size_t)K_X64_DXMT_MODULE_COUNT);
    // Wine's own d3d11, dxgi and d3d10core must be replaced, and winemetal
    // has to sit beside them to bind to the packaged winemetal.so.
    for (const char* expected : {"d3d11.dll", "dxgi.dll", "d3d10core.dll", "winemetal.dll"}) {
        CHECK(std::find(names.begin(), names.end(), std::string(expected)) != names.end());
    }
    // Only a real file replaces a builtin; a missing or directory source
    // leaves Wine's module in place.
    CHECK(boxedvn::shouldOverlayX64WineModule(true, false));
    CHECK(!boxedvn::shouldOverlayX64WineModule(false, false));
    CHECK(!boxedvn::shouldOverlayX64WineModule(true, true));
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
