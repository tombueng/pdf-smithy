#!/bin/sh
# Builds the Arch package from a checkout.
#
#   packaging/arch/build.sh
#
# The PKGBUILD points at the tarball GitHub generates for a tag, which does not
# exist until the tag is pushed. This makes the same tarball out of the working
# tree, points a copy of the PKGBUILD at it, and runs makepkg, so a change to
# the recipe is tried before the tag that would publish it exists.
#
# makepkg refuses to run as root, which is worth knowing if this is being run in
# a container: create an unprivileged user first. The result lands in
# build-arch/.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
work="${root}/build-arch"
version=$(sed -n 's/^set(PS_VERSION *"\([^"]*\)".*/\1/p' "${root}/CMakeLists.txt")

"${root}/packaging/check-versions.sh"

rm -rf "${work}"
mkdir -p "${work}"
"${root}/packaging/make-tarball.sh" "${work}/pdf-smithy-${version}.tar.gz" > /dev/null

# The one line that has to change: the remote tarball becomes the local one.
# Everything below it in the recipe, which is the part worth testing, is used
# exactly as it will be used from the AUR.
sed "s|^source=(.*|source=(\"pdf-smithy-${version}.tar.gz\")|" \
    "${root}/packaging/arch/PKGBUILD" > "${work}/PKGBUILD"

cd "${work}"
makepkg --force --syncdeps --noconfirm

echo
echo "── done"
ls -la "${work}"/*.pkg.tar.*
