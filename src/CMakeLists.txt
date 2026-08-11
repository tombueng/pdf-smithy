configure_file(config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h @ONLY)

# QPDF reports every failure by throwing, and KDECompilerSettings turns
# exceptions off by default. Without this, a damaged PDF terminates the process
# instead of producing an error message.
kde_enable_exceptions()

# ══ ps_core, MIT ══════════════════════════════════════════════════════════
# The engine. Links only against permissively licensed libraries so that it
# stays reusable outside this project. Deliberately knows nothing about
# Poppler; rendering arrives through the RenderBackend interface.
add_library(ps_core STATIC
    core/Source.cpp
    core/Document.cpp
    core/DocumentWriter.cpp
    core/RenderCache.cpp
    core/PageRange.cpp
    core/PdfGeometry.cpp
    core/Overlay.cpp
    core/PdfImage.cpp
    core/ImageIO.cpp
    core/PageNumbering.cpp
    core/PageCrop.cpp
    core/Annotation.cpp
    core/Outline.cpp
    core/Forms.cpp
    core/Compare.cpp
    core/Redaction.cpp
    core/FontEmbedder.cpp
    core/FontInventory.cpp
    core/ImageEdit.cpp
    core/ColourTools.cpp
    core/Preflight.cpp
    core/FormBuilder.cpp
    core/PageLayout.cpp
    core/Convert.cpp
    core/Typeset.cpp
    core/PageObjects.cpp
    core/TextEdit.cpp
    core/NUp.cpp
    core/PdfFile.cpp
    core/Metadata.cpp
    core/Encryption.cpp
    core/Splitter.cpp
    core/BlankPdf.cpp
    core/Compressor.cpp
    core/Archival.cpp
    core/commands/PageCommands.cpp
)

target_include_directories(ps_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(ps_core PUBLIC
    Qt6::Core
    Qt6::Gui          # QImage, QUndoStack
    Qt6::Concurrent
    KF6::I18n
    PkgConfig::QPDF
)

set_target_properties(ps_core PROPERTIES
    AUTOMOC ON
    POSITION_INDEPENDENT_CODE ON
)

# Colour management. LittleCMS is MIT, so it keeps ps_core permissive, and it is
# the only way an ICC conversion into CMYK can happen in process rather than by
# handing the whole document to Ghostscript. Optional on purpose: without it the
# colour tools fall back and say which route they took.
pkg_check_modules(LCMS2 IMPORTED_TARGET lcms2)
if(LCMS2_FOUND)
    target_link_libraries(ps_core PUBLIC PkgConfig::LCMS2)
    target_compile_definitions(ps_core PRIVATE PS_WITH_LCMS=1)
endif()

# OCR and scan straightening. Tesseract is Apache-2.0 and Leptonica is
# BSD/MIT, so linking them keeps ps_core permissively licensed.
if(PS_WITH_OCR)
    target_sources(ps_core PRIVATE core/ScanProcessor.cpp)
    target_link_libraries(ps_core PUBLIC PkgConfig::Tesseract PkgConfig::Leptonica)
    target_compile_definitions(ps_core PUBLIC PS_WITH_OCR=1)
endif()

# ══ ps_render, GPL-3.0-or-later ═══════════════════════════════════════════
# The single Poppler-linked target. Isolating it here is what keeps the GPL
# obligation off ps_core and leaves the door open to a permissive engine.
add_library(ps_render STATIC
    render/PopplerBackend.cpp
)

target_link_libraries(ps_render PUBLIC
    ps_core
    PkgConfig::Poppler
)

set_target_properties(ps_render PROPERTIES
    AUTOMOC ON
    POSITION_INDEPENDENT_CODE ON
)

# ══ ps_print, GPL-3.0-or-later ════════════════════════════════════════════
# Imposition and printing. Separate from the GUI so the imposition logic can be
# unit-tested without a window or a printer.
if(TARGET Qt6::PrintSupport)
    add_library(ps_print STATIC print/PrintController.cpp)
    set_target_properties(ps_print PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(ps_print PUBLIC ps_core Qt6::PrintSupport Qt6::Gui)
endif()

# ══ The application ═══════════════════════════════════════════════════════
if(PS_BUILD_GUI AND TARGET Qt6::Widgets AND TARGET KF6::XmlGui)
    # The window lives in a library rather than straight in the executable, so
    # the end-to-end tests can drive the real thing instead of a stand-in.
    add_library(ps_gui STATIC
        ui/MainWindow.cpp
        ui/PageModel.cpp
        ui/PageDelegate.cpp
        ui/PageGridView.cpp
        ui/PrintOptionsWidget.cpp
        ui/PageProcessor.cpp
        ui/OperationDialogs.cpp
        ui/DocumentCommands.cpp
        ui/DocumentProperties.cpp
        ui/SignatureStore.cpp
        ui/PagePlacementView.cpp
        ui/RemoteFile.cpp
        ui/DocumentDialogs.cpp
        ui/PageToolDialogs.cpp
        ui/PageCanvas.cpp
        ui/OutlineDock.cpp
        ui/CompareDialog.cpp
        ui/RedactionView.cpp
    # The editing tools. One translation unit each, so that adding a tool does
    # not mean editing the window.
        ui/EditorMode.cpp
        ui/RedactEditor.cpp
        ui/FormEditor.cpp
        ui/FormFieldView.cpp
        ui/SignEditor.cpp
        ui/SignatureCanvas.cpp
        ui/CropEditor.cpp
        ui/CropView.cpp
        ui/WatermarkEditor.cpp
        ui/WatermarkView.cpp
        ui/NumberEditor.cpp
        ui/PageView.cpp
        ui/PageClipboard.cpp
        ui/Alignment.cpp
        ui/AlignmentPanel.cpp
        ui/FormDesignOverlay.cpp
        ui/ThumbnailDock.cpp
        ui/PageNavigation.cpp
        ui/InspectorDock.cpp
        ui/FormOverlay.cpp
        ui/TextOverlay.cpp
        ui/ObjectOverlay.cpp
        ui/AnnotationOverlay.cpp
        ui/ToolSupport.cpp
        ui/ToolPreflightDialog.cpp
        ui/ToolColourDialog.cpp
        ui/ToolFontsDialog.cpp
        ui/ToolImagesDialog.cpp
        ui/ToolLayoutDialog.cpp
        ui/ToolFormBuilderDialog.cpp
        ui/ToolObjectsDialog.cpp
        ui/ToolTypesetDialog.cpp
        resources.qrc
    )

    set_target_properties(ps_gui PROPERTIES AUTOMOC ON AUTORCC ON POSITION_INDEPENDENT_CODE ON)

    target_link_libraries(ps_gui PUBLIC
        ps_core
        ps_render
        ps_print
        Qt6::Widgets
        KF6::CoreAddons   # KAboutData
        KF6::XmlGui
        KF6::ConfigCore
        KF6::ConfigGui
        KF6::ConfigWidgets
        KF6::KIOCore       # opening documents on network shares
        KF6::KIOWidgets
        KF6::JobWidgets
        KF6::Crash
        KF6::IconThemes
        KF6::WidgetsAddons
        KF6::GuiAddons
    )

    add_executable(${PS_NAME} main.cpp)
    set_target_properties(${PS_NAME} PROPERTIES AUTOMOC ON)
    target_link_libraries(${PS_NAME} PRIVATE ps_gui)

    install(TARGETS ${PS_NAME} ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
    install(FILES pdf-smithyui.rc DESTINATION ${KDE_INSTALL_KXMLGUIDIR}/${PS_NAME})
endif()

# ══ Command-line companion ════════════════════════════════════════════════
# Same engine, no window. Every document operation the GUI offers has to be
# reachable from a script; that is a design rule, not an extra.
if(PS_BUILD_CLI)
    # One translation unit per area of the command line. Kept apart so that
    # adding a command touches one file rather than the file everything is in.
    add_executable(${PS_NAME}-cli
        cli/main.cpp
        cli/Common.cpp
        cli/CommandsColour.cpp
        cli/CommandsFonts.cpp
        cli/CommandsImages.cpp
        cli/CommandsLayout.cpp
        cli/CommandsPreflight.cpp
        cli/CommandsForm.cpp
        cli/CommandsTypeset.cpp
        cli/CommandsObjects.cpp
        cli/CommandsConvert.cpp
    )
    set_target_properties(${PS_NAME}-cli PROPERTIES AUTOMOC ON)
    target_link_libraries(${PS_NAME}-cli PRIVATE ps_core ps_render KF6::CoreAddons)
    install(TARGETS ${PS_NAME}-cli ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
endif()
