# Host-independent tests.  These run on any machine CMake can build for,
# including a CI runner with nothing installed beyond Xcode and CMake.

add_executable(boxedvn_tests
    test_main.cpp
    test_pe_inspector.cpp
    test_path_safety.cpp
    test_manifest.cpp
    test_zip_import.cpp
)

target_include_directories(boxedvn_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# boxedwine_zlib is linked directly so the ZIP tests can build hostile archives
# with minizip's writer rather than checking in binary fixtures.
target_link_libraries(boxedvn_tests PRIVATE boxedvn_support boxedwine_zlib)
target_compile_options(boxedvn_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_test(NAME boxedvn_support COMMAND boxedvn_tests)
set_tests_properties(boxedvn_support PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
