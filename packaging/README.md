# Packaging

Three ways to ship PDF Smithy, in the order they are supported.

| Artefact | Recipe | Built by |
| --- | --- | --- |
| Debian package | `debian/` | the `package` job in `.github/workflows/ci.yml`, on every push |
| Flatpak bundle | `packaging/flatpak/io.github.tombueng.PdfSmithy.yml` | the `flatpak` job in `.github/workflows/release.yml` |
| AppImage | `packaging/appimage/build-appimage.sh` | the `appimage` job in `.github/workflows/release.yml` |

The bundles live in `release.yml` rather than `ci.yml` because between them they
download a KDE runtime and compile QPDF, Poppler, Leptonica, Tesseract and
Ghostscript from source. That is most of an hour of machine time which says
nothing about a change to a dialog, so it runs on a version tag, on a pull
request that touches `packaging/`, once a week, and on demand. The weekly run is
the point of the whole arrangement: these builds break from the outside — an
upstream tag moves, a runtime branch goes end of life, a linuxdeploy release
changes its mind — and finding that on a Monday is far cheaper than finding it on
a release day.

## Debian package

```sh
sudo apt-get install devscripts equivs
sudo mk-build-deps --install --remove --tool "apt-get -y --no-install-recommends" debian/control
dpkg-buildpackage -us -uc -b
```

The result lands one directory above the source tree. `debian/rules` runs the
test suite through `dh_auto_test`, so the German locale has to exist first or the
`-de` half of the suite tests nothing:

```sh
sudo locale-gen de_DE.UTF-8 && sudo update-locale
```

## Flatpak bundle

```sh
flatpak install flathub org.kde.Platform//6.10 org.kde.Sdk//6.10
flatpak-builder --user --install --force-clean build-flatpak \
    packaging/flatpak/io.github.tombueng.PdfSmithy.yml
flatpak run io.github.tombueng.PdfSmithy
```

To produce a single-file bundle the way CI does:

```sh
flatpak-builder --repo=build-flatpak-repo --force-clean build-flatpak \
    packaging/flatpak/io.github.tombueng.PdfSmithy.yml
flatpak build-bundle build-flatpak-repo \
    io.github.tombueng.PdfSmithy.flatpak io.github.tombueng.PdfSmithy
```

`flatpak-builder --show-manifest <manifest>` parses the file and prints what it
understood, which is the quickest way to check an edit without building.
`flatpak-builder --download-only <dir> <manifest>` goes one step further and
fetches every source, verifying each checksum, in a few minutes rather than an
hour.

What the manifest builds and why:

* **QPDF, Leptonica, Tesseract** — no runtime ships them.
* **Poppler** — the KDE runtime does not carry it either; Flathub's own Okular
  builds its own copy for the same reason. Built with `ENABLE_BOOST=OFF`, because
  Poppler's CMake treats a missing Boost as fatal even though it only uses it to
  speed up the Splash rasteriser, and there is no Boost in the KDE SDK.
* **Ghostscript** — image compression and PDF/A conversion run `gs` as a
  subprocess, and a sandboxed application cannot borrow the host's copy. Without
  it those two features fail with a message telling the user to install a package
  they cannot install from inside the sandbox.
* **Tessdata** — three pinned language files, so text recognition works out of
  the box.

The application module is a `dir` source: what gets built is what is checked out,
so a pull request that changes the manifest is tried against the code beside it.
The three git dependencies use tags, which need no checksum. Everything added
since is pinned by checksum, and the tessdata files additionally by commit rather
than by tag — a tag can be moved, a commit cannot.

`runtime-version` in the manifest and the container image in the `flatpak` job
name the same KDE runtime. Bumping one means bumping the other.

## AppImage

```sh
packaging/appimage/build-appimage.sh
```

Run it from the project root, on the **oldest** distribution you intend to
support. An AppImage bundles the libraries it was linked against but not glibc,
and glibc compatibility only runs forwards: a bundle built on Ubuntu 26.04 will
not start on 24.04, while one built on 24.04 runs on both. That is why the CI job
pins `ubuntu-24.04` instead of following `ubuntu-latest`, and it needs to keep
naming the oldest release the build matrix in `ci.yml` targets.

The script fetches `linuxdeploy` and its Qt plugin on each first run, builds the
project, stages it into an `AppDir`, copies in the Breeze icon theme if the
machine has one, and writes
`build-appimage/pdf-smithy-<version>-<arch>.AppImage` — inside the build
directory, which git already ignores. Two environment settings in it are worth
knowing about:
`APPIMAGE_EXTRACT_AND_RUN=1`, because the tools are themselves AppImages and
mounting one needs libfuse2, which current Ubuntu no longer ships; and the tools
directory on `PATH`, because that is how `linuxdeploy` finds its plugins once it
is being extracted rather than mounted.

Ghostscript and the Tesseract language data are deliberately not bundled. They
are large, they are packaged everywhere, and the application already says plainly
what to install when they are missing, which beats a 300 MB image for the sake of
one optional feature.

The AppImage is a best-effort convenience for distributions that package none of
this. It carries Qt and the Frameworks it was linked against, with three known
gaps: KIO's workers are not deployed, so network locations behave as they do
without KIO installed; only the XCB platform plugin is bundled, so a Wayland
session runs it through XWayland; and the handbook is only inside it when the
machine that built it had KF6 KDocTools, which Ubuntu 24.04 does not package. The
`.deb` and the Flatpak are the supported paths.

## Both, from a tag

Pushing a tag that starts with `v` builds both bundles and attaches them to the
GitHub release for that tag, creating the release if the tag was pushed without
one.
