# Third-party dependencies. Versions are pinned so builds are reproducible;
# bump them deliberately (see docs/PLAN.md, "Dependency policy").

set(DAWG_JUCE_VERSION    9.0.4)
set(DAWG_CATCH2_VERSION  3.16.1)
set(DAWG_WEBVIEW2_VERSION 1.0.4258.31)

CPMAddPackage(
    NAME JUCE
    GITHUB_REPOSITORY juce-framework/JUCE
    GIT_TAG ${DAWG_JUCE_VERSION}
    GIT_SHALLOW TRUE)

# Treat JUCE headers as system headers so DAWg's strict warning flags (and
# -Werror / /WX) apply to DAWg code only.
foreach(module IN ITEMS juce_core juce_events juce_data_structures juce_graphics juce_gui_basics juce_gui_extra
                        juce_audio_basics juce_audio_devices juce_audio_formats juce_audio_processors
                        juce_audio_processors_headless juce_audio_utils)
    set_target_properties(${module} PROPERTIES SYSTEM TRUE)
endforeach()

if(DAWG_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION ${DAWG_CATCH2_VERSION}
        GIT_SHALLOW TRUE
        SYSTEM TRUE)
endif()

# WebView2 (Windows only). JUCE's FindWebView2 expects the NuGet package in a
# local folder; download and unpack it here so no manual NuGet step is needed.
if(WIN32)
    set(_wv2_root "${CMAKE_BINARY_DIR}/_deps/webview2")
    set(_wv2_dir  "${_wv2_root}/Microsoft.Web.WebView2.${DAWG_WEBVIEW2_VERSION}")

    if(NOT EXISTS "${_wv2_dir}/build/native/include/WebView2.h")
        set(_wv2_pkg "${_wv2_root}/webview2.${DAWG_WEBVIEW2_VERSION}.zip")
        message(STATUS "Downloading WebView2 SDK ${DAWG_WEBVIEW2_VERSION}")
        file(DOWNLOAD
            "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${DAWG_WEBVIEW2_VERSION}"
            "${_wv2_pkg}"
            STATUS _wv2_status
            TLS_VERIFY ON)
        list(GET _wv2_status 0 _wv2_code)
        if(NOT _wv2_code EQUAL 0)
            message(FATAL_ERROR "Failed to download WebView2 SDK: ${_wv2_status}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_wv2_pkg}" DESTINATION "${_wv2_dir}")
    endif()

    set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${_wv2_root}" CACHE PATH "" FORCE)
endif()

# Rubber Band (GPL v2 or later, used under GPLv3 inside AGPL DAWg): offline time stretching with its R3 engine
# (docs/design/time-stretching.md, TS1). Its single-file build has its own FFT and resampler, so DawgEngine.cmake
# compiles single/RubberBandSingle.cpp into the engine and nothing else is needed.
set(DAWG_RUBBERBAND_VERSION 4.0.0)
CPMAddPackage(
    NAME rubberband
    URL https://github.com/breakfastquay/rubberband/archive/refs/tags/v${DAWG_RUBBERBAND_VERSION}.tar.gz
    URL_HASH SHA256=24300f48a8014b7c863b573a9647e61b1b19b37875e2cdd92005e64c6424d266
    DOWNLOAD_ONLY YES)

# LAME (LGPL v2 or later, which DAWg's AGPL allows): the MP3 encoder for export (docs/design/phase9-export-midi-input.md,
# X6). It has no CMake build, so its library sources become a static library here, with cmake/lame/config.h in
# place of the configure script's header.
set(DAWG_LAME_VERSION 3.100)
CPMAddPackage(
    NAME lame
    URL https://downloads.sourceforge.net/project/lame/lame/${DAWG_LAME_VERSION}/lame-${DAWG_LAME_VERSION}.tar.gz
    URL_HASH SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
    DOWNLOAD_ONLY YES)

if(NOT TARGET dawg_lame)
    set(_lame "${lame_SOURCE_DIR}/libmp3lame")
    add_library(dawg_lame STATIC
        "${_lame}/VbrTag.c" "${_lame}/bitstream.c" "${_lame}/encoder.c" "${_lame}/fft.c" "${_lame}/gain_analysis.c"
        "${_lame}/id3tag.c" "${_lame}/lame.c" "${_lame}/mpglib_interface.c" "${_lame}/newmdct.c" "${_lame}/presets.c"
        "${_lame}/psymodel.c" "${_lame}/quantize.c" "${_lame}/quantize_pvt.c" "${_lame}/reservoir.c"
        "${_lame}/set_get.c" "${_lame}/tables.c" "${_lame}/takehiro.c" "${_lame}/util.c" "${_lame}/vbrquantize.c"
        "${_lame}/version.c")
    target_include_directories(dawg_lame PRIVATE "${CMAKE_CURRENT_LIST_DIR}/lame" "${_lame}")
    target_include_directories(dawg_lame SYSTEM PUBLIC "${lame_SOURCE_DIR}/include")
    target_compile_definitions(dawg_lame PRIVATE HAVE_CONFIG_H=1 _CRT_SECURE_NO_WARNINGS=1)
    set_target_properties(dawg_lame PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(dawg_lame PRIVATE /W0)
    else()
        target_compile_options(dawg_lame PRIVATE -w)
        find_library(DAWG_LIBM m)
        if(DAWG_LIBM)
            target_link_libraries(dawg_lame PRIVATE ${DAWG_LIBM})
        endif()
    endif()
endif()

# MIDI files that ship with the app (MIT; docs/design/sampler-midi-delay-limiter.md, section 3).
# The CLAP SDK: headers only (docs/design/clap-hosting.md, C1).
set(DAWG_CLAP_VERSION 1.2.9)
CPMAddPackage(
    NAME clap
    GITHUB_REPOSITORY free-audio/clap
    GIT_TAG ${DAWG_CLAP_VERSION}
    GIT_SHALLOW TRUE
    DOWNLOAD_ONLY YES)

set(DAWG_FREE_MIDI_CHORDS_VERSION 20260914)
CPMAddPackage(
    NAME free_midi_chords
    URL https://github.com/ldrolez/free-midi-chords/releases/download/v0.${DAWG_FREE_MIDI_CHORDS_VERSION}/free-midi-chords-${DAWG_FREE_MIDI_CHORDS_VERSION}.zip
    URL_HASH SHA256=153c4d9b186fb240473df5d4cb999502bd4e3264e22d03ff4d0a1c95b1918c8b
    DOWNLOAD_ONLY YES)
