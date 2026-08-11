# Screenshot helper, built apart from the application.
#
# This is a stand-alone project on purpose. It is a documentation tool rather
# than a part of the program, nobody installs it, and adding it to the main
# build would make every developer pay for it on every configure. It links
# against the static libraries the main build has already produced, so what it
# photographs is the window in the tree beside it and not an installed copy
# that may be older.
#
# Configure it against the main build tree:
#
#   cmake -S tools/screenshots -B /somewhere/outside/the/repository -G Ninja
#   ninja -C /somewhere/outside/the/repository
#
# and point PS_BUILD_DIR elsewhere if the main build does not live in build/.

cmake_minimum_required(VERSION 3.22)

project(pdf-smithy-shot VERSION 0.1.0 LANGUAGES CXX)

set(QT_MIN_VERSION "6.4.0")
set(KF_MIN_VERSION "6.0.0")

set(PS_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../build" CACHE PATH
    "The main project's build tree, where the static libraries and config.h are")
set(PS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src" CACHE PATH
    "The main project's src directory, for the headers")

# ── Dependencies, the same ones the application itself needs ──────────────
# Static archives carry no record of what they were linked against, so
# everything the window uses has to be named again here.

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# QPDF reports every failure by throwing, and the KDE compiler settings turn
# exceptions off. Without this a damaged document ends the process instead of
# producing a message, which is the same reason src/CMakeLists.txt says it.
kde_enable_exceptions()

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui Concurrent Widgets PrintSupport)

find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS
    CoreAddons I18n XmlGui Config ConfigWidgets KIO Crash IconThemes WidgetsAddons GuiAddons)

find_package(PkgConfig REQUIRED)
pkg_check_modules(QPDF    REQUIRED IMPORTED_TARGET libqpdf)
pkg_check_modules(Poppler REQUIRED IMPORTED_TARGET poppler-qt6)

# Optional in the main build, and optional in the same way here: whether the
# archives actually need these symbols depends on how that build was
# configured, so they are looked for and linked when they are there.
pkg_check_modules(Tesseract IMPORTED_TARGET tesseract)
pkg_check_modules(Leptonica IMPORTED_TARGET lept)
pkg_check_modules(LCMS2     IMPORTED_TARGET lcms2)

# ── The libraries the main build left behind ──────────────────────────────

set(PS_STATIC_LIBS ps_gui ps_render ps_print ps_core)

foreach(_lib IN LISTS PS_STATIC_LIBS)
    set(_path "${PS_BUILD_DIR}/lib/lib${_lib}.a")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR
            "${_path} is missing, so there is nothing to photograph.\n"
            "This tool links against the libraries the main project produces; build that first:\n"
            "    cmake -S ${CMAKE_CURRENT_SOURCE_DIR}/../.. -B ${PS_BUILD_DIR} -G Ninja\n"
            "    ninja -C ${PS_BUILD_DIR}\n"
            "or point -DPS_BUILD_DIR= at the build tree that already has them.")
    endif()
    list(APPEND PS_STATIC_PATHS "${_path}")
endforeach()

add_executable(pdf-smithy-shot shot.cpp)

target_include_directories(pdf-smithy-shot PRIVATE
    ${PS_SOURCE_DIR}
    # config.h is generated rather than checked in, and it is generated here.
    ${PS_BUILD_DIR}/src
)

# The order of the four archives is the order the main build uses, and with
# static archives it is not decoration: the linker only takes from an archive
# what is still undefined when it reaches it, so a library must come before the
# one it calls into.
target_link_libraries(pdf-smithy-shot PRIVATE
    ${PS_STATIC_PATHS}

    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Core

    KF6::XmlGui
    KF6::CoreAddons
    KF6::I18n
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::ConfigWidgets
    KF6::KIOCore
    KF6::KIOWidgets
    KF6::JobWidgets
    KF6::Crash
    KF6::IconThemes
    KF6::WidgetsAddons
    KF6::GuiAddons

    PkgConfig::Poppler
    PkgConfig::QPDF
)

if(LCMS2_FOUND)
    target_link_libraries(pdf-smithy-shot PRIVATE PkgConfig::LCMS2)
endif()

if(Tesseract_FOUND AND Leptonica_FOUND)
    target_link_libraries(pdf-smithy-shot PRIVATE PkgConfig::Tesseract PkgConfig::Leptonica)
endif()

# Nothing is installed. The pictures are the product; the program that takes
# them is scaffolding and stays in whatever build tree made it.
