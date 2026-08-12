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
    test_getrusage_fairness.cpp
    test_vk_pipeline_guard.cpp
    test_wine_prefix.cpp
    ../runtime/src/BVNLaunchArguments.cpp
    ../runtime/src/BVNRangeAllocator.cpp
    ../runtime/src/BVNJitArenaPlan.cpp
)

target_include_directories(boxedvn_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
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
