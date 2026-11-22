# Pitch detection for pitch correction (docs/design/pitch-correction.md, PC2): ONNX Runtime and the neural pitch
# models it runs. Both are downloaded at configure time, pinned by hash, like the WebView2 SDK.
#
# * ONNX Runtime (MIT), Microsoft's Microsoft.ML.OnnxRuntime package from nuget.org: one signed package with the
#   prebuilt CPU library for every platform and the C/C++ headers. (Its GitHub releases were the source until the
#   repository went away in November 2026.) Its shared library is copied next to every target that compiles the
#   engine on Windows, and found through the rpath elsewhere (dawg_add_engine).
# * FCPE (MIT, the weights by CN_ChiTu): the unofficial ONNX export at huggingface.co/Scicom-intl/fcpe-onnx, which has
#   FCPE's mel front end inside the graph and gives the 360-bin salience.
# * CREPE tiny, small, medium and full (MIT): the ONNX exports of github.com/yqzhishen/onnxcrepe, release v1.1.0, with
#   the frames' normalisation inside the graph.
#
# DAWG_PITCH_MODELS_DIR is where the models land; the tests and the pitch tool read them from there.

set(DAWG_ONNXRUNTIME_VERSION 1.32.0)
set(_ort_hash "a9150bc2992cbbb6722070da9910e1ffa989a8bfc67bdd5bb8e745ca755641db")

# The library inside the package, and the name it is laid out under: its own install name (soname), so the loader
# finds it through the rpath.
if(WIN32)
    set(_ort_runtime "win-x64")
    set(_ort_files onnxruntime.dll onnxruntime.lib)
    set(_ort_library "onnxruntime.dll")
elseif(APPLE)
    set(_ort_runtime "osx-arm64")
    set(_ort_files libonnxruntime.dylib)
    set(_ort_library "libonnxruntime.1.dylib")
else()
    set(_ort_runtime "linux-x64")
    set(_ort_files libonnxruntime.so)
    set(_ort_library "libonnxruntime.so.1")
endif()

set(_ort_root "${CMAKE_BINARY_DIR}/_deps/onnxruntime")
set(DAWG_ONNXRUNTIME_DIR "${_ort_root}/onnxruntime-${_ort_runtime}-${DAWG_ONNXRUNTIME_VERSION}-nuget")

if(NOT EXISTS "${DAWG_ONNXRUNTIME_DIR}/lib/${_ort_library}")
    set(_ort_pkg "${_ort_root}/microsoft.ml.onnxruntime.${DAWG_ONNXRUNTIME_VERSION}.nupkg")
    set(_ort_unpacked "${_ort_root}/unpacked")
    message(STATUS "Downloading ONNX Runtime ${DAWG_ONNXRUNTIME_VERSION}")
    file(DOWNLOAD
        "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${DAWG_ONNXRUNTIME_VERSION}/microsoft.ml.onnxruntime.${DAWG_ONNXRUNTIME_VERSION}.nupkg"
        "${_ort_pkg}"
        EXPECTED_HASH SHA256=${_ort_hash}
        TLS_VERIFY ON)

    # A .nupkg is a zip. Only the headers, this platform's library and the licences come out of it.
    set(_ort_patterns "build/native/include/*" LICENSE ThirdPartyNotices.txt)
    foreach(_file IN LISTS _ort_files)
        list(APPEND _ort_patterns "runtimes/${_ort_runtime}/native/${_file}")
    endforeach()
    file(REMOVE_RECURSE "${_ort_unpacked}" "${DAWG_ONNXRUNTIME_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${_ort_pkg}" DESTINATION "${_ort_unpacked}" PATTERNS ${_ort_patterns})

    file(COPY "${_ort_unpacked}/build/native/include/" DESTINATION "${DAWG_ONNXRUNTIME_DIR}/include")
    file(COPY "${_ort_unpacked}/LICENSE" "${_ort_unpacked}/ThirdPartyNotices.txt" DESTINATION "${DAWG_ONNXRUNTIME_DIR}")
    foreach(_file IN LISTS _ort_files)
        file(COPY "${_ort_unpacked}/runtimes/${_ort_runtime}/native/${_file}" DESTINATION "${DAWG_ONNXRUNTIME_DIR}/lib")
    endforeach()
    if(NOT WIN32)
        file(RENAME "${DAWG_ONNXRUNTIME_DIR}/lib/${_ort_files}" "${DAWG_ONNXRUNTIME_DIR}/lib/${_ort_library}")
    endif()
    file(REMOVE_RECURSE "${_ort_unpacked}")
    file(REMOVE "${_ort_pkg}")
endif()

if(NOT TARGET dawg_onnxruntime)
    add_library(dawg_onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(dawg_onnxruntime PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${DAWG_ONNXRUNTIME_DIR}/include"
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DAWG_ONNXRUNTIME_DIR}/include"
        IMPORTED_LOCATION "${DAWG_ONNXRUNTIME_DIR}/lib/${_ort_library}")
    if(WIN32)
        set_target_properties(dawg_onnxruntime PROPERTIES IMPORTED_IMPLIB "${DAWG_ONNXRUNTIME_DIR}/lib/onnxruntime.lib")
    endif()
endif()

set(DAWG_PITCH_MODELS_DIR "${CMAKE_BINARY_DIR}/_deps/pitch-models" CACHE INTERNAL "The pitch models (*.onnx)")

function(dawg_download_pitch_model name url hash)
    set(_file "${DAWG_PITCH_MODELS_DIR}/${name}.onnx")
    if(EXISTS "${_file}")
        file(SHA256 "${_file}" _have)
        if(_have STREQUAL hash)
            return()
        endif()
    endif()
    message(STATUS "Downloading pitch model ${name}")
    file(DOWNLOAD "${url}" "${_file}" EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
endfunction()

dawg_download_pitch_model(fcpe
    "https://huggingface.co/Scicom-intl/fcpe-onnx/resolve/11b6e6f968224227acd682cee8397ebe5b1821e5/fcpe.onnx"
    1a36aeb24b8695dce42fb18c432deb2ca9cd46567afb1a12e0e16301d524b925)

set(_crepe "https://github.com/yqzhishen/onnxcrepe/releases/download/v1.1.0")
dawg_download_pitch_model(crepe-tiny   "${_crepe}/tiny.onnx"   91fc2a0fd10f965dbf7775995daf50e99273caedd7efd00001f23be649da1bc3)
dawg_download_pitch_model(crepe-small  "${_crepe}/small.onnx"  cd119f6f5f608d9342a1c6f81e4653f21dbcde440a96d474ecbf6f27c6253466)
dawg_download_pitch_model(crepe-medium "${_crepe}/medium.onnx" 9bd7cd61bfd07596c7861e580ca04ef7170407994956d4c554ffd4863fb43ae3)
dawg_download_pitch_model(crepe-full   "${_crepe}/full.onnx"   119845c72c702e052e5262430f9d120bce46176689aa226c39d09dea5cc3a610)
