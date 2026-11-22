# Copies the bundled MIDI files next to the app, once per version: there are thousands, too many to copy on every link.
#   cmake -DSOURCE=<dir> -DDEST=<dir> -DVERSION=<version> -P CopyMidiFiles.cmake

set(_marker "${DEST}/.version")

if(EXISTS "${_marker}")
    file(READ "${_marker}" _copied)

    if(_copied STREQUAL VERSION)
        return()
    endif()
endif()

file(REMOVE_RECURSE "${DEST}")
file(MAKE_DIRECTORY "${DEST}")
file(COPY "${SOURCE}/" DESTINATION "${DEST}/Free MIDI Chords")
file(WRITE "${_marker}" "${VERSION}")
