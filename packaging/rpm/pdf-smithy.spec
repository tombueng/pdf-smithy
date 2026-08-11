# RPM spec for PDF Smithy, written against Fedora's CMake and Qt macros. It
# also builds on RHEL and its rebuilds once EPEL is enabled, because that is
# where kf6-* and poppler-qt6-devel come from there.
#
# Built by the rpm job in .github/workflows/release.yml inside a Fedora
# container. To build it by hand from a checkout:
#
#   packaging/rpm/build-rpm.sh
#
# which makes the tarball this file expects and hands the whole thing to
# rpmbuild.

%global appid io.github.tombueng.PdfSmithy

Name:           pdf-smithy
Version:        0.1.0
Release:        1%{?dist}
Summary:        Native PDF editor that keeps your documents on your machine

# Both programs in this package link Poppler, which is GPL, so the binaries are
# GPL-3.0-or-later whatever the licence of the sources that went into them: the
# permissively licensed engine under src/core is MIT, it is linked statically
# into them, and the combination is governed by the GPL. Claiming MIT for a
# binary that loads libpoppler would be untrue, and this field describes the
# binary that is actually shipped. LICENSING.md sets out the whole arrangement.
#
# The other two entries are files rather than code: the application icon is
# CC-BY-SA-4.0 and the AppStream metadata is CC0-1.0, and both are installed by
# this package, so both are named.
License:        GPL-3.0-or-later AND CC-BY-SA-4.0 AND CC0-1.0
URL:            https://github.com/tombueng/pdf-smithy
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  extra-cmake-modules
BuildRequires:  gettext
BuildRequires:  pkgconf-pkg-config

BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6Concurrent)
BuildRequires:  cmake(Qt6PrintSupport)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  qt6-qttools-devel

BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-ki18n-devel
BuildRequires:  kf6-kxmlgui-devel
BuildRequires:  kf6-kconfig-devel
BuildRequires:  kf6-kconfigwidgets-devel
BuildRequires:  kf6-kio-devel
BuildRequires:  kf6-kcrash-devel
BuildRequires:  kf6-kiconthemes-devel
BuildRequires:  kf6-kwidgetsaddons-devel
BuildRequires:  kf6-kguiaddons-devel
# Without it the handbook is not built and F1 opens nothing.
BuildRequires:  kf6-kdoctools-devel

BuildRequires:  qpdf-devel
BuildRequires:  poppler-qt6-devel
BuildRequires:  tesseract-devel
BuildRequires:  leptonica-devel
# Optional in CMake, named here on purpose: without it the colour tools hand
# every ICC conversion to Ghostscript instead of doing it in process, and a
# package that quietly loses a feature is worse than one that fails to build.
BuildRequires:  lcms2-devel

BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

# What the suite needs in order to run its cases rather than skip them. Nearly
# every one of these tests reports itself as passing when its tool is absent,
# so a shorter list here would turn a real suite into a green tick.
#
#   Xvfb              the end-to-end tests drive a real window
#   ghostscript       the scan fixtures are rasterised with gs, and the
#                     compression and PDF/A cases shell out to it
#   urw-base35-fonts  gs needs a Helvetica to draw those fixtures with
#   tesseract-langpack-*  with no language data the whole OCR suite skips
#   appstream, libxml2, python3, docbook-dtds  back the metadata check, the
#                     handbook check and the four Python guards
#   glibc-langpack-de every C++ test is registered twice, once under LC_ALL=C
#                     and once under de_DE.UTF-8, and asking for a locale that
#                     does not exist is not an error: the C locale applies
#                     instead and half the suite silently tests nothing
BuildRequires:  xorg-x11-server-Xvfb
BuildRequires:  ghostscript
BuildRequires:  urw-base35-fonts
BuildRequires:  tesseract-langpack-eng
BuildRequires:  tesseract-langpack-deu
BuildRequires:  appstream
BuildRequires:  libxml2
BuildRequires:  python3
BuildRequires:  docbook-dtds
BuildRequires:  glibc-langpack-de

# Both are run as subprocesses rather than linked, so nothing in the build
# discovers them and nothing but this line asks for them.
Recommends:     ghostscript
Recommends:     tesseract-langpack-eng
Suggests:       tesseract-langpack-deu
# A KDE application away from Plasma asks the icon theme for names such as
# "object-rotate-right" and gets nothing back, which shows up as an empty
# toolbar rather than as an error.
Recommends:     breeze-icon-theme

%description
PDF Smithy reorders, merges, splits, rotates, compresses, signs and OCRs PDF
documents locally, so confidential contracts and medical records never leave
the computer they are on.

Page operations are lossless: the document's object structure is edited rather
than its pages re-rendered, so a scanned contract keeps the exact image data it
arrived with. Text recognition adds an invisible layer over the scan instead of
replacing it, and crooked pages are straightened by transform rather than by
resampling.

Everything the window can do is also available from pdf-smithy-cli, so the same
operations can be scripted.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -GNinja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=ON
%cmake_build

%install
%cmake_install
# Puts every catalogue this package installs into %%{name}.lang, so a language
# added under po/ needs no edit here.
%find_lang %{name}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{appid}.desktop
appstream-util validate-relax --nonet \
    %{buildroot}%{_metainfodir}/%{appid}.metainfo.xml

# Under Xvfb even though every test is handed QT_QPA_PLATFORM=offscreen: the
# plugin covers the widgets, and the display is here for anything underneath
# them that asks the X server a question anyway.
#
# --no-tests=error rather than the default, so that a tests directory which
# stops being configured fails the build instead of passing an empty run.
xvfb-run -a --server-args="-screen 0 1400x900x24" \
    ctest --test-dir %{__cmake_builddir} --output-on-failure --no-tests=error

%files -f %{name}.lang
%doc README.md
# LICENSES/ is where the SPDX texts belong and is empty today, so the file that
# actually states the terms is named instead. Once the texts are added this
# becomes "%%license LICENSES/*".
%license LICENSING.md
%{_bindir}/%{name}
%{_bindir}/%{name}-cli
%{_datadir}/applications/%{appid}.desktop
%{_metainfodir}/%{appid}.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/%{appid}.svg
%{_datadir}/kxmlgui5/%{name}/
%{_datadir}/doc/HTML/en/%{name}/
%{_mandir}/man1/%{name}.1*
%{_mandir}/man1/%{name}-cli.1*

%changelog
* Mon Jul 27 2026 Tom Bueng <tombueng@gmail.com> - 0.1.0-1
- Initial package.
