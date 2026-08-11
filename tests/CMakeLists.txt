# The fixture builder talks to QPDF directly, so it needs exceptions too.
kde_enable_exceptions()

add_library(ps_testsupport STATIC TestPdf.cpp)
target_link_libraries(ps_testsupport PUBLIC ps_core Qt6::Test)
target_include_directories(ps_testsupport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set(PS_CORE_TESTS
    tst_document
    tst_pagecommands
    tst_documentwriter
    tst_pagerange
    tst_splitter
    tst_compressor
    tst_documentproperties
    tst_pdfgeometry
    tst_typeset
    tst_pagelayout
    tst_formbuilder
    tst_preflight
    tst_fonts
    tst_archival
    tst_encryption
    tst_blankpdf
)

# These need the Poppler backend to look at what was produced, so they are
# listed apart from the pure-core tests.
list(APPEND PS_RENDER_TESTS tst_overlay tst_imageio tst_pagetools tst_redaction tst_nup tst_annotation
                            tst_outline tst_forms tst_textedit tst_compare tst_imageedit tst_colour tst_convert tst_pageobjects
                            tst_render)
if(PS_WITH_OCR)
    list(APPEND PS_RENDER_TESTS tst_scanprocessor)
endif()

if(TARGET ps_print)
    list(APPEND PS_PRINT_TESTS tst_printcontroller)
endif()

# Drives the real window. Runs headless through the offscreen platform plugin,
# so it behaves the same on a developer machine and in CI.
if(TARGET ps_gui)
    # tst_alignment needs no window, but the arithmetic it drives lives beside
    # the interface that uses it, so it links against the same library.
    list(APPEND PS_GUI_TESTS tst_mainwindow tst_pageprocessor tst_tools tst_documentview tst_alignment tst_workmode
                             tst_overlayrows tst_pageturn tst_pastedobjects tst_thumbnaildock
                             tst_documentproperties_panel tst_textwysiwyg tst_formwysiwyg tst_liftedobjects)
endif()

foreach(_test ${PS_CORE_TESTS} ${PS_RENDER_TESTS} ${PS_PRINT_TESTS} ${PS_GUI_TESTS})
    add_executable(${_test} ${_test}.cpp)
    target_link_libraries(${_test} PRIVATE ps_core ps_testsupport Qt6::Test)
    if(_test IN_LIST PS_RENDER_TESTS)
        target_link_libraries(${_test} PRIVATE ps_render)
    endif()
    if(_test IN_LIST PS_PRINT_TESTS)
        target_link_libraries(${_test} PRIVATE ps_print ps_render)
    endif()
    if(_test IN_LIST PS_GUI_TESTS)
        target_link_libraries(${_test} PRIVATE ps_gui)
    endif()
    set_target_properties(${_test} PROPERTIES AUTOMOC ON)
    add_test(NAME ${_test} COMMAND ${_test})
    # Keep tests off the developer's real configuration and translations.
    set_tests_properties(${_test} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LC_ALL=C")

    # And again where the decimal separator is a comma. PDF numbers are written
    # and read in the C locale; anything that goes through strtod or snprintf
    # instead reads every fractional number as zero on most of Europe's
    # desktops. Running only under LC_ALL=C is what let that reach a release
    # once: an A4 page measured 595 x 841 and a yellow highlight came out red.
    add_test(NAME ${_test}-de COMMAND ${_test})
    set_tests_properties(${_test}-de PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LC_ALL=de_DE.UTF-8")
endforeach()

# The command line as a whole: run as a process, because what is being checked
# (option names claimed twice, verbs the help promises but the dispatcher does
# not know) only exists once the areas are linked together into one binary.
if(TARGET ${PS_NAME}-cli)
    add_executable(tst_cli tst_cli.cpp)
    target_link_libraries(tst_cli PRIVATE ps_core ps_testsupport Qt6::Test)
    set_target_properties(tst_cli PROPERTIES AUTOMOC ON)
    add_dependencies(tst_cli ${PS_NAME}-cli)
    foreach(_locale C de_DE.UTF-8)
        if(_locale STREQUAL C)
            set(_name tst_cli)
        else()
            set(_name tst_cli-de)
        endif()
        add_test(NAME ${_name} COMMAND tst_cli)
        set_tests_properties(${_name} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LC_ALL=${_locale}" TIMEOUT 180)
    endforeach()
endif()

# Ceilings, so a hang shows up as a failed test rather than a stuck CI job.
# The end-to-end test builds and tears down a whole window per case; the scan
# tests rasterise fixtures and run real OCR over them.
if(TARGET ps_gui)
    set_tests_properties(tst_mainwindow PROPERTIES TIMEOUT 120)
    # The tool windows are modal; the test answers them from a timer and has a
    # watchdog of its own. This is the backstop below that, so a tool that ever
    # blocks outside the event loop fails the run instead of wedging it.
    set_tests_properties(tst_tools PROPERTIES TIMEOUT 120)
    set_tests_properties(tst_tools-de PROPERTIES TIMEOUT 120)
    # It builds and tears down a window per registered action, which is slow
    # even though each one is quick.
    set_tests_properties(tst_documentview PROPERTIES TIMEOUT 180)
    set_tests_properties(tst_documentview-de PROPERTIES TIMEOUT 180)
    set_tests_properties(tst_workmode PROPERTIES TIMEOUT 120)
    set_tests_properties(tst_workmode-de PROPERTIES TIMEOUT 120)
endif()
if(PS_WITH_OCR)
    set_tests_properties(tst_scanprocessor PROPERTIES TIMEOUT 300)
    if(TARGET ps_gui)
        set_tests_properties(tst_pageprocessor PROPERTIES TIMEOUT 300)
    endif()
endif()

# Text wrapped in tr() or QCoreApplication::translate compiles and runs, but
# Messages.sh never sees it, so it stays English in every language. Nothing
# else catches that, because the build is clean and the catalogue reports
# itself complete, so it is checked here.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME translatable-strings
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check-translatable.py)

    # Two actions wanting one shortcut makes KXmlGuiWindow open a modal box
    # while the window is still being built: it greets the user at startup and
    # hangs the end-to-end test before it can say why. Checked from the source,
    # so the guard works even when the window will not come up.
    add_test(NAME unique-shortcuts
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check-shortcuts.py)

    # KXmlGui drops an unknown menu entry and hides an unlisted action without
    # a word either way. The window comes up looking fine and one command is
    # simply gone, which is the sort of thing only a user finds.
    add_test(NAME actions-reachable
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check-actions.py)

    # A manual that lists a command the program does not have is worse than no
    # manual: it makes people doubt the parts that are still right.
    if(PS_BUILD_CLI)
        add_test(NAME manpage-current
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check-manpage.py
                         $<TARGET_FILE:${PS_NAME}-cli>)
    endif()

    # The handbook is only compiled where KDocTools is installed, which is not
    # every machine and not every contributor's, so a mistyped tag can sit in the
    # repository until it fails somebody else's build or, worse, until a user
    # presses F1. This is deliberately not conditional on KDocTools: the check
    # needs xmllint at most, and it skips itself when even that is absent.
    add_test(NAME handbook-valid
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check-docbook.py)
endif()
