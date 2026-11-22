# libfaust with the interpreter backend (docs/design/phase11-faust-panels.md, F1), optionally the LLVM backend in
# optimised builds (DAWG_FAUST_LLVM_DIR), and the Faust libraries.
#
# Faust's own build (build/CMakeLists.txt) is a top-level project with install and packaging rules, so it is not
# added as a subdirectory: this compiles the same sources the way its "staticlib" target does, with the interpreter
# backend (backend (INTERP interpreter ...) in build/misc/llvm.cmake) and no LLVM. The libraries are a git
# submodule of Faust, so they are fetched at the commit Faust 2.88.0 pins.

set(DAWG_FAUST_VERSION 2.88.0)
set(DAWG_FAUSTLIBRARIES_COMMIT f1d729e82ff51db9a4c9ecd300c90234448f56e1)

CPMAddPackage(
    NAME faust
    URL https://github.com/grame-cncm/faust/archive/refs/tags/${DAWG_FAUST_VERSION}.tar.gz
    DOWNLOAD_ONLY YES)

CPMAddPackage(
    NAME faustlibraries
    URL https://github.com/grame-cncm/faustlibraries/archive/${DAWG_FAUSTLIBRARIES_COMMIT}.tar.gz
    DOWNLOAD_ONLY YES)

set(DAWG_FAUST_LIBRARIES_DIR "${faustlibraries_SOURCE_DIR}" CACHE INTERNAL "The Faust libraries (*.lib)")
set(_faust_src "${faust_SOURCE_DIR}/compiler")

# The interpreter's peephole optimiser peeks at the instructions after the current one with *(cur + 1), also on a
# block's last instruction. Release builds read past the end harmlessly (the value is only used when the current
# instruction matches, which a block's final return never does), but MSVC's checked iterators stop Debug builds with
# an assertion. Indexing the raw pointer instead keeps the release behaviour.
set(_faust_optimizer "${_faust_src}/generator/interpreter/interpreter_optimizer.hh")
file(READ "${_faust_optimizer}" _faust_optimizer_text)
string(REPLACE "*(cur + 1)" "(&*cur)[1]" _faust_patched "${_faust_optimizer_text}")
string(REPLACE "*(cur + 2)" "(&*cur)[2]" _faust_patched "${_faust_patched}")
string(REPLACE "*(cur + 3)" "(&*cur)[3]" _faust_patched "${_faust_patched}")
if(NOT _faust_patched STREQUAL _faust_optimizer_text)
    file(WRITE "${_faust_optimizer}" "${_faust_patched}")
endif()

file(GLOB _faust_sources
    "${_faust_src}/*.cpp" "${_faust_src}/*/*.cpp" "${_faust_src}/draw/*/*.cpp"
    "${_faust_src}/generator/interpreter/*.cpp")
# main.cpp stays: with FAUST_LIB it only defines the empty faust_alarm() libfaust needs.

add_library(dawg_faust STATIC ${_faust_sources})

set(_faust_include_dirs "${_faust_src}")
foreach(dir IN ITEMS boxes documentator draw errors evaluate extended FaustAlgebra DirectedGraph interval generator
                     generator/c generator/cpp generator/dlang generator/csharp generator/fir generator/julia
                     generator/rust generator/cmajor generator/vhdl
                     generator/interpreter normalize parallelize parser patternmatcher propagate signals tlib
                     transform utils draw/device draw/schema)
    list(APPEND _faust_include_dirs "${_faust_src}/${dir}")
endforeach()

target_include_directories(dawg_faust PRIVATE ${_faust_include_dirs})
target_include_directories(dawg_faust SYSTEM PUBLIC "${faust_SOURCE_DIR}/architecture")
target_compile_features(dawg_faust PUBLIC cxx_std_17)
target_compile_definitions(dawg_faust PRIVATE
    FAUST_LIB INTERP_BUILD
    "TLIB_EXPORT_HEADER=<faust/export.h>" TLIB_API=LIBFAUST_API)

if(MSVC)
    target_compile_definitions(dawg_faust PRIVATE MSVisualStudio _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
    target_compile_options(dawg_faust PRIVATE /W0 /bigobj /utf-8 /MP)
    target_link_libraries(dawg_faust PRIVATE Ws2_32)
else()
    target_compile_definitions(dawg_faust PRIVATE "LIBDIR=\"lib\"")
    target_compile_options(dawg_faust PRIVATE -w)
    find_package(Threads REQUIRED)
    target_link_libraries(dawg_faust PRIVATE Threads::Threads)
endif()

if(DAWG_SANITIZE)
    target_compile_options(dawg_faust PRIVATE -fsanitize=${DAWG_SANITIZE} -fno-omit-frame-pointer)
endif()

# The LLVM backend compiles scripts to native code, about 9x faster than the interpreter (phase11-faust-panels.md,
# section 7). FaustNode.cpp picks the backend with DAWG_FAUST_LLVM. The defines are the ones Faust's own build sets
# (build/CMakeLists.txt, build/misc/llvm.cmake).
#
# Which LLVM: DAWG_FAUST_LLVM_DIR if set. Otherwise, on Windows, LLVM's release for MSVC (Windows has no LLVM package
# with libraries; its LLVM installer has tools only), and elsewhere the system's (apt: llvm-<version>-dev, Homebrew:
# llvm), which Faust supports from LLVM 15 to 22.
set(DAWG_LLVM_VERSION 21.1.8)

if(DAWG_FAUST_LLVM AND NOT DAWG_FAUST_LLVM_DIR AND WIN32)
    CPMAddPackage(
        NAME llvm
        URL https://github.com/llvm/llvm-project/releases/download/llvmorg-${DAWG_LLVM_VERSION}/clang+llvm-${DAWG_LLVM_VERSION}-x86_64-pc-windows-msvc.tar.xz
        URL_HASH SHA256=749d22f565fcd5718dbed06512572d0e5353b502c03fe1f7f17ee8b8aca21a47
        DOWNLOAD_ONLY YES)
    set(_llvm_paths PATHS "${llvm_SOURCE_DIR}/lib/cmake/llvm" NO_DEFAULT_PATH)
elseif(DAWG_FAUST_LLVM_DIR)
    set(_llvm_paths PATHS "${DAWG_FAUST_LLVM_DIR}/lib/cmake/llvm" NO_DEFAULT_PATH)
endif()

if(DAWG_FAUST_LLVM)
    # LLVMConfig.cmake sets CMAKE_MSVC_RUNTIME_LIBRARY to the runtime LLVM was built with, for every configuration;
    # keep ours (CMakeLists.txt), which leaves Debug builds on the DLL runtime.
    set(_runtime "${CMAKE_MSVC_RUNTIME_LIBRARY}")
    find_package(LLVM CONFIG ${_llvm_paths})
    set(CMAKE_MSVC_RUNTIME_LIBRARY "${_runtime}")

    if(NOT LLVM_FOUND)
        message(FATAL_ERROR "Faust's LLVM backend needs LLVM's libraries: install them (apt: llvm-18-dev, Homebrew: "
                            "llvm), point DAWG_FAUST_LLVM_DIR at an LLVM install, or set DAWG_FAUST_LLVM=OFF to use "
                            "the interpreter")
    endif()

    message(STATUS "Faust: LLVM ${LLVM_PACKAGE_VERSION} backend")
    string(REGEX REPLACE "([0-9]+)\\..*" "\\1" _llvm_major "${LLVM_PACKAGE_VERSION}")

    # With MSVC, Debug keeps the interpreter: LLVM's release has optimised libraries for the static runtime only.
    if(MSVC)
        set(_optimised "$<NOT:$<CONFIG:Debug>>")
    else()
        set(_optimised "1")
    endif()
    file(GLOB _faust_llvm_sources "${_faust_src}/generator/llvm/*.cpp")
    llvm_map_components_to_libnames(_llvm_libs
        core executionengine mcjit native passes ipo irreader bitreader bitwriter linker)

    # LLVM's PDB reader names the DIA SDK by the path on the machine that built the release; use this Visual Studio's.
    if(MSVC AND TARGET LLVMDebugInfoPDB)
        get_target_property(_pdb_libs LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
        list(TRANSFORM _pdb_libs REPLACE ".*/diaguids\\.lib$" "${CMAKE_GENERATOR_INSTANCE}/DIA SDK/lib/amd64/diaguids.lib")
        set_target_properties(LLVMDebugInfoPDB PROPERTIES INTERFACE_LINK_LIBRARIES "${_pdb_libs}")
    endif()

    # Each entry gets its own condition: a list inside one generator expression would be split apart around it.
    list(TRANSFORM _faust_llvm_sources REPLACE "^(.+)$" "$<${_optimised}:\\1>")
    list(TRANSFORM _llvm_libs REPLACE "^(.+)$" "$<${_optimised}:\\1>")
    target_sources(dawg_faust PRIVATE ${_faust_llvm_sources})
    target_include_directories(dawg_faust PRIVATE "${_faust_src}/generator/llvm" ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(dawg_faust
        PRIVATE
            "$<${_optimised}:LLVM_BUILD>" "$<${_optimised}:LLVM_${_llvm_major}0>"
            "$<${_optimised}:LLVM_VERSION=\"${LLVM_PACKAGE_VERSION}\">"
        PUBLIC
            "$<${_optimised}:DAWG_FAUST_LLVM=1>")
    target_link_libraries(dawg_faust PUBLIC ${_llvm_libs})

    # As in Faust's own build: its LLVM glue is compiled like LLVM (without RTTI, with LLVM's definitions), except with
    # MSVC, where LLVM's definitions (UNICODE among them) would break libfaust.
    if(NOT MSVC)
        separate_arguments(_llvm_definitions UNIX_COMMAND "${LLVM_DEFINITIONS}")
        set_source_files_properties("${_faust_src}/generator/llvm/llvm_dsp_aux.cpp"
                                    "${_faust_src}/generator/llvm/llvm_dynamic_dsp_aux.cpp"
                                    PROPERTIES COMPILE_OPTIONS "-fno-rtti;${_llvm_definitions}")
    endif()
endif()
