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
    test_fex_callret_guard.cpp
    test_fex_guest_mode_policy.cpp
    test_fex_exit_dispatch_contract.cpp
    test_fex32_guest_window.cpp
    test_fex32_kmemory_window_binding.cpp
    test_fex32_darwin_vm_header.cpp
    test_fex32_backend_admission.cpp
    test_fex32_backend_descriptor.cpp
    test_guest_address_contract32.cpp
    test_guest_address_space.cpp
    test_guest_mmap_diagnostics.cpp
    test_guest_mmap_placement.cpp
    test_guest_low_alias.cpp
    test_guest_top_alias.cpp
    test_native_map_plan.cpp
    test_syscall_tail_ring.cpp
    test_wine_server_reply.cpp
    test_guest_segment_table.cpp
    test_guest_signal_frame64.cpp
    test_launch_preflight.cpp
    test_exec_transition.cpp
    test_x64_runtime_dir.cpp
    test_guest_wine_prefix.cpp
    test_guest_wine64_layout.cpp
    test_guest_wine_audio_registration.cpp
    test_wine_nt_syscall_stub.cpp
    test_bounded_syscall_report.cpp
    test_sparse_reservation_policy.cpp
    test_cmpxchg16b.cpp
    test_boot_diagnostics.cpp
    test_pixmap_formats.cpp
    test_x11_layout64.cpp
    test_x11_bridge64_policy.cpp
    test_sse42_string_compare.cpp
    test_dxmt_guest_pointer.cpp
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

# Keep the complete production syscall body in this focused backend fixture.
# Reconfigure on edits so the tested function cannot lag behind the shipping one.
set(mmap_source "${CMAKE_CURRENT_SOURCE_DIR}/../../source/kernel/syscall64.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${mmap_source}")
file(READ "${mmap_source}" syscall_source)
string(FIND "${syscall_source}" "static U64 sys_mmap64_file(CPU64*" mmap_begin REVERSE)
string(FIND "${syscall_source}" "// writev — iterate the iovec array" mmap_end)
if(mmap_begin LESS 0 OR mmap_end LESS mmap_begin)
    message(FATAL_ERROR "Cannot locate production file-mmap syscall for regression fixture")
endif()
math(EXPR mmap_length "${mmap_end} - ${mmap_begin}")
string(SUBSTRING "${syscall_source}" ${mmap_begin} ${mmap_length} BOXEDVN_MMAP_SYSCALL_BODY)
configure_file(test_shared_file_mmap.cpp.in test_shared_file_mmap.cpp @ONLY)
add_executable(boxedvn_shared_file_mmap_test "${CMAKE_CURRENT_BINARY_DIR}/test_shared_file_mmap.cpp")
target_include_directories(boxedvn_shared_file_mmap_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
add_test(NAME boxedvn_shared_file_mmap COMMAND boxedvn_shared_file_mmap_test)

string(FIND "${syscall_source}" "static S32 directoryBase64(" at_begin REVERSE)
string(FIND "${syscall_source}" "static U64 sys_fstat64(CPU64* cpu, U64 fd, U64 statbuf) {" at_end)
if(at_begin LESS 0 OR at_end LESS at_begin)
    message(FATAL_ERROR "Cannot locate production stat syscalls for regression fixture")
endif()
math(EXPR at_length "${at_end} - ${at_begin}")
string(SUBSTRING "${syscall_source}" ${at_begin} ${at_length} BOXEDVN_AT_SYSCALL_BODY)
configure_file(test_at_file_syscalls.cpp.in test_at_file_syscalls.cpp @ONLY)
add_executable(boxedvn_at_file_syscalls_test "${CMAKE_CURRENT_BINARY_DIR}/test_at_file_syscalls.cpp")
add_test(NAME boxedvn_at_file_syscalls COMMAND boxedvn_at_file_syscalls_test)

set(pacer_source "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/BVNAppDelegate.mm")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pacer_source}")
file(READ "${pacer_source}" pacer_text)
string(FIND "${pacer_text}" "static int BVNGuestFrameRateLimitForMode(" mode_begin)
string(FIND "${pacer_text}" "// DXMT's native present path" mode_end)
string(FIND "${pacer_text}" "static void BVNResetGuestFramePacer(" pacer_begin)
string(FIND "${pacer_text}" "static void BVNApplyGuestFrameRateMode(void) {" pacer_end)
if(mode_begin LESS 0 OR mode_end LESS mode_begin OR pacer_begin LESS 0 OR pacer_end LESS pacer_begin)
    message(FATAL_ERROR "Cannot locate production frame pacing functions")
endif()
math(EXPR mode_length "${mode_end} - ${mode_begin}")
math(EXPR pacer_length "${pacer_end} - ${pacer_begin}")
string(SUBSTRING "${pacer_text}" ${mode_begin} ${mode_length} BOXEDVN_FRAME_MODE_BODY)
string(SUBSTRING "${pacer_text}" ${pacer_begin} ${pacer_length} BOXEDVN_FRAME_PACER_BODY)
configure_file(test_guest_frame_pacer.cpp.in test_guest_frame_pacer.cpp @ONLY)
add_executable(boxedvn_guest_frame_pacer_test "${CMAKE_CURRENT_BINARY_DIR}/test_guest_frame_pacer.cpp")
add_test(NAME boxedvn_guest_frame_pacer COMMAND boxedvn_guest_frame_pacer_test)

# Dependency pinning: verifies every pin is well formed and that a mismatched
# checksum is actually refused.  Needs no network access.
if(UNIX)
    add_test(NAME boxedvn_dependency_pins
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/test-pins.sh")
endif()
