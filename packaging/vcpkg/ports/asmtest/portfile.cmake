# vcpkg overlay port for the asm-test C core: the MIT static library, headers and
# pkg-config file (the GPL engines are conveyed only by the language bindings,
# never by this port). No CMake upstream — the build drives the upstream Makefile.
#
# Pins the v1.1.0 release tarball ASSET by SHA512. That asset is published by
# distribution-packaging.md T3 (a maintainer action) and does not exist in this
# tree yet, so `make docker-syspkg-vcpkg` is hermetic: it pre-places the
# reproducible `make package-source` tarball in vcpkg's downloads cache under the
# name vcpkg_from_github expects and matches its SHA512, so no network fetch runs.
# Upstreaming to microsoft/vcpkg (a one-port PR) waits on the >=6-month maturity
# criterion (v1.0.0 is 2026-06-24); the overlay port is the supported path meanwhile.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wilvk/asm-test
    REF "v${VERSION}"
    SHA512 319ebeae4dd840e7b6d34a0f1512deeec8f7c3d1626c4e58db67074914d9ba646426a1105a5ca90ba3f0a408ae5975da093513c6c7fa8b33e13c1bb9e2435572
    HEAD_REF main
)

# The upstream `install` target lays out headers + the static lib + the .pc under
# a prefix exactly as vcpkg wants (include/asmtest, lib, lib/pkgconfig). It builds
# `lib` as a prerequisite, so one invocation covers build + install.
vcpkg_execute_required_process(
    COMMAND make install "PREFIX=${CURRENT_PACKAGES_DIR}"
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME "install-${TARGET_TRIPLET}"
)

# A single static archive serves both configs (x64-linux is static by default);
# mirror it into debug/lib so vcpkg's release/debug binary counts match.
file(INSTALL "${CURRENT_PACKAGES_DIR}/lib/libasmtest.a"
     DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")

vcpkg_fixup_pkgconfig()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
