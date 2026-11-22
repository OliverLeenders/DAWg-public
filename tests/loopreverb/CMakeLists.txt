# The Loop Reverb's clean-room tests (README.md): the clean side's own test runner, run against the reverb itself
# (engine/src/loopreverb/), without JUCE. Its acceptance tests include a CPU-time limit (SPEC A11), so ctest runs
# it in release configurations only; tests/LoopReverbTests.cpp covers the DAWg adapter in every configuration.
add_executable(dawg_loop_reverb_tests
    harness/Check.cpp
    harness/Render.cpp
    harness/Wav.cpp
    node/AcceptanceTests.cpp
    node/BehaviourTests.cpp
    node/InterfaceTests.cpp
    node/Measure.cpp
    node/Nodes.cpp
    "${DAWG_ENGINE_DIR}/src/loopreverb/Pattern.cpp"
    "${DAWG_ENGINE_DIR}/src/loopreverb/Reverb.cpp"
    "${DAWG_ENGINE_DIR}/src/loopreverb/dawg_sdk/Node.cpp")

# shim/ first: its dawg_sdk/Node.h makes the node API visible as dawg::, as the tests expect.
target_include_directories(dawg_loop_reverb_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim" "${DAWG_ENGINE_DIR}/src/loopreverb")
dawg_set_common_flags(dawg_loop_reverb_tests)

# The copied tests compare floating-point output exactly on purpose, and capture constants that Clang calls unused
# captures (see .clang-tidy here). Appended to each test source's own options, after the strict flags above.
if(NOT MSVC)
    foreach(source IN ITEMS harness/Check.cpp harness/Render.cpp harness/Wav.cpp node/AcceptanceTests.cpp
                            node/BehaviourTests.cpp node/InterfaceTests.cpp node/Measure.cpp node/Nodes.cpp)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${source}" TARGET_DIRECTORY dawg_loop_reverb_tests APPEND
                     PROPERTY COMPILE_OPTIONS -Wno-float-equal)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${source}" TARGET_DIRECTORY dawg_loop_reverb_tests APPEND
                         PROPERTY COMPILE_OPTIONS -Wno-unused-lambda-capture)
        endif()
    endforeach()
endif()

if(CMAKE_CONFIGURATION_TYPES)
    add_test(NAME loop_reverb_clean_room COMMAND dawg_loop_reverb_tests CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
elseif(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    add_test(NAME loop_reverb_clean_room COMMAND dawg_loop_reverb_tests)
endif()
