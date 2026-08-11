#!/bin/sh
# The version is written down in four places: the project file, the Debian
# changelog, the RPM spec and the PKGBUILD. Three of them are edited by hand at
# release time and any one of them can be forgotten, which produces a package
# built from one tree and named after another. That is the sort of mistake
# nobody notices until somebody reports a bug against a version that was never
# built.
#
#   packaging/check-versions.sh
#
# Run by the packaging jobs before anything is built, and by the version-check
# job in CI so that a stale changelog fails the pull request that introduced it
# rather than the release three weeks later.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
status=0

expected=$(sed -n 's/^set(PS_VERSION *"\([^"]*\)".*/\1/p' "${root}/CMakeLists.txt")
if [ -z "${expected}" ]; then
    echo "could not read PS_VERSION out of CMakeLists.txt" >&2
    exit 1
fi
echo "CMakeLists.txt          ${expected}"

check() {
    printf '%-24s%s\n' "$1" "${2:-<not found>}"
    if [ "$2" != "${expected}" ]; then
        echo "  ^ expected ${expected}" >&2
        status=1
    fi
}

# 3.0 (native), so the changelog version carries no Debian revision.
deb=$(sed -n '1s/^pdf-smithy (\([^)]*\)).*/\1/p' "${root}/debian/changelog")
check "debian/changelog" "${deb}"

rpm=$(sed -n 's/^Version: *\([^ ]*\) *$/\1/p' "${root}/packaging/rpm/pdf-smithy.spec")
check "rpm spec" "${rpm}"

arch=$(sed -n 's/^pkgver=\(.*\)$/\1/p' "${root}/packaging/arch/PKGBUILD")
check "arch PKGBUILD" "${arch}"

if [ "${status}" -ne 0 ]; then
    echo >&2
    echo "The packaging recipes disagree with CMakeLists.txt about the version." >&2
    echo "Bring them into line before building anything." >&2
fi
exit "${status}"
