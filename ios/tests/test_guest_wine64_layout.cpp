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
    // The lane launches through the name upstream's WoW64 layout uses; the
    // packaged wine64 and its preloader are aliased under it at startup.
    CHECK_EQ(std::string(K_X64_WINE_LOADER),
             std::string(K_X64_WINE_MODULE_ROOT) + "/wine");
    CHECK_EQ(std::string(K_X64_WINE_LOADER64),
             std::string(K_X64_WINE_MODULE_ROOT) + "/wine64");
    CHECK_EQ(std::string(K_X64_WINE_PRELOADER),
             std::string(K_X64_WINE_MODULE_ROOT) + "/wine-preloader");
    CHECK_EQ(std::string(K_X64_WINE_LOADER64_NAME), std::string("wine64"));
    CHECK_EQ(std::string(K_X64_WINE_PRELOADER64_NAME), std::string("wine64-preloader"));
    CHECK_EQ(std::string(K_X64_WINE_PE_DIR),
             std::string(K_X64_WINE_MODULE_ROOT) + "/x86_64-windows");
    CHECK_EQ(std::string(K_X64_WINE_BUILTIN_PROBE),
             std::string(K_X64_WINE_PE_DIR) + "/kernel32.dll");
    CHECK_EQ(std::string(K_X64_WINE_DERIVED_DATA_ROOT),
             std::string("/usr/lib/share/wine"));
    CHECK_EQ(std::string(K_X64_WINE_DATA_ROOT),
             std::string("/usr/share/wine"));
}

BOXEDVN_TEST(wine64_layout_disables_the_vex_hwcaps_glibc_would_otherwise_use) {
    // Only the launched process runs on FEX; every forked child runs on the
    // SSE2-only interpreter. glibc picks its string routines once, per
    // process, so the tunables have to name every feature that would select a
    // VEX-encoded implementation -- and XSAVE/XSAVEC as well, because ld.so
    // picks its lazy-binding trampoline from those and the interpreter has
    // FXSAVE/FXRSTOR but no XSAVE/XSAVEC/XRSTOR.
    const std::string hwcaps = K_X64_GUEST_GLIBC_HWCAPS;
    for (const char* feature : {"-AVX", "-AVX2", "-AVX_Fast_Unaligned_Load",
                                "-FMA", "-FMA4", "-XSAVE", "-XSAVEC"}) {
        CHECK(hwcaps.find(feature) != std::string::npos);
    }
    // Every name is negated: an un-negated one would ENABLE the feature.
    CHECK(hwcaps.find('+') == std::string::npos);
    CHECK(hwcaps[0] == '-');
    for (std::string::size_type at = hwcaps.find(','); at != std::string::npos;
         at = hwcaps.find(',', at + 1)) {
        CHECK(hwcaps[at + 1] == '-');
    }
    // glibc 2.39 (the Ubuntu 24.04 rootfs) has no F16C tunable; naming one
    // would be silently ignored and would read as protection that is not there.
    CHECK(hwcaps.find("F16C") == std::string::npos);
    CHECK_EQ(std::string(K_X64_GUEST_GLIBC_HWCAPS_TUNABLE),
             std::string("glibc.cpu.hwcaps=") + hwcaps);
}

BOXEDVN_TEST(wine64_layout_appends_its_hwcaps_to_a_callers_tunables) {
    // Nothing set: the launch supplies the list on its own.
    CHECK_EQ(boxedvn::guestGlibcTunablesValue(""),
             std::string(K_X64_GUEST_GLIBC_HWCAPS_TUNABLE));
    CHECK_EQ(boxedvn::guestGlibcTunablesAssignment(""),
             std::string("GLIBC_TUNABLES=") + K_X64_GUEST_GLIBC_HWCAPS_TUNABLE);

    // A caller tuning something else keeps it and gains ours; glibc separates
    // tunables with ':'.
    CHECK_EQ(boxedvn::guestGlibcTunablesValue("glibc.malloc.arena_max=1"),
             std::string("glibc.malloc.arena_max=1:") +
                 K_X64_GUEST_GLIBC_HWCAPS_TUNABLE);

    // A caller that already chose an hwcaps list keeps it untouched: there is
    // one list per process and merging two of them is a guess.
    CHECK_EQ(boxedvn::guestGlibcTunablesValue("glibc.cpu.hwcaps=-AVX512F"),
             std::string("glibc.cpu.hwcaps=-AVX512F"));
    CHECK_EQ(boxedvn::guestGlibcTunablesValue(
                 "glibc.malloc.arena_max=1:glibc.cpu.hwcaps=-AVX512F"),
             std::string("glibc.malloc.arena_max=1:glibc.cpu.hwcaps=-AVX512F"));

    // The name has to be a whole tunable, not the tail of a longer one.
    CHECK(!boxedvn::glibcTunablesSetHwcaps("glibc.notglibc.cpu.hwcaps=-AVX"));
    CHECK(boxedvn::glibcTunablesSetHwcaps("glibc.cpu.hwcaps=-AVX"));
    CHECK(boxedvn::glibcTunablesSetHwcaps("glibc.malloc.arena_max=1:glibc.cpu.hwcaps=-AVX"));
    CHECK(!boxedvn::glibcTunablesSetHwcaps("glibc.cpu.x86_shstk=permissive"));

    CHECK(boxedvn::isGlibcTunablesAssignment("GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX"));
    CHECK(!boxedvn::isGlibcTunablesAssignment("WINEPREFIX=/tmp/prefix"));
}

BOXEDVN_TEST(wine64_layout_merges_the_dxvk_d3d9_override_into_the_callers) {
    // The app always sets WINEDLLOVERRIDES for the DXMT modules, so this is
    // the case that runs. Declining left the projected DXVK image unused.
    CHECK_EQ(boxedvn::wineDllOverridesWithDxvkD3d9(
                 "d3d11,dxgi,d3d10core,winemetal=n,b"),
             std::string("d3d11,dxgi,d3d10core,winemetal=n,b;d3d9=n"));
    // Nothing set: unchanged from before, the launch supplies the whole value.
    CHECK_EQ(boxedvn::wineDllOverridesWithDxvkD3d9(""), std::string("d3d9=n"));
    // A caller that named d3d9 itself keeps its own load order.
    CHECK_EQ(boxedvn::wineDllOverridesWithDxvkD3d9("d3d9=b"),
             std::string("d3d9=b"));
    CHECK_EQ(boxedvn::wineDllOverridesWithDxvkD3d9("dxgi=n,b;d3d9,d3d8=n"),
             std::string("dxgi=n,b;d3d9,d3d8=n"));

    // Whole module names only, case-insensitively, and never a fragment of a
    // longer one -- d3dx9_43 is a different module from d3d9.
    CHECK(boxedvn::wineDllOverridesNameModule("D3D9=n", "d3d9"));
    CHECK(boxedvn::wineDllOverridesNameModule("dxgi,d3d9 = n,b", "d3d9"));
    CHECK(!boxedvn::wineDllOverridesNameModule("d3dx9_43=n", "d3d9"));
    CHECK(!boxedvn::wineDllOverridesNameModule("d3d9x=n", "d3d9"));
    // The load order is not a module name: a value that happens to spell one
    // must not count as an override of it.
    CHECK(!boxedvn::wineDllOverridesNameModule("dxgi=d3d9", "d3d9"));
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

BOXEDVN_TEST(wine64_layout_places_the_32_bit_pe_tree_beside_the_64_bit_one) {
    // New WoW64 runs 32-bit PE builtins inside the 64-bit Unix process, so the
    // i386 tree is a third architecture directory under the SAME module root
    // Wine derives from ntdll.so's parent -- not a second Wine installation.
    CHECK_EQ(std::string(K_X64_WINE_PE32_DIR),
             std::string(K_X64_WINE_MODULE_ROOT) + "/i386-windows");
    CHECK(std::string(K_X64_WINE_PE32_DIR) != std::string(K_X64_WINE_PE_DIR));
    // There is deliberately no i386-unix constant: the Unix side stays 64-bit,
    // which is what removes the need for 32-bit Linux libraries.
    CHECK_EQ(std::string(K_X64_WINE_UNIX_DIR),
             std::string(K_X64_WINE_MODULE_ROOT) + "/x86_64-unix");
}

BOXEDVN_TEST(wine64_layout_names_the_32_bit_import_chain_a_program_needs) {
    // A device run of a 32-bit Direct3D 9 probe entered 32-bit mode at ntdll's
    // WoW64 entry and resolved every one of these out of the projected tree
    // except zlib1.dll, which 32-bit wined3d imports. The 64-bit tree carried
    // it and the 32-bit tree did not, so the process ended
    // STATUS_DLL_NOT_FOUND before its own entry point -- a program that never
    // appears rather than an error anything reports.
    //
    // libgcc_s_dw2-1.dll came from a later run of the same probe and is the
    // same kind of name: a mingw runtime DLL Wine's i386 PE modules import
    // and neither Wine tree builds. Its absence is quieter still -- the
    // importing builtin fails to load and the program runs on without
    // Direct3D -- so the list is the only place it is named.
    const std::vector<std::string> names =
        boxedvn::x64Wow64LanePe32ModuleNames();
    CHECK_EQ(names.size(), (size_t)K_X64_WOW64_LANE_PE32_MODULE_COUNT);
    for (const char* expected : {"ntdll.dll", "kernel32.dll", "kernelbase.dll",
                                 "advapi32.dll", "sechost.dll", "msvcrt.dll",
                                 "ucrtbase.dll", "gdi32.dll", "user32.dll",
                                 "win32u.dll", "opengl32.dll", "wined3d.dll",
                                 "d3d9.dll", "zlib1.dll",
                                 "libgcc_s_dw2-1.dll"}) {
        CHECK(std::find(names.begin(), names.end(), std::string(expected)) !=
              names.end());
    }
    // These are 32-bit builtins, not Wine's 64-bit WoW64 thunking layer: the
    // thunk modules live in the x86_64-windows tree and serve the lane from
    // the other side of the transition.
    for (const std::string& thunk : {std::string("wow64.dll"),
                                     std::string("wow64win.dll"),
                                     std::string("wow64cpu.dll")}) {
        CHECK(std::find(names.begin(), names.end(), thunk) == names.end());
    }
}

BOXEDVN_TEST(wine64_layout_names_wines_own_wow64_thunk_modules) {
    const char* const names[] = K_X64_WOW64_MODULE_NAMES;
    CHECK_EQ(sizeof(names) / sizeof(names[0]),
             (size_t)K_X64_WOW64_MODULE_COUNT);
    // These are 64-bit builtins: ntdll loads wow64.dll to build the 32-bit
    // process, wow64win.dll thunks user/GDI syscalls into the 64-bit win32u,
    // and wow64cpu.dll performs the mode transfer.
    CHECK_EQ(std::string(names[0]), std::string("wow64.dll"));
    CHECK_EQ(std::string(names[1]), std::string("wow64win.dll"));
    CHECK_EQ(std::string(names[2]), std::string("wow64cpu.dll"));
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
    // Large enough for a program's whole import tree - 128 stopped forty
    // operations short of the import that failed - and still bounded.
    CHECK(K_DLL_SEARCH_TRACE_BUDGET >= 1024);
    CHECK(K_DLL_SEARCH_TRACE_BUDGET <= 4096);
    boxedvn::setDllSearchTraceEnabled(false);
}

BOXEDVN_TEST(dll_search_module_name_reads_the_module_a_path_ends_in) {
    char name[K_DLL_SEARCH_MODULE_NAME_MAX];

    CHECK(boxedvn::dllSearchModuleName(
        "/home/username/.wine64/dosdevices/c:/windows/system32/MSVCP140.dll",
        name, sizeof(name)));
    CHECK_EQ(std::string(name), std::string("msvcp140.dll"));

    // Wine builds the same name with either separator, and the case it uses
    // is the case the import table carries.
    CHECK(boxedvn::dllSearchModuleName("D:\\Program\\Fmod64.DLL", name,
                                       sizeof(name)));
    CHECK_EQ(std::string(name), std::string("fmod64.dll"));

    // A directory, the main image and the Unix half of a builtin are not
    // imports.
    CHECK(!boxedvn::dllSearchModuleName(
        "/home/username/.wine64/dosdevices/c:/windows/system32", name,
        sizeof(name)));
    CHECK(!boxedvn::dllSearchModuleName("D:\\Program\\program.exe", name,
                                        sizeof(name)));
    CHECK(!boxedvn::dllSearchModuleName(
        "/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winemetal.dll.so", name,
        sizeof(name)));
    CHECK(!boxedvn::dllSearchModuleName(nullptr, name, sizeof(name)));
}

BOXEDVN_TEST(dll_search_trace_names_the_module_no_search_path_produced) {
    boxedvn::setDllSearchTraceEnabled(true);
    boxedvn::DllSearchTrace trace;
    char module[K_DLL_SEARCH_MODULE_NAME_MAX];

    // A module found on a later path is not missing, however many probes
    // missed first - the loader stops at the first hit.
    trace.noteResult("/home/username/.wine64/dosdevices/d:/game/dxgi.dll", -2);
    trace.noteResult("/home/username/.wine64/dosdevices/c:/windows/system32/dxgi.dll", 0);
    trace.lastUnresolvedModule(module, sizeof(module));
    CHECK_EQ(std::string(module), std::string());

    // An import searched for in the middle of its parent's search, and found
    // at the module root, must not displace the one that is still missing.
    trace.noteResult("/home/username/.wine64/dosdevices/d:/game/fmod64.dll", -2);
    trace.noteResult("/home/username/.wine64/dosdevices/d:/game/winemetal.dll", -2);
    trace.noteResult("/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/winemetal.dll", 7);
    trace.lastUnresolvedModule(module, sizeof(module));
    CHECK_EQ(std::string(module), std::string("fmod64.dll"));

    // The name survives a spent budget: that is the case it exists for.
    for (unsigned i = 0; i < K_DLL_SEARCH_TRACE_BUDGET + 8; ++i) {
        trace.record("/usr/lib/wine/kernel32.dll");
    }
    trace.noteResult("/home/username/.wine64/dosdevices/d:/game/fmod64.dll", -2);
    trace.lastUnresolvedModule(module, sizeof(module));
    CHECK_EQ(std::string(module), std::string("fmod64.dll"));
    CHECK(trace.probes() >= 5);
    boxedvn::setDllSearchTraceEnabled(false);
}

BOXEDVN_TEST(dll_search_trace_records_nothing_when_it_is_not_armed) {
    boxedvn::setDllSearchTraceEnabled(false);
    boxedvn::DllSearchTrace trace;
    char module[K_DLL_SEARCH_MODULE_NAME_MAX];
    trace.noteResult("/home/username/.wine/dosdevices/d:/game/fmod64.dll", -2);
    trace.lastUnresolvedModule(module, sizeof(module));
    CHECK_EQ(std::string(module), std::string());
    CHECK_EQ(trace.probes(), 0u);
}
