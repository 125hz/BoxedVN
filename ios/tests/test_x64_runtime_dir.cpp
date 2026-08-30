#include "boxedvn_test.h"
#include "x64_runtime_dir.h"

#include <string>

using namespace boxedvn;

// FsFileNode::getMode grants owner write to the modelled user's runtime
// directory because ZIP entries carry no Unix mode and wineserver otherwise
// dies on `mkdir /run/user/1000/wine: Permission denied`. The exception has to
// stay exactly one subtree: a writable /run would be a far worse bug than the
// one it fixes.

BOXEDVN_TEST(x64_runtime_dir_matches_the_directory_and_its_subtree) {
    CHECK(isX64UserRuntimePath("/run/user/1000"));
    CHECK(isX64UserRuntimePath("/run/user/1000/"));
    CHECK(isX64UserRuntimePath("/run/user/1000/wine"));
    CHECK(isX64UserRuntimePath("/run/user/1000/wine/server"));
    CHECK(isX64UserRuntimePath("/run/user/1000/wine/socket"));
}

BOXEDVN_TEST(x64_runtime_dir_excludes_the_rest_of_run) {
    // The whole point: none of these may become writable.
    CHECK(!isX64UserRuntimePath("/run"));
    CHECK(!isX64UserRuntimePath("/run/"));
    CHECK(!isX64UserRuntimePath("/run/user"));
    CHECK(!isX64UserRuntimePath("/run/user/"));
    CHECK(!isX64UserRuntimePath("/run/systemd"));
    CHECK(!isX64UserRuntimePath("/run/lock"));
}

BOXEDVN_TEST(x64_runtime_dir_excludes_other_users) {
    CHECK(!isX64UserRuntimePath("/run/user/0"));
    CHECK(!isX64UserRuntimePath("/run/user/1001"));
    CHECK(!isX64UserRuntimePath("/run/user/999"));
    // A path that merely shares the leading characters is not inside it.
    CHECK(!isX64UserRuntimePath("/run/user/10001"));
    CHECK(!isX64UserRuntimePath("/run/user/1000x"));
    CHECK(!isX64UserRuntimePath("/run/user/1000-wine"));
}

BOXEDVN_TEST(x64_runtime_dir_excludes_unrelated_and_partial_paths) {
    CHECK(!isX64UserRuntimePath(nullptr));
    CHECK(!isX64UserRuntimePath(""));
    CHECK(!isX64UserRuntimePath("/"));
    CHECK(!isX64UserRuntimePath("/run/use"));
    CHECK(!isX64UserRuntimePath("/tmp/run/user/1000"));
    CHECK(!isX64UserRuntimePath("run/user/1000"));
    CHECK(!isX64UserRuntimePath("/home/username"));
    CHECK(!isX64UserRuntimePath("/usr/lib/wine/wineserver"));
    // Read-only rootfs paths are untouched.
    CHECK(!isX64UserRuntimePath("/lib64/ld-linux-x86-64.so.2"));
    CHECK(!isX64UserRuntimePath("/etc/passwd"));
}

BOXEDVN_TEST(x64_runtime_dir_constants_describe_a_private_directory) {
    CHECK(std::string(K_X64_USER_RUNTIME_DIR) == "/run/user/1000");
    CHECK(K_X64_MODELLED_UID == 1000);
    CHECK(K_X64_MODELLED_GID == 1000);
    // 0700: owner read, write and execute; nothing for group or other.
    CHECK(K_X64_USER_RUNTIME_DIR_MODE == 0700);
    CHECK((K_X64_USER_RUNTIME_DIR_MODE & 0070) == 0);
    CHECK((K_X64_USER_RUNTIME_DIR_MODE & 0007) == 0);
    // The path is the one the guest actually asks for: XDG_RUNTIME_DIR for
    // the single user the 64-bit syscall layer models.
    CHECK(std::string(K_X64_USER_RUNTIME_DIR) ==
          "/run/user/" + std::to_string(K_X64_MODELLED_UID));
}
