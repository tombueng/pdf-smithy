# Application icon, installed under the freedesktop naming scheme so that
# Plasma, GNOME and the .desktop file all find it by the same id.
install(FILES icons/scalable/apps/${PS_APPID}.svg
        DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps)

configure_file(app.desktop.in  ${CMAKE_CURRENT_BINARY_DIR}/${PS_APPID}.desktop     @ONLY)
configure_file(app.metainfo.in ${CMAKE_CURRENT_BINARY_DIR}/${PS_APPID}.metainfo.xml @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${PS_APPID}.desktop
        DESTINATION ${KDE_INSTALL_APPDIR})

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${PS_APPID}.metainfo.xml
        DESTINATION ${KDE_INSTALL_METAINFODIR})

# Man pages. The command table in the CLI page is generated from the program's
# own help output by tools/generate-manpages.py, and a test checks the two have
# not drifted apart — a manual that lists commands the program does not have is
# worse than none.
foreach(_page pdf-smithy pdf-smithy-cli)
    configure_file(${CMAKE_SOURCE_DIR}/docs/${_page}.1.in
                   ${CMAKE_CURRENT_BINARY_DIR}/${_page}.1 @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${_page}.1
            DESTINATION ${KDE_INSTALL_MANDIR}/man1)
endforeach()

# Validates the AppStream metadata as part of the test suite, so a broken
# metainfo file fails CI instead of Flathub review.
if(BUILD_TESTING)
    find_program(APPSTREAMCLI appstreamcli)
    if(APPSTREAMCLI)
        add_test(NAME metainfo-valid
                 COMMAND ${APPSTREAMCLI} validate --no-net --explain
                         ${CMAKE_CURRENT_BINARY_DIR}/${PS_APPID}.metainfo.xml)
    endif()
endif()
