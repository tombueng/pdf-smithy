#!/bin/sh
# Builds the RPM from a checkout.
#
#   packaging/rpm/build-rpm.sh
#
# Run it on Fedora, or on anything with rpmbuild and the build requirements the
# spec lists. The packages to install first are exactly what the spec asks for,
# and dnf will read them out of it:
#
#   sudo dnf install rpm-build 'dnf5-command(builddep)'
#   sudo dnf builddep packaging/rpm/pdf-smithy.spec
#
# The result lands in build-rpm/RPMS/. The spec's Source0 points at the tarball
# GitHub generates for a tag; this script makes the same tarball out of the
# working tree instead, so a change to the recipe can be tried before the tag
# that would publish it exists.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
work="${root}/build-rpm"
version=$(sed -n 's/^set(PS_VERSION *"\([^"]*\)".*/\1/p' "${root}/CMakeLists.txt")

# The spec carries the version too, and a mismatch produces a package built
# from one tree and named after another. Checked here rather than left to be
# noticed later.
"${root}/packaging/check-versions.sh"

mkdir -p "${work}/SOURCES" "${work}/SPECS"
"${root}/packaging/make-tarball.sh" "${work}/SOURCES/pdf-smithy-${version}.tar.gz" > /dev/null
cp "${root}/packaging/rpm/pdf-smithy.spec" "${work}/SPECS/"

rpmbuild -ba \
    --define "_topdir ${work}" \
    "${work}/SPECS/pdf-smithy.spec"

echo
echo "── done"
find "${work}/RPMS" -name '*.rpm'
