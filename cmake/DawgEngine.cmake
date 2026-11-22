# The engine is GUI-free code shared by the app and the tests.
#
# JUCE modules are INTERFACE libraries that must be linked PRIVATE into the
# final target (see JUCE's "CMake API.md"), which rules out a normal static
# library that re-exports them. Instead each consumer compiles the engine
# sources itself via dawg_add_engine(<target>).

set(DAWG_ENGINE_DIR "${CMAKE_CURRENT_LIST_DIR}/../engine")

set(DAWG_ENGINE_SOURCES
    "${DAWG_ENGINE_DIR}/src/AllocationGuard.cpp"
    "${DAWG_ENGINE_DIR}/src/AudioEngine.cpp"
    "${DAWG_ENGINE_DIR}/src/AudioPlayer.cpp"
    "${DAWG_ENGINE_DIR}/src/AudioSource.cpp"
    "${DAWG_ENGINE_DIR}/src/AudioToMidi.cpp"
    "${DAWG_ENGINE_DIR}/src/BuiltinNodes.cpp"
    "${DAWG_ENGINE_DIR}/src/Commands.cpp"
    "${DAWG_ENGINE_DIR}/src/Convolver.cpp"
    "${DAWG_ENGINE_DIR}/src/Crossover.cpp"
    "${DAWG_ENGINE_DIR}/src/Curve.cpp"
    "${DAWG_ENGINE_DIR}/src/DrumSynth.cpp"
    "${DAWG_ENGINE_DIR}/src/DrumVoices.cpp"
    "${DAWG_ENGINE_DIR}/src/EffectNodes.cpp"
    "${DAWG_ENGINE_DIR}/src/SplitNodes.cpp"
    "${DAWG_ENGINE_DIR}/src/MidiEffects.cpp"
    "${DAWG_ENGINE_DIR}/src/Export.cpp"
    "${DAWG_ENGINE_DIR}/src/FaustNode.cpp"
    "${DAWG_ENGINE_DIR}/src/GraphCompiler.cpp"
    "${DAWG_ENGINE_DIR}/src/JsonPatch.cpp"
    "${DAWG_ENGINE_DIR}/src/LoopReverb.cpp"
    "${DAWG_ENGINE_DIR}/src/loopreverb/Pattern.cpp"
    "${DAWG_ENGINE_DIR}/src/loopreverb/Reverb.cpp"
    "${DAWG_ENGINE_DIR}/src/loopreverb/dawg_sdk/Node.cpp"
    "${DAWG_ENGINE_DIR}/src/MidiFileIO.cpp"
    "${DAWG_ENGINE_DIR}/src/MidiInput.cpp"
    "${DAWG_ENGINE_DIR}/src/Model.cpp"
    "${DAWG_ENGINE_DIR}/src/ModulationNodes.cpp"
    "${DAWG_ENGINE_DIR}/src/MpeEncoder.cpp"
    "${DAWG_ENGINE_DIR}/src/Node.cpp"
    "${DAWG_ENGINE_DIR}/src/Onsets.cpp"
    "${DAWG_ENGINE_DIR}/src/NodeRegistry.cpp"
    "${DAWG_ENGINE_DIR}/src/ClapNoteEncoder.cpp"
    "${DAWG_ENGINE_DIR}/src/ClipOps.cpp"
    "${DAWG_ENGINE_DIR}/src/ClapPlugin.cpp"
    "${DAWG_ENGINE_DIR}/src/Oscillator.cpp"
    "${DAWG_ENGINE_DIR}/src/ParamLinks.cpp"
    "${DAWG_ENGINE_DIR}/src/Recording.cpp"
    "${DAWG_ENGINE_DIR}/src/RemotePlugin.cpp"
    "${DAWG_ENGINE_DIR}/src/RubberBandPitchShifter.cpp"
    "${DAWG_ENGINE_DIR}/src/RubberBandStretcher.cpp"
    "${DAWG_ENGINE_DIR}/src/SampleMapping.cpp"
    "${DAWG_ENGINE_DIR}/src/Sampler.cpp"
    "${DAWG_ENGINE_DIR}/src/PitchAnalysis.cpp"
    "${DAWG_ENGINE_DIR}/src/PitchModel.cpp"
    "${DAWG_ENGINE_DIR}/src/PitchNotes.cpp"
    "${DAWG_ENGINE_DIR}/src/PsolaPitchShifter.cpp"
    "${DAWG_ENGINE_DIR}/src/PluginNode.cpp"
    "${DAWG_ENGINE_DIR}/src/ProjectFile.cpp"
    "${DAWG_ENGINE_DIR}/src/Sequence.cpp"
    "${DAWG_ENGINE_DIR}/src/Sfz.cpp"
    "${DAWG_ENGINE_DIR}/src/Session.cpp"
    "${DAWG_ENGINE_DIR}/src/StretchRenderer.cpp"
    "${DAWG_ENGINE_DIR}/src/Time.cpp"
    "${DAWG_ENGINE_DIR}/src/TimingMap.cpp"
    "${DAWG_ENGINE_DIR}/src/Vibrato.cpp"
    "${DAWG_ENGINE_DIR}/src/VoiceAllocator.cpp"
    "${DAWG_ENGINE_DIR}/src/VoiceNodes.cpp")

# Extra nodes kept outside the repository (DAWG_PRIVATE_NODES_DIR). The folder's DawgPrivateNodes.cmake sets
# DAWG_PRIVATE_ENGINE_SOURCES (which define registerPrivateNodes()), DAWG_PRIVATE_TEST_SOURCES and, for extra
# targets, DAWG_PRIVATE_SUBDIRECTORIES.
set(DAWG_PRIVATE_ENGINE_SOURCES "")
set(DAWG_PRIVATE_TEST_SOURCES "")
set(DAWG_PRIVATE_SUBDIRECTORIES "")
if(DAWG_PRIVATE_NODES_DIR)
    file(TO_CMAKE_PATH "${DAWG_PRIVATE_NODES_DIR}" DAWG_PRIVATE_NODES_DIR)
    string(REGEX REPLACE "/+$" "" DAWG_PRIVATE_NODES_DIR "${DAWG_PRIVATE_NODES_DIR}")
    if(NOT EXISTS "${DAWG_PRIVATE_NODES_DIR}/DawgPrivateNodes.cmake")
        message(FATAL_ERROR "DAWG_PRIVATE_NODES_DIR has no DawgPrivateNodes.cmake: ${DAWG_PRIVATE_NODES_DIR}")
    endif()
    include("${DAWG_PRIVATE_NODES_DIR}/DawgPrivateNodes.cmake")
    message(STATUS "DAWg: private nodes from ${DAWG_PRIVATE_NODES_DIR}")
endif()

function(dawg_add_engine target)
    target_sources(${target} PRIVATE ${DAWG_ENGINE_SOURCES} ${DAWG_PRIVATE_ENGINE_SOURCES})
    target_include_directories(${target} PRIVATE "${DAWG_ENGINE_DIR}/include")
    target_include_directories(${target} SYSTEM PRIVATE "${clap_SOURCE_DIR}/include")

    # Rubber Band's single-file build (Dependencies.cmake): third-party code, so without DAWg's warnings.
    set(rubberband_single "${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp")
    target_sources(${target} PRIVATE "${rubberband_single}")
    target_include_directories(${target} SYSTEM PRIVATE "${rubberband_SOURCE_DIR}")
    if(MSVC)
        set_source_files_properties("${rubberband_single}" TARGET_DIRECTORY ${target}
            PROPERTIES COMPILE_OPTIONS "/W0;/bigobj" COMPILE_DEFINITIONS "_USE_MATH_DEFINES;NOMINMAX")
    else()
        set_source_files_properties("${rubberband_single}" TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    if(DAWG_PRIVATE_NODES_DIR)
        target_include_directories(${target} PRIVATE "${DAWG_PRIVATE_NODES_DIR}")
        target_compile_definitions(${target} PRIVATE DAWG_PRIVATE_NODES=1
            "DAWG_PRIVATE_NODES_DIR=\"${DAWG_PRIVATE_NODES_DIR}\"")
    else()
        target_compile_definitions(${target} PRIVATE DAWG_PRIVATE_NODES=0)
    endif()
    target_link_libraries(${target} PRIVATE
        juce::juce_core
        juce::juce_events
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors_headless
        juce::juce_dsp
        dawg_faust
        dawg_lame
        dawg_onnxruntime)

    # ONNX Runtime (PitchModels.cmake) is a shared library. On Windows and Linux it goes next to the executable, so the
    # app folder works wherever it is installed (on Windows also before the older copy Windows itself has); Linux finds
    # it there through an $ORIGIN rpath. macOS finds it through its rpath into the build.
    if(APPLE)
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${DAWG_ONNXRUNTIME_DIR}/lib")
    else()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:dawg_onnxruntime>" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
        if(NOT WIN32)
            set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
        endif()
    endif()

    target_compile_definitions(${target} PRIVATE "DAWG_FAUST_LIBRARIES_DIR=\"${DAWG_FAUST_LIBRARIES_DIR}\""
        "DAWG_PITCH_MODELS_DIR=\"${DAWG_PITCH_MODELS_DIR}\""
        # Imports read MP3; MP3 export reads its own file back for the loudness report (phase9-export-midi-input.md, X6).
        JUCE_USE_MP3AUDIOFORMAT=1)

    # ThreadSanitizer's runtime defines its own global operator new/delete, which
    # clash with the allocation guard's replacements, so the guard is off there.
    if(DAWG_ALLOCATION_GUARD AND NOT DAWG_SANITIZE MATCHES "thread")
        target_compile_definitions(${target} PRIVATE DAWG_ALLOCATION_GUARD=$<CONFIG:Debug>)
    else()
        target_compile_definitions(${target} PRIVATE DAWG_ALLOCATION_GUARD=0)
    endif()
endfunction()

# Flags for a DAWg target. Strict warnings (and warnings-as-errors) apply to
# DAWg's own sources only; JUCE module sources compiled into the same target
# keep JUCE's recommended flags.
function(dawg_set_common_flags target)
    target_link_libraries(${target} PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    if(MSVC)
        target_compile_options(${target} PRIVATE /permissive- /utf-8)
    endif()

    if(DAWG_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=${DAWG_SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=${DAWG_SANITIZE})

        # JUCE's bundled libFLAC calls its C callbacks through function pointers of a slightly different type,
        # which Clang's -fsanitize=function reports. That is third-party code, so the check is off.
        if(DAWG_SANITIZE MATCHES "undefined" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -fno-sanitize=function)
            target_link_options(${target} PRIVATE -fno-sanitize=function)
        endif()
    endif()

    get_target_property(sources ${target} SOURCES)
    set(own_sources "")
    foreach(source IN LISTS sources)
        get_filename_component(path "${source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        string(FIND "${path}" "${CMAKE_SOURCE_DIR}/" in_repo)
        string(FIND "${path}" "${CMAKE_BINARY_DIR}/" in_build)
        # CI keeps its dependency cache inside the checkout (CPM_SOURCE_CACHE): third-party code, not DAWg's own.
        string(FIND "${path}" "/.cpm-cache/" in_cache)
        if(in_repo EQUAL 0 AND NOT in_build EQUAL 0 AND in_cache EQUAL -1 AND path MATCHES "\\.(cpp|mm)$")
            list(APPEND own_sources "${path}")
        endif()
        if(DAWG_PRIVATE_NODES_DIR)
            string(FIND "${path}" "${DAWG_PRIVATE_NODES_DIR}/" in_private)
            if(in_private EQUAL 0 AND path MATCHES "\\.cpp$")
                list(APPEND own_sources "${path}")
            endif()
        endif()
    endforeach()

    if(MSVC)
        set(strict_flags /W4)
        if(DAWG_WARNINGS_AS_ERRORS)
            list(APPEND strict_flags /WX)
        endif()
    else()
        set(strict_flags -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
        if(DAWG_WARNINGS_AS_ERRORS)
            list(APPEND strict_flags -Werror)
        endif()
    endif()

    set_source_files_properties(${own_sources} TARGET_DIRECTORY ${target}
        PROPERTIES COMPILE_OPTIONS "${strict_flags}")
endfunction()
