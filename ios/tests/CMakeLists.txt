# Host-independent tests.  These run on any machine CMake can build for,
# including a CI runner with nothing installed beyond Xcode and CMake.

add_executable(boxedvn_tests
    test_main.cpp
    test_pe_inspector.cpp
    test_path_safety.cpp
    test_manifest.cpp
    test_zip_import.cpp
    test_launch_arguments.cpp
    test_range_allocator.cpp
    test_jit_arena_plan.cpp
    test_direct3d_profile.cpp
    test_engine_profile.cpp
    test_elf_inspector.cpp
    test_elf_loader_policy.cpp
    test_fex_code_buffer_layout.cpp
    test_fex64_kernel.cpp
    test_fex64_loader_handoff.cpp
    test_fex_guest_mode_policy.cpp
    test_fex_exit_dispatch_contract.cpp
    test_fex32_guest_window.cpp
    test_fex32_kmemory_window_binding.cpp
    test_fex32_darwin_vm_header.cpp
    test_fex32_backend_admission.cpp
    test_fex32_backend_descriptor.cpp
    test_guest_address_contract32.cpp
    test_guest_address_space.cpp
    test_guest_mmap_placement.cpp
    test_guest_low_alias.cpp
    test_boot_diagnostics.cpp
    test_pixmap_formats.cpp
    test_getrusage_fairness.cpp
    test_kernel_random.cpp
    test_vk_pipeline_guard.cpp
    test_wine_prefix.cpp
    ../runtime/src/BVNLaunchArguments.cpp
    ../runtime/src/BVNRangeAllocator.cpp
    ../runtime/src/BVNJitArenaPlan.cpp
    ../../source/x11/xpixmapformats.cpp
)

target_include_directories(boxedvn_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../source/x11"
)
# boxedwine_zlib is linked directly so the ZIP tests can build hostile archives
# with minizip's writer rather than checking in binary fixtures.
target_link_libraries(boxedvn_tests PRIVATE boxedvn_support boxedwine_zlib)
if(MSVC)
    target_compile_options(boxedvn_tests PRIVATE /W4 /wd4100 /EHsc)
else()
    target_compile_options(boxedvn_tests PRIVATE
        -Wall -Wextra -Wno-unused-parameter)
endif()

add_test(NAME boxedvn_support COMMAND boxedvn_tests)
set_tests_properties(boxedvn_support PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# Dependency pinning: verifies every pin is well formed and that a mismatched
# checksum is actually refused.  Needs no network access.
if(UNIX)
    add_test(NAME boxedvn_dependency_pins
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/test-pins.sh")
endif()
