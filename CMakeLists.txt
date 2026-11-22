cmake_minimum_required(VERSION 3.25)

project(DAWg VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(DAWG_ENABLE_VST2 "Enable VST2 plugin hosting (requires the legacy VST2 SDK, not distributed with DAWg)" OFF)
set(DAWG_VST2_SDK_PATH "" CACHE PATH "Path to the legacy VST2 SDK (the folder containing pluginterfaces/vst2.x)")
option(DAWG_BUILD_UI "Build the web UI (npm ci && npm run build) as part of the app build" ON)
option(DAWG_BUILD_TESTS "Build unit tests" ON)
option(DAWG_WARNINGS_AS_ERRORS "Treat compiler warnings in DAWg's own sources as errors" ON)
option(DAWG_ALLOCATION_GUARD "Count heap allocations on the audio thread in Debug builds" ON)
set(DAWG_SANITIZE "" CACHE STRING "Sanitizers to enable with GCC/Clang, e.g. 'address,undefined' or 'thread'")
option(DAWG_FAUST_LLVM "Compile Faust scripts to native code with libfaust's LLVM backend (OFF: its interpreter)" ON)
set(DAWG_FAUST_LLVM_DIR "" CACHE PATH
    "The LLVM install (with lib/cmake/llvm) for DAWG_FAUST_LLVM; empty: on Windows LLVM's release, downloaded, elsewhere the system's")

# LLVM's Windows releases are built with the static C runtime, and MSVC links objects of one runtime only, so an
# optimised build with the LLVM backend uses it everywhere; Debug builds keep the interpreter and the DLL runtime.
if(MSVC AND DAWG_FAUST_LLVM)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>")
endif()

set(DAWG_PRIVATE_NODES_DIR "" CACHE PATH
    "Folder of extra nodes compiled into this build only (its DawgPrivateNodes.cmake lists them); empty for none")

# ---------------------------------------------------------------------------
# Dependencies (pinned to the latest releases at the time of writing)
# ---------------------------------------------------------------------------
include(cmake/CPM.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Faust.cmake)
include(cmake/PitchModels.cmake)

if(DAWG_ENABLE_VST2)
    if(NOT EXISTS "${DAWG_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffect.h")
        message(FATAL_ERROR
            "DAWG_ENABLE_VST2 is ON but DAWG_VST2_SDK_PATH does not point to a VST2 SDK "
            "(expected ${DAWG_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffect.h).")
    endif()
    juce_set_vst2_sdk_path("${DAWG_VST2_SDK_PATH}")
endif()

include(cmake/DawgEngine.cmake)

add_subdirectory(app)

if(WIN32)
    add_subdirectory(tools/ipcbench)
endif()

foreach(directory IN LISTS DAWG_PRIVATE_SUBDIRECTORIES)
    get_filename_component(name "${directory}" NAME)
    add_subdirectory("${directory}" "${CMAKE_BINARY_DIR}/private/${name}")
endforeach()

if(DAWG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
