#!/bin/sh
# Builds a self-contained AppImage of PDF Smithy.
#
#   packaging/appimage/build-appimage.sh
#
# Run it from the project root on the *oldest* distribution you intend to
# support — an AppImage bundles the libraries it was linked against, and glibc
# only ever works forwards. Building on Ubuntu 24.04 produces something that
# runs on 24.04 and later; building on 26.04 does not run on 24.04. That is why
# the appimage job in .github/workflows/release.yml pins ubuntu-24.04 rather
# than following ubuntu-latest, and why this is the script that job runs: there
# is one recipe, and it is this one.
#
# Ghostscript and the Tesseract language data are deliberately not bundled — see
# the note further down. The AppImage is a convenience for distributions that
# package none of this; the .deb and the Flatpak are the supported paths.
set -eu

root=$(pwd)
work="${root}/build-appimage"
appdir="${work}/AppDir"
tools="${work}/tools"

mkdir -p "${tools}"

# linuxdeploy finds its plugins by name on PATH. It would also find them beside
# its own binary, except that with APPIMAGE_EXTRACT_AND_RUN set "beside its own
# binary" is a temporary extraction directory, so PATH is the only way that
# keeps working.
PATH="${tools}:${PATH}"
export PATH

# The tools are themselves AppImages, and mounting one needs libfuse2, which
# neither Ubuntu 24.04 nor the GitHub runners ship any more. Unpacking each tool
# instead of mounting it costs a few seconds and works everywhere.
APPIMAGE_EXTRACT_AND_RUN=1
export APPIMAGE_EXTRACT_AND_RUN

# appimagetool works the architecture out for itself most of the time, and when
# it cannot it says so at the very last step, after everything has been built.
# Spelling it out costs nothing and removes the possibility.
ARCH=$(uname -m)
export ARCH

fetch() {
    if [ ! -x "${tools}/$1" ]; then
        echo "── fetching $1"
        curl -sSL -o "${tools}/$1" "$2"
        chmod +x "${tools}/$1"
    fi
}

fetch linuxdeploy \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch linuxdeploy-plugin-qt \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage

# The Qt plugin locates Qt through qmake and says nothing useful when it cannot,
# so it is checked here where the message can name the package to install.
qmake=$(command -v qmake6 || command -v qmake || true)
if [ -z "${qmake}" ]; then
    echo "qmake6 was not found — install qt6-base-dev" >&2
    exit 1
fi
QMAKE="${qmake}"
export QMAKE

echo "── configuring"
cmake -S "${root}" -B "${work}/cmake" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF

echo "── building"
cmake --build "${work}/cmake"

echo "── staging"
rm -rf "${appdir}"
DESTDIR="${appdir}" cmake --install "${work}/cmake"

# Breeze, when the machine building this has it. The application asks the icon
# theme for names of its own — "document-save", "object-rotate-right" — and away
# from Plasma nothing answers them: the window comes up with an empty toolbar.
# Thirty megabytes buys every icon in the interface. linuxdeploy's AppRun puts
# the bundle's share directory on XDG_DATA_DIRS, which is what makes a theme
# dropped in here visible to the icon loader.
if [ -d /usr/share/icons/breeze ]; then
    echo "── adding the Breeze icon theme"
    mkdir -p "${appdir}/usr/share/icons"
    cp -r /usr/share/icons/breeze "${appdir}/usr/share/icons/"
else
    echo "── no Breeze icon theme here; the bundle will rely on the host's"
fi

# Ghostscript and the Tesseract language data are deliberately *not* bundled.
# They are large, they are packaged everywhere, and the application already
# says plainly what to install when they are missing — which is better than
# shipping a 300 MB image so that one optional feature works.
#
# The name is fixed rather than left to appimagetool, which derives it from the
# desktop file and has changed its mind about the format before now. The version
# is read from the same line the build reads it from, so the file name cannot
# disagree with what the program reports. It lands in the build directory rather
# than in the project root because that directory is already ignored by git, and
# a 100 MB file appearing next to the source is the sort of thing that ends up
# committed by accident.
echo "── bundling"
version=$(sed -n 's/^set(PS_VERSION *"\([^"]*\)".*/\1/p' "${root}/CMakeLists.txt")
: "${version:=0.0.0}"
OUTPUT="${work}/pdf-smithy-${version}-${ARCH}.AppImage"
export OUTPUT

QML_SOURCES_PATHS="" \
"${tools}/linuxdeploy" \
    --appdir "${appdir}" \
    --plugin qt \
    --desktop-file "${appdir}/usr/share/applications/io.github.tombueng.PdfSmithy.desktop" \
    --icon-file "${appdir}/usr/share/icons/hicolor/scalable/apps/io.github.tombueng.PdfSmithy.svg" \
    --output appimage

echo
echo "── done"
ls -la "${OUTPUT}" 2>/dev/null || {
    echo "no AppImage was produced — check the output above" >&2
    exit 1
}
