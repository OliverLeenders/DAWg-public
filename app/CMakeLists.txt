# Update checks fetch over HTTPS, which JUCE does through libcurl on Linux (docs/design/update-checks.md).
if(UNIX AND NOT APPLE)
    set(DAWG_NEEDS_CURL TRUE)
else()
    set(DAWG_NEEDS_CURL FALSE)
endif()

juce_add_gui_app(DAWg
    PRODUCT_NAME "DAWg"
    COMPANY_NAME "DAWg"
    BUNDLE_ID "org.dawg.app"
    # The executable's, window's and taskbar's icon (branding/dawg-icon.svg, rendered).
    ICON_BIG "${CMAKE_SOURCE_DIR}/branding/dawg-icon-1024.png"
    ICON_SMALL "${CMAKE_SOURCE_DIR}/branding/dawg-icon-256.png"
    VERSION ${PROJECT_VERSION}
    NEEDS_WEBVIEW2 TRUE
    NEEDS_WEB_BROWSER TRUE
    NEEDS_CURL ${DAWG_NEEDS_CURL}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "DAWg needs audio input access to record.")

target_sources(DAWg PRIVATE
    src/AudioLibrary.cpp
    src/ClapEditor.cpp
    src/Main.cpp
    src/AutomationControl.cpp
    src/ComponentControl.cpp
    src/DesktopCheck.cpp
    src/ExportControl.cpp
    src/FaustControl.cpp
    src/MainComponent.cpp
    src/MidiControl.cpp
    src/MidiInputHub.cpp
    src/MixerWindow.cpp
    src/PianoRollWindow.cpp
    src/PanelControl.cpp
    src/Bridge.cpp
    src/PluginHost.cpp
    src/PluginScanner.cpp
    src/PluginWindows.cpp
    src/ProjectManager.cpp
    src/ScanWorker.cpp
    src/UiResources.cpp
    src/UpdateControl.cpp
    src/UiRecovery.cpp
    src/AppLog.cpp
    src/WindowChrome.cpp)

dawg_add_engine(DAWg)
dawg_set_common_flags(DAWg)

target_compile_definitions(DAWg PRIVATE
    # HTTPS for update checks (docs/design/update-checks.md): libcurl on Linux (NEEDS_CURL above), loaded when first
    # used.
    JUCE_LOAD_CURL_SYMBOLS_LAZILY=1
    JUCE_USE_WIN_WEBVIEW2=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:DAWg,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:DAWg,JUCE_VERSION>"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=$<BOOL:${DAWG_ENABLE_VST2}>
    JUCE_PLUGINHOST_AU=$<BOOL:${APPLE}>
    JUCE_PLUGINHOST_LV2=$<BOOL:${UNIX}>
    JUCE_PLUGINHOST_LADSPA=0
    # ASIO drivers on Windows, with the ASIO SDK that JUCE bundles (Steinberg's GPLv3 option, which AGPLv3 code may use).
    JUCE_ASIO=$<BOOL:${WIN32}>
    DAWG_ENABLE_VST2=$<BOOL:${DAWG_ENABLE_VST2}>)

# JUCE exports the app's symbols on Linux so that its web view's child process finds its entry point. That exported
# the LLVM DAWg links statically (for Faust) too, and the child's GL driver (Mesa, with its own LLVM) then called into
# it and crashed, so the UI never appeared. Symbols from static libraries stay inside the executable.
if(UNIX AND NOT APPLE)
    target_link_options(DAWg PRIVATE "LINKER:--exclude-libs,ALL")
endif()

target_link_libraries(DAWg PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_audio_processors
    juce::juce_gui_extra
    juce::juce_recommended_lto_flags)

# ---------------------------------------------------------------------------
# Web UI: built with npm and copied next to the executable (or into the macOS
# bundle's Resources). The app serves it through WebBrowserComponent's
# resource provider. Run with --dev to use the Vite dev server instead.
# ---------------------------------------------------------------------------
set(DAWG_UI_DIR "${CMAKE_SOURCE_DIR}/ui")

if(APPLE)
    set(DAWG_UI_DEST "$<TARGET_BUNDLE_CONTENT_DIR:DAWg>/Resources/ui")
else()
    set(DAWG_UI_DEST "$<TARGET_FILE_DIR:DAWg>/ui")
endif()

if(DAWG_BUILD_UI)
    # npm.cmd only on Windows: under WSL, Windows' own npm can be on PATH too, and can't run there.
    if(WIN32)
        find_program(NPM_EXECUTABLE NAMES npm.cmd npm REQUIRED)
    else()
        find_program(NPM_EXECUTABLE NAMES npm REQUIRED)
    endif()

    file(GLOB_RECURSE DAWG_UI_SOURCES CONFIGURE_DEPENDS "${DAWG_UI_DIR}/src/*")
    list(APPEND DAWG_UI_SOURCES
        "${DAWG_UI_DIR}/index.html"
        "${DAWG_UI_DIR}/package.json"
        "${DAWG_UI_DIR}/vite.config.ts"
        "${DAWG_UI_DIR}/tsconfig.json")

    add_custom_command(
        OUTPUT "${DAWG_UI_DIR}/node_modules/.package-lock.json"
        COMMAND "${NPM_EXECUTABLE}" ci --no-audit --no-fund
        WORKING_DIRECTORY "${DAWG_UI_DIR}"
        DEPENDS "${DAWG_UI_DIR}/package-lock.json"
        COMMENT "Installing UI dependencies (npm ci)"
        VERBATIM)

    set(DAWG_UI_STAMP "${CMAKE_CURRENT_BINARY_DIR}/dawg_ui_build.stamp")

    add_custom_command(
        OUTPUT "${DAWG_UI_STAMP}"
        COMMAND "${NPM_EXECUTABLE}" run build
        COMMAND "${CMAKE_COMMAND}" -E touch "${DAWG_UI_STAMP}"
        WORKING_DIRECTORY "${DAWG_UI_DIR}"
        DEPENDS ${DAWG_UI_SOURCES} "${DAWG_UI_DIR}/node_modules/.package-lock.json"
        COMMENT "Building UI (vite build)"
        VERBATIM)

    add_custom_target(dawg_ui DEPENDS "${DAWG_UI_STAMP}")
    add_dependencies(DAWg dawg_ui)
endif()

if(DAWG_BUILD_UI)
    # dist exists because the target above builds it; with DAWG_BUILD_UI=OFF there is nothing to copy.
    add_custom_command(TARGET DAWg POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${DAWG_UI_DEST}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${DAWG_UI_DIR}/dist" "${DAWG_UI_DEST}"
        COMMENT "Copying web UI next to the app"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# MIDI files that ship with the app, next to it like the UI, with their licence.
# ---------------------------------------------------------------------------
if(APPLE)
    set(DAWG_MIDI_FILES_DEST "$<TARGET_BUNDLE_CONTENT_DIR:DAWg>/Resources/midi-files")
else()
    set(DAWG_MIDI_FILES_DEST "$<TARGET_FILE_DIR:DAWg>/midi-files")
endif()

add_custom_command(TARGET DAWg POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${free_midi_chords_SOURCE_DIR}" "-DDEST=${DAWG_MIDI_FILES_DEST}"
            "-DVERSION=${DAWG_FREE_MIDI_CHORDS_VERSION}" -P "${CMAKE_SOURCE_DIR}/cmake/CopyMidiFiles.cmake"
    COMMENT "Copying the MIDI files next to the app"
    VERBATIM)

# ---------------------------------------------------------------------------
# Faust: the libraries scripts import (docs/design/phase11-faust-panels.md,
# F5), their licences and the example scripts (F6), next to the app like the UI.
# ---------------------------------------------------------------------------
if(APPLE)
    set(DAWG_FAUST_DEST "$<TARGET_BUNDLE_CONTENT_DIR:DAWg>/Resources/faust")
else()
    set(DAWG_FAUST_DEST "$<TARGET_FILE_DIR:DAWg>/faust")
endif()

file(GLOB DAWG_FAUST_LIBRARY_FILES "${DAWG_FAUST_LIBRARIES_DIR}/*.lib")

add_custom_command(TARGET DAWg POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${DAWG_FAUST_DEST}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DAWG_FAUST_DEST}"
    COMMAND "${CMAKE_COMMAND}" -E copy ${DAWG_FAUST_LIBRARY_FILES} "${DAWG_FAUST_DEST}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${DAWG_FAUST_LIBRARIES_DIR}/README.md" "${DAWG_FAUST_DEST}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${DAWG_FAUST_LIBRARIES_DIR}/licenses" "${DAWG_FAUST_DEST}/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/faust/examples" "${DAWG_FAUST_DEST}/examples"
    COMMENT "Copying the Faust libraries and examples next to the app"
    VERBATIM)

# ---------------------------------------------------------------------------
# The pitch model the audio editor's Pitch view runs (docs/design/pitch-correction.md, section 3.2), in
# models/pitch next to the executable, where pitchModelsFolder() looks first.
# ---------------------------------------------------------------------------
add_custom_command(TARGET DAWg POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:DAWg>/models/pitch"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DAWG_PITCH_MODELS_DIR}/fcpe.onnx" "$<TARGET_FILE_DIR:DAWg>/models/pitch"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/models/pitch/LICENSE-fcpe.txt"
            "$<TARGET_FILE_DIR:DAWg>/models/pitch"
    COMMENT "Copying the pitch model and its licence next to the app"
    VERBATIM)

# ONNX Runtime's licence and third-party notices, which ship with its library (PitchModels.cmake).
add_custom_command(TARGET DAWg POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:DAWg>/licenses/onnxruntime"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DAWG_ONNXRUNTIME_DIR}/LICENSE" "${DAWG_ONNXRUNTIME_DIR}/ThirdPartyNotices.txt"
            "$<TARGET_FILE_DIR:DAWg>/licenses/onnxruntime"
    COMMENT "Copying ONNX Runtime's licence next to the app"
    VERBATIM)
