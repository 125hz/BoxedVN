#!/usr/bin/env bash
# BoxedVN - assemble the x86-64 Linux/Wine64 layers on an Ubuntu runner.
#
# This is the CI counterpart of the audited container recipe. It deliberately
# consumes the runner's distro Wine package instead of downloading an opaque
# image: the package manager, runner image, and resulting archive hashes are
# recorded in the generated manifest. The output is suitable for the
# BoxedWine64 -root/-zip launch sequence.
#
# Usage:
#   scripts/build-wine64-runtime-ci.sh --output-dir DIR \
#       [--dxmt-unixlib PATH]

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

OUTPUT_DIR=""
DXMT_UNIXLIB=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) [[ $# -ge 2 ]] || die "--output-dir needs a value"
                       OUTPUT_DIR="$2"; shift 2 ;;
        --dxmt-unixlib) [[ $# -ge 2 ]] || die "--dxmt-unixlib needs a value"
                         DXMT_UNIXLIB="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done

[[ -n "${OUTPUT_DIR}" ]] || die "--output-dir is required."
if [[ -n "${DXMT_UNIXLIB}" ]]; then
    require_file "${DXMT_UNIXLIB}" \
        "Build the x86-64 guest DXMT unixlib before assembling the runtime."
fi
[[ "$(uname -s)" == "Linux" ]] || die "This builder must run on Linux."
require_command find
require_command ldd
require_command readlink
require_command file
require_command unzip
require_command zip
if [[ -n "${DXMT_UNIXLIB}" ]]; then
    file "${DXMT_UNIXLIB}" | grep -Eqi 'ELF 64-bit.*x86-64' \
        || die "'${DXMT_UNIXLIB}' is not an x86-64 ELF shared object."
fi

OUTPUT_DIR="$(mkdir -p "${OUTPUT_DIR}" && cd "${OUTPUT_DIR}" && pwd)"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/boxedvn-wine64-stage.XXXXXX")"
cleanup() { rm -rf "${STAGE}"; }
trap cleanup EXIT

find_first() {
    local candidate
    for candidate in "$@"; do
        if [[ -f "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

WINE64="$(find_first \
    /usr/lib/wine/wine64 \
    /usr/lib/x86_64-linux-gnu/wine/wine64 \
    "$(command -v wine64 2>/dev/null || true)" \
    "$(command -v wine 2>/dev/null || true)")" \
    || die "The Ubuntu Wine64 package is not installed or exposes no wine64 ELF."

WINE_ROOT="$(dirname "${WINE64}")"
WINE_UNIX=""
WINE_WINDOWS=""
for candidate in \
    "${WINE_ROOT}/x86_64-unix" \
    /usr/lib/x86_64-linux-gnu/wine/x86_64-unix \
    /usr/lib/wine/x86_64-unix; do
    if [[ -d "${candidate}" ]]; then WINE_UNIX="${candidate}"; break; fi
done
for candidate in \
    "${WINE_ROOT}/x86_64-windows" \
    /usr/lib/x86_64-linux-gnu/wine/x86_64-windows \
    /usr/lib/wine/x86_64-windows; do
    if [[ -d "${candidate}" ]]; then WINE_WINDOWS="${candidate}"; break; fi
done
[[ -n "${WINE_UNIX}" && -n "${WINE_WINDOWS}" ]] \
    || die "Wine64 module trees were not found beside '${WINE64}'."

WINE_SERVER="$(find_first \
    /usr/lib/wine/wineserver64 \
    /usr/lib/x86_64-linux-gnu/wine/wineserver64 \
    /usr/lib/wine/wineserver \
    /usr/lib/x86_64-linux-gnu/wine/wineserver)" \
    || die "The Ubuntu Wine package exposes no wineserver64 binary."

log "Wine64 loader: ${WINE64}"
log "Wine64 unix modules: ${WINE_UNIX}"

copy_abs() {
    local source="$1"
    [[ -e "${source}" ]] || return 0
    local real
    real="$(readlink -f "${source}")"
    [[ -f "${real}" ]] || return 0
    mkdir -p "${STAGE}$(dirname "${source}")"
    cp -aL "${real}" "${STAGE}${source}"
}

copy_as() {
    local source="$1" destination="$2" real
    [[ -e "${source}" ]] || die "Required Wine64 path '${source}' is missing."
    real="$(readlink -f "${source}")"
    [[ -f "${real}" ]] || die "Wine64 path '${source}' is not a regular file."
    mkdir -p "${STAGE}$(dirname "${destination}")"
    cp -aL "${real}" "${STAGE}${destination}"
}

# Resolve the complete native dependency closure of Wine's ELF entry points.
# This keeps the layer independent of the host filesystem after it is mounted
# into BoxedWine and catches both /lib and /usr/lib layouts used by Ubuntu.
SEEDS=("${WINE64}" "${WINE_SERVER}" /lib64/ld-linux-x86-64.so.2)
while IFS= read -r module; do SEEDS+=("${module}"); done < <(
    find "${WINE_UNIX}" -maxdepth 1 -type f -name '*.so' -print
)
# winex11.drv resolves these client libraries with dlopen, so they do not
# appear in the ELF DT_NEEDED closure above. Include their direct dependencies
# as well; the Ubuntu job installs the corresponding runtime packages.
for x11lib in \
    /usr/lib/x86_64-linux-gnu/libXrender.so.1 \
    /usr/lib/x86_64-linux-gnu/libXcursor.so.1 \
    /usr/lib/x86_64-linux-gnu/libXfixes.so.3 \
    /usr/lib/x86_64-linux-gnu/libXcomposite.so.1 \
    /usr/lib/x86_64-linux-gnu/libXi.so.6 \
    /usr/lib/x86_64-linux-gnu/libXinerama.so.1 \
    /usr/lib/x86_64-linux-gnu/libXxf86vm.so.1 \
    /usr/lib/x86_64-linux-gnu/libXrandr.so.2; do
    [[ -f "${x11lib}" ]] || continue
    SEEDS+=("${x11lib}")
done
# Wine's X11 and user/environment helpers load NSS modules dynamically. They
# are not present in DT_NEEDED, but getpwuid/getaddrinfo can reach them before
# the first window is created. Include the small glibc NSS set explicitly.
for nsslib in \
    /lib/x86_64-linux-gnu/libnss_files.so.2 \
    /lib/x86_64-linux-gnu/libnss_dns.so.2 \
    /lib/x86_64-linux-gnu/libnss_compat.so.2 \
    /lib/x86_64-linux-gnu/libresolv.so.2 \
    /usr/lib/x86_64-linux-gnu/libnss_files.so.2 \
    /usr/lib/x86_64-linux-gnu/libnss_dns.so.2 \
    /usr/lib/x86_64-linux-gnu/libnss_compat.so.2 \
    /usr/lib/x86_64-linux-gnu/libresolv.so.2; do
    [[ -f "${nsslib}" ]] || continue
    SEEDS+=("${nsslib}")
done
LIBS=""
for seed in "${SEEDS[@]}"; do
    [[ -f "${seed}" ]] || continue
    while IFS= read -r library; do
        [[ -n "${library}" ]] && LIBS+="${library}"$'\n'
    done < <(ldd "${seed}" 2>/dev/null | awk '
        /=>/ && $3 ~ /^\// { print $3 }
        /^[[:space:]]*\// && $1 ~ /^\// { print $1 }
    ')
done
while IFS= read -r library; do
    [[ -n "${library}" ]] && copy_abs "${library}"
done < <(printf '%s' "${LIBS}" | sort -u)

ld_loader="$(printf '%s\n' "${LIBS}" | awk '/ld-linux-x86-64\.so\.2$/ { print; exit }')"
if [[ -n "${ld_loader}" ]]; then copy_abs "${ld_loader}"; fi
copy_abs /lib/x86_64-linux-gnu/libc.so.6
copy_abs /lib/x86_64-linux-gnu/libgcc_s.so.1
copy_abs /usr/lib/x86_64-linux-gnu/libgcc_s.so.1

# Normalize the loader paths expected by the BoxedWine64 launcher while
# preserving Ubuntu's module-tree path. Some distro releases put wine64 under
# /usr/lib/x86_64-linux-gnu/wine; the guest must not depend on that variation.
copy_as "${WINE64}" /usr/lib/wine/wine64
copy_as "${WINE_SERVER}" /usr/lib/wine/wineserver64
if [[ -f "${WINE_ROOT}/wineserver" ]]; then
    copy_as "${WINE_ROOT}/wineserver" /usr/lib/wine/wineserver
fi
mkdir -p "${STAGE}/usr/lib/x86_64-linux-gnu/wine"
cp -aL "${WINE_UNIX}" "${STAGE}/usr/lib/x86_64-linux-gnu/wine/"
cp -aL "${WINE_WINDOWS}" "${STAGE}/usr/lib/x86_64-linux-gnu/wine/"
if [[ -n "${DXMT_UNIXLIB}" ]]; then
    cp "${DXMT_UNIXLIB}" \
       "${STAGE}/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winemetal.so"
fi

if [[ -d /usr/share/wine ]]; then
    mkdir -p "${STAGE}/usr/share"
    cp -aL /usr/share/wine "${STAGE}/usr/share/"
fi
if [[ -d /usr/share/X11/locale ]]; then
    mkdir -p "${STAGE}/usr/share/X11"
    cp -aL /usr/share/X11/locale "${STAGE}/usr/share/X11/"
fi
if [[ -d /usr/share/locale ]]; then
    mkdir -p "${STAGE}/usr/share"
    cp -aL /usr/share/locale "${STAGE}/usr/share/"
fi
if [[ -d /usr/lib/locale/C.utf8 ]]; then
    mkdir -p "${STAGE}/usr/lib/locale"
    cp -aL /usr/lib/locale/C.utf8 "${STAGE}/usr/lib/locale/"
fi
if [[ -f /etc/localtime ]]; then
    mkdir -p "${STAGE}/etc"
    cp -aL /etc/localtime "${STAGE}/etc/localtime"
fi
mkdir -p "${STAGE}/etc"
# Keep identity and NSS selection deterministic. The runtime is mounted as a
# private guest root, so copying the runner's passwd/group database would make
# its host accounts part of the app resource.
printf '%s\n' \
    'root:x:0:0:root:/root:/bin/sh' \
    'username:x:1000:1000:username:/home/username:/bin/sh' \
    'nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin' \
    > "${STAGE}/etc/passwd"
printf '%s\n' \
    'root:x:0:' \
    'users:x:100:' \
    'username:x:1000:' \
    'nogroup:x:65534:' \
    > "${STAGE}/etc/group"
printf '%s\n' \
    'passwd: files' \
    'group: files' \
    'shadow: files' \
    'hosts: files dns' \
    'networks: files' \
    'protocols: files' \
    'services: files' \
    > "${STAGE}/etc/nsswitch.conf"
mkdir -p "${STAGE}/usr/bin" "${STAGE}/root"
ln -s /usr/lib/wine/wine64 "${STAGE}/usr/bin/wine64"
ln -s /usr/lib/wine/wineserver64 "${STAGE}/usr/bin/wineserver"

# These directories are created by wineserver and by Wine's default prefix.
mkdir -p "${STAGE}/tmp" "${STAGE}/run/user/1000" \
         "${STAGE}/home/username" "${STAGE}/var/tmp"

rm -f "${OUTPUT_DIR}/glibc-rootfs64.zip" "${OUTPUT_DIR}/wine64.zip" \
      "${OUTPUT_DIR}/wine64-runtime.manifest"
( cd "${STAGE}" && zip -qry9 "${OUTPUT_DIR}/glibc-rootfs64.zip" \
      lib64 lib etc tmp run home var )
( cd "${STAGE}" && zip -qry9 "${OUTPUT_DIR}/wine64.zip" usr root )

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}
{
    printf '%s\n' '# BoxedVN Wine64 runtime manifest v1.'
    printf '%s\n' 'format=boxedvn-wine64-v1'
    printf '%s\n' 'source=scripts/build-wine64-runtime-ci.sh'
    printf '%s\n' 'source_image=ubuntu-24.04-apt'
    # Ubuntu's Wine64 is not being presented as an iOS-native Wine build.
    # Upstream Wine deliberately starts its 64-bit TEB allocation below 2 GiB
    # and publishes KUSER_SHARED_DATA at 0x7ffe0000.  The BoxedWine native
    # identity backend therefore has to provide its high-window allocator and
    # KUSER alias/fault bridge; recording this contract makes an accidental
    # stock-runtime acceptance visible to the validator and to device logs.
    printf '%s\n' 'wine_address_contract=stock-low-teb-hint-fixed-kuser-v1'
    printf '%s\n' 'boxedwine_bridge_required=K64_NATIVE_IDENTITY_KUSER_ALIAS'
    printf '%s\n' 'glibc_archive=glibc-rootfs64.zip'
    printf 'glibc_sha256=%s\n' "$(sha256_file "${OUTPUT_DIR}/glibc-rootfs64.zip")"
    printf '%s\n' 'wine_archive=wine64.zip'
    printf 'wine_sha256=%s\n' "$(sha256_file "${OUTPUT_DIR}/wine64.zip")"
    if [[ -n "${DXMT_UNIXLIB}" ]]; then
        printf 'dxmt_unixlib_sha256=%s\n' "$(sha256_file "${DXMT_UNIXLIB}")"
    fi
} > "${OUTPUT_DIR}/wine64-runtime.manifest"

ok "Wine64 runtime layers: ${OUTPUT_DIR}"
du -h "${OUTPUT_DIR}"/*.zip
