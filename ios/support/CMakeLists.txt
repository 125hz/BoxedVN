# BoxedVN support library: host-independent logic with no Boxedwine, SDL or
# UIKit dependency, so it can be unit tested on any host.

add_library(boxedvn_support STATIC
    src/architecture.cpp
    src/json.cpp
    src/manifest.cpp
    src/path_safety.cpp
    src/pe_inspector.cpp
    src/zip_import.cpp
    src/game_import.cpp
)

target_include_directories(boxedvn_support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(boxedvn_support PRIVATE boxedwine_zlib)

target_compile_features(boxedvn_support PUBLIC cxx_std_20)

target_compile_options(boxedvn_support PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Mirrors the JIT configuration of boxedwine_core so
# boxedwineX86Capabilities() reports what was actually built.
if(BOXEDVN_TARGET_IOS)
    target_compile_definitions(boxedvn_support PRIVATE BOXEDWINE_JIT)
endif()
