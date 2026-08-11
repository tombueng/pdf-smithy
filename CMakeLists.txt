cmake_minimum_required(VERSION 3.22)

# ══════════════════════════════════════════════════════════════════════════
#  Project identity. Renaming the application means changing these lines and
#  nothing else — binary, .desktop file, AppStream metainfo, D-Bus name and
#  C++ constants are all derived from them.
#
#  PS_APPID must be a valid D-Bus name: dot-separated elements of [A-Za-z0-9_]
#  only. Hyphens are formally permitted by Flatpak but break other consumers,
#  hence "PdfSmithy" rather than "pdf-smithy" here.
# ══════════════════════════════════════════════════════════════════════════
set(PS_NAME    "pdf-smithy"                   CACHE STRING "Binary and repository name")
set(PS_DISPLAY "PDF Smithy"                   CACHE STRING "Human-facing application name")
set(PS_APPID   "io.github.tombueng.PdfSmithy" CACHE STRING "AppStream / D-Bus identifier")
set(PS_VERSION "0.1.0"                        CACHE STRING "Release version")

project(${PS_NAME} VERSION ${PS_VERSION} LANGUAGES CXX)

set(QT_MIN_VERSION "6.4.0")
set(KF_MIN_VERSION "6.0.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMSetupVersion)
include(ECMQtDeclareLoggingCategory)
include(FeatureSummary)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ───────────────────────────────────────────────────────────────
option(PS_BUILD_GUI "Build the graphical application" ON)
option(PS_BUILD_CLI "Build the command-line companion" ON)
option(PS_WITH_OCR  "Enable OCR and scan cleanup (Tesseract + Leptonica)" ON)

# ── Dependencies ──────────────────────────────────────────────────────────
find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui Concurrent)
find_package(Qt6 ${QT_MIN_VERSION} OPTIONAL_COMPONENTS Widgets PrintSupport Test)

find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS CoreAddons I18n)
find_package(KF6 ${KF_MIN_VERSION} OPTIONAL_COMPONENTS
    XmlGui Config ConfigWidgets KIO Crash IconThemes WidgetsAddons GuiAddons)

find_package(KF6DocTools ${KF_MIN_VERSION})

find_package(PkgConfig REQUIRED)
pkg_check_modules(QPDF    REQUIRED IMPORTED_TARGET libqpdf)
pkg_check_modules(Poppler REQUIRED IMPORTED_TARGET poppler-qt6)

if(PS_WITH_OCR)
    pkg_check_modules(Tesseract IMPORTED_TARGET tesseract)
    pkg_check_modules(Leptonica IMPORTED_TARGET lept)
    if(NOT Tesseract_FOUND OR NOT Leptonica_FOUND)
        message(WARNING "Tesseract/Leptonica not found — building without OCR")
        set(PS_WITH_OCR OFF)
    endif()
endif()

set_package_properties(QPDF PROPERTIES
    TYPE REQUIRED PURPOSE "Lossless PDF structure manipulation")
set_package_properties(Poppler PROPERTIES
    TYPE REQUIRED PURPOSE "Page rendering and text extraction")

set_package_properties(KF6DocTools PROPERTIES
    TYPE OPTIONAL PURPOSE "Turns doc/index.docbook into the handbook that F1 opens")

add_subdirectory(src)

# Without the tooling there is no handbook, and pressing F1 finds nothing. That
# is a worse program, not a broken build, so the documentation is left out
# rather than being allowed to stop somebody compiling on a minimal system.
if(KF6DocTools_FOUND)
    add_subdirectory(doc)
endif()

if(BUILD_TESTING AND Qt6Test_FOUND)
    add_subdirectory(tests)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/data/CMakeLists.txt)
    add_subdirectory(data)
endif()

# Translations are installed only once they exist, so a fresh checkout builds
# before anyone has run the extraction script.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/po)
    ki18n_install(po)
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
