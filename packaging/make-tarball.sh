#!/bin/sh
# Writes a release tarball of the working tree.
#
#   packaging/make-tarball.sh [output.tar.gz]
#
# The archive unpacks into pdf-smithy-<version>/, which is the directory name
# the RPM spec and the PKGBUILD both expect, and the version comes from the one
# line in CMakeLists.txt that defines it, so the tarball cannot be named
# something the recipes then fail to find.
#
# This exists so that the RPM and Arch builds can be tried against a checkout
# rather than only against a published tag: on a tag the recipes fetch the
# tarball GitHub generates, which is byte-for-byte the same layout as this one.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
version=$(sed -n 's/^set(PS_VERSION *"\([^"]*\)".*/\1/p' "${root}/CMakeLists.txt")
if [ -z "${version}" ]; then
    echo "could not read PS_VERSION out of CMakeLists.txt" >&2
    exit 1
fi

prefix="pdf-smithy-${version}"
out=${1:-"${root}/${prefix}.tar.gz"}

# Everything that is generated, downloaded or private to one machine. The build
# directories are the obvious ones; testdata/ holds third-party scans fetched on
# demand and is not ours to redistribute.
cd "${root}"
tar -czf "${out}" \
    --transform "s,^\.,${prefix}," \
    --exclude=./.git \
    --exclude=./build \
    --exclude='./build-*' \
    --exclude=./.flatpak-builder \
    --exclude=./.cache \
    --exclude=./.claude \
    --exclude=./testdata \
    --exclude=./debian/.debhelper \
    --exclude=./debian/pdf-smithy \
    --exclude=./debian/files \
    --exclude='./debian/*.substvars' \
    --exclude='./debian/*.debhelper.log' \
    --exclude=./debian/debhelper-build-stamp \
    --exclude='./*.tar.gz' \
    --exclude=./compile_commands.json \
    .

echo "${out}"
