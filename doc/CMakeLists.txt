# The guard repeats the one in the top-level file on purpose. Documentation
# tooling is the first thing missing from a minimal build environment, and a
# person who adds another add_subdirectory(doc) somewhere should not be the one
# to discover that it only works when KDocTools happens to be installed.
if(NOT COMMAND kdoctools_create_handbook)
    message(STATUS "KDocTools was not found, so the handbook is neither built nor installed")
    return()
endif()

# The subdirectory has to carry the same name that KAboutData reports as the
# component, because that is what the Help menu turns into the help:/ address
# when F1 is pressed: a handbook installed under any other name is a handbook
# nobody can open. Taking it from PS_NAME keeps the promise made at the top of
# the project file, that renaming the application means changing those lines and
# nothing else.
kdoctools_create_handbook(index.docbook
    INSTALL_DESTINATION ${KDE_INSTALL_DOCBUNDLEDIR}/en
    SUBDIR ${PS_NAME}
)

# The pictures, which kdoctools_create_handbook does not carry: it globs
# doc/*.png and nothing below it, and these live in a directory of their own so
# that the README at the top of the tree can point at the same files the
# handbook uses. Installed under the name the <imagedata> elements give them,
# which is why the directory name is repeated here rather than flattened.
#
# How each one was taken is recorded in tools/screenshots/shots.json, and
# tools/screenshots/make-screenshots.py replays the lot after an interface
# change.
install(DIRECTORY screenshots
    DESTINATION ${KDE_INSTALL_DOCBUNDLEDIR}/en/${PS_NAME}
    FILES_MATCHING PATTERN "*.png"
)
