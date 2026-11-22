juce_add_console_app(dawg_tests PRODUCT_NAME "dawg_tests")

target_sources(dawg_tests PRIVATE
    CommandTests.cpp
    CurveTests.cpp
    EngineRenderTests.cpp
    ExportTests.cpp
    FeedbackBenchTests.cpp
    ProjectBenchTests.cpp
    ComponentTests.cpp
    FeedbackTests.cpp
    FaustTests.cpp
    GateExpanderTests.cpp
    GraphCompilerTests.cpp
    GroupTests.cpp
    JsonPatchTests.cpp
    LoopReverbTests.cpp
    MidiInputTests.cpp
    MixingTests.cpp
    LinkTests.cpp
    MacroTests.cpp
    ModulationTests.cpp
    ModulationEffectsTests.cpp
    NodeGraphTests.cpp
    PitchAnalysisTests.cpp
    PitchNotesTests.cpp
    PitchShifterTests.cpp
    PluginTests.cpp
    ProjectFileTests.cpp
    ArrangementTests.cpp
    AutomationTests.cpp
    AudioTests.cpp
    AudioToMidiTests.cpp
    RealtimeTests.cpp
    SaturatorTests.cpp
    ClipperTests.cpp
    ClipOpsTests.cpp
    BounceTests.cpp
    CrossfadeTests.cpp
    FreezeTests.cpp
    PunchTests.cpp
    ConvolutionReverbTests.cpp
    ConvolverTests.cpp
    WaveshaperTests.cpp
    BitCrusherTests.cpp
    TransparentAdaaTests.cpp
    DelayLimiterTests.cpp
    LinkedNoteTests.cpp
    MidiEffectTests.cpp
    MidiFileTests.cpp
    SendTests.cpp
    MultiOutputTests.cpp
    SplitNodeTests.cpp
    SessionTests.cpp
    SidechainTests.cpp
    SliceTests.cpp
    TakesTests.cpp
    SwingTests.cpp
    TempoMapTests.cpp
    TimeStretchTests.cpp
    StretchRenderTests.cpp
    TimingMarkerTests.cpp
    VibratoTests.cpp
    VoiceAllocatorTests.cpp
    ClapTests.cpp
    RemotePluginTests.cpp
    TestMain.cpp
    OscillatorTests.cpp
    DrumDspTests.cpp
    DrumVoiceTests.cpp
    DrumSynthTests.cpp
    DrumSynthBenchTests.cpp
    DrumVoiceBenchTests.cpp
    CymbalPrototypeTests.cpp
    SampleMappingTests.cpp
    SfzTests.cpp
    SamplerTests.cpp
    VoiceGraphTests.cpp
    VoiceRuntimeTests.cpp
    OutputProtectionTests.cpp
    ${DAWG_PRIVATE_TEST_SOURCES})

dawg_add_engine(dawg_tests)
dawg_set_common_flags(dawg_tests)

# Tests compare floating-point results exactly on purpose (tests/.clang-tidy says which checks don't fit test code).
# Appended to each source's own options, after the strict flags dawg_set_common_flags gives them, so these win. The
# clean-room reverb's own test runner does the same in loopreverb/CMakeLists.txt.
if(NOT MSVC)
    get_target_property(test_sources dawg_tests SOURCES)
    foreach(source IN LISTS test_sources)
        get_filename_component(path "${source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        string(FIND "${path}" "${CMAKE_CURRENT_SOURCE_DIR}/" in_tests)
        if(in_tests EQUAL 0 AND path MATCHES "\\.cpp$")
            set_property(SOURCE "${path}" TARGET_DIRECTORY dawg_tests APPEND PROPERTY COMPILE_OPTIONS -Wno-float-equal)
        endif()
    endforeach()
endif()

target_compile_definitions(dawg_tests PRIVATE
    JUCE_USE_CURL=0
    "DAWG_FAUST_EXAMPLES_DIR=\"${CMAKE_SOURCE_DIR}/app/faust/examples\"")

# Private nodes' tests include TestUtils.h from here.
target_include_directories(dawg_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(dawg_tests PRIVATE Catch2::Catch2)

# The CLAP host's tests load these (docs/design/clap-hosting.md, section 7).
add_library(dawg_test_clap MODULE clap/TestPlugins.cpp)
target_include_directories(dawg_test_clap SYSTEM PRIVATE "${clap_SOURCE_DIR}/include")
target_compile_features(dawg_test_clap PRIVATE cxx_std_20)
set_target_properties(dawg_test_clap PROPERTIES PREFIX "" SUFFIX ".clap")
add_dependencies(dawg_tests dawg_test_clap)
target_compile_definitions(dawg_tests PRIVATE "DAWG_TEST_CLAP_PATH=\"$<TARGET_FILE:dawg_test_clap>\"")

include("${Catch2_SOURCE_DIR}/extras/Catch.cmake")
catch_discover_tests(dawg_tests)
add_subdirectory(loopreverb)
