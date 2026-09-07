#!/usr/bin/env bash
# One source tree supplies both PE architectures, ntdll, wineserver and OSS.
set -euo pipefail
cd "$(dirname "$0")/.."
version=11.0
source_sha=c07a6857933c1fc60dff5448d79f39c92481c1e9db5aa628db9d0358446e0701
work=build/wine11-source
mkdir -p "${work}"
archive="${work}/wine-${version}.tar.xz"
curl --fail --location --retry 3 \
    "https://dl.winehq.org/wine/source/11.0/wine-${version}.tar.xz" -o "${archive}"
printf '%s  %s\n' "${source_sha}" "${archive}" | sha256sum --check --strict
tar -xf "${archive}" -C "${work}"
# Use Wine's portable read-only shared-section path. Linux MAP_PRIVATE
# visibility depends on a page cache that BoxedWine does not currently model.
patch --forward -d "${work}/wine-${version}" -p1 < scripts/wine-patches/readonly-section-shared-backing.patch
python3 scripts/test_wine_readonly_sections.py "${work}/wine-${version}"
bash scripts/build-wine64-oss-driver.sh --wine-version "${version}" \
    --wine-source "${work}/wine-${version}" --output-dir build/wine64-oss \
    --runtime-install build/wine11-install
# These are compiler runtime dependencies, not Wine 9 modules. Preserve the
# packaging contract even when this compiler links its builtins statically.
pe32=build/wine11-install/usr/lib/x86_64-linux-gnu/wine/i386-windows
for name in zlib1.dll libgcc_s_dw2-1.dll; do
    if [[ ! -f "${pe32}/${name}" ]]; then
        runtime="$(find /usr/i686-w64-mingw32 /usr/lib/gcc/i686-w64-mingw32 -name "${name}" -print -quit)"
        test -n "${runtime}"
        cp "${runtime}" "${pe32}/${name}"
    fi
done
# Run the built Linux loader only for its version, never any user program.
test "$(build/wine11-install/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/wine --version)" = "wine-11.0"
