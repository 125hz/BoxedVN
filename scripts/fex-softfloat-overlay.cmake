# Keep reference arithmetic and its ABI, but expose its helper bodies to the
# optimizer. No fast-math, reduced precision, or vendor source changes.
function(boxedvn_optimize_softfloat)
    set_target_properties(softfloat_3e PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 0)
    # These unused entry points deliberately lack dependencies in FEX's trimmed
    # archive. Keep them independently extractable rather than pulling them in
    # when an x87 entry point is linked.
    get_target_property(source_dir softfloat_3e SOURCE_DIR)
    foreach(source f128_mulAdd f128_to_f16 f128_to_ui32)
        set_property(SOURCE "${source_dir}/src/${source}.c" DIRECTORY "${source_dir}"
            PROPERTY SKIP_UNITY_BUILD_INCLUSION ON)
    endforeach()
endfunction()
cmake_language(DEFER CALL boxedvn_optimize_softfloat)
