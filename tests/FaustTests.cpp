#include "TestUtils.h"
#include "dawg/FaustNode.h"
#include "dawg/GraphCompiler.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <thread>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Phase 11: Faust scripting (docs/design/phase11-faust-panels.md, F1-F4, F8).

namespace
{
constexpr int frames = 4096;
constexpr float level = 0.25f;

// A stereo gain with one slider.
constexpr const char* gainScript = R"(g = hslider("gain[unit:dB]", 0.5, 0, 2, 0.01);
process = *(g), *(g);)";

[[nodiscard]] std::shared_ptr<const FaustProgram> compileScript (const char* code)
{
    juce::String error;
    auto program = FaustProgram::compile (code, FaustCompiler::shared().libraryDirectory(), error);
    INFO (error);
    REQUIRE (program != nullptr);
    return program;
}

[[nodiscard]] const ParamInfo& paramNamed (const NodeDescriptor& descriptor, const char* name)
{
    const auto it = std::ranges::find_if (descriptor.params,
                                          [name] (const ParamInfo& p) { return std::string_view (p.name) == name; });
    REQUIRE (it != descriptor.params.end());
    return *it;
}

// An audio track monitoring a constant input, with a Faust node in its chain.
struct FaustRig
{
    explicit FaustRig (const char* code)
        : input (frames, level)
    {
        project = apply ({}, "addTrack", { { "kind", "audio" } });
        trackId = project.tracks[0]->id;
        project = apply (project, "setTrackSwitches", { { "id", trackId }, { "inputChannel", 0 }, { "monitor", 2 } });
        reference = render();
        project =
            apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::faust }, { "code", code } });
        nodeId =
            std::ranges::find_if (track().graph.nodes, [] (const GraphNode& n) { return n.type == nodetype::faust; })
                ->id;
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);
    }

    // Renders after an update(), so instances (and their crossfades) carry over between calls.
    [[nodiscard]] StereoBuffer render (int numSamples = frames)
    {
        if (auto compiled = compiler.update (project, spec))
            engine.setEngine (compiled);

        const std::array<const float*, 1> inputs { input.data() };
        StereoBuffer out (numSamples);
        engine.renderOffline (out.channels(), 2, numSamples, sampleRate, inputs.data(), 1);
        return out;
    }

    [[nodiscard]] const Track& track() const { return *project.tracks[0]; }

    [[nodiscard]] FaustNode* node() const { return dynamic_cast<FaustNode*> (compiler.findInstance (trackId, nodeId)); }

    std::vector<float> input;
    Project project;
    ObjectId trackId = 0;
    ObjectId nodeId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
    StereoBuffer reference { 1 };
};
} // namespace

TEST_CASE ("a Faust script compiles to a node description", "[faust]")
{
    const auto program = compileScript (gainScript);
    const auto& descriptor = program->descriptor();

    CHECK (std::string_view (descriptor.type) == nodetype::faust);
    CHECK (program->numInputs() == 2);
    CHECK (program->numOutputs() == 2);
    REQUIRE (descriptor.inputs.size() == 1 + maxModulationInputs);
    REQUIRE (descriptor.outputs.size() == 1);
    REQUIRE (descriptor.params.size() == 1);

    const auto& gain = descriptor.params[0];
    CHECK (juce::String (gain.id).endsWith ("/gain"));
    CHECK (std::string_view (gain.unit) == "dB");
    CHECK_THAT (gain.defaultValue, WithinAbs (0.5, 1.0e-6));
    CHECK_THAT (gain.maxValue, WithinAbs (2.0, 1.0e-6));
    CHECK (gain.kind == ParamKind::continuous);
}

TEST_CASE ("Faust errors name the line", "[faust]")
{
    juce::String error;
    const auto program =
        FaustProgram::compile ("g = 1;\nprocess = nothing;\n", FaustCompiler::shared().libraryDirectory(), error);

    CHECK (program == nullptr);
    CHECK (error.contains ("2"));
    CHECK (error.containsIgnoreCase ("nothing"));
}

TEST_CASE ("Faust scripts import the bundled libraries", "[faust]")
{
    const auto program = compileScript (R"(import("stdfaust.lib");
process = fi.lowpass(2, 1000);)");

    // One channel in and out: a mono port each.
    CHECK (program->numInputs() == 1);
    CHECK (program->descriptor().inputs.size() == 1 + maxModulationInputs);
    CHECK (program->descriptor().outputs.size() == 1);
}

TEST_CASE ("Faust controls become parameters and a grouped layout", "[faust]")
{
    const auto program = compileScript (R"(
mode = nentry("mode[style:menu{'Soft':0;'Hard':1;'Fold':2}]", 1, 0, 2, 1);
steps = nentry("steps", 4, 1, 16, 1);
on = checkbox("on");
freq = hslider("freq[unit:Hz][scale:log][style:knob]", 1000, 20, 20000, 1);
meter = hbargraph("level", 0, 1);
process = hgroup("Shaper", vgroup("Main", *(mode + steps + on + freq) : meter)), _;
)");
    const auto& descriptor = program->descriptor();

    const auto& mode = paramNamed (descriptor, "mode");
    CHECK (mode.kind == ParamKind::choice);
    REQUIRE (mode.choices.size() == 3);
    CHECK (std::string_view (mode.choices[2]) == "Fold");

    CHECK (paramNamed (descriptor, "steps").kind == ParamKind::integer);
    CHECK (paramNamed (descriptor, "on").kind == ParamKind::toggle);
    CHECK_THAT (paramNamed (descriptor, "freq").midValue, WithinAbs (std::sqrt (20.0 * 20000.0), 0.1));
    CHECK (juce::String (paramNamed (descriptor, "freq").id).contains ("/Shaper/Main/"));
    CHECK (program->numMeters() == 1);

    // The layout follows the groups (F8).
    const auto& root = program->layout();
    const std::function<juce::var (const juce::var&, const juce::String&)> findLabel =
        [&] (const juce::var& item, const juce::String& label) -> juce::var
    {
        if (item["label"].toString() == label)
            return item;

        if (const auto* items = item["items"].getArray())
            for (const auto& child : *items)
                if (auto found = findLabel (child, label); ! found.isVoid())
                    return found;

        return {};
    };

    CHECK (findLabel (root, "Shaper")["kind"].toString() == "hgroup");
    CHECK (findLabel (root, "Main")["kind"].toString() == "vgroup");
    CHECK (findLabel (root, "freq")["style"].toString() == "knob");
    CHECK (findLabel (root, "level")["kind"].toString() == "hbargraph");
    CHECK (static_cast<int> (findLabel (root, "level")["meter"]) == 0);

    const auto described = program->toVar();
    CHECK (static_cast<int> (described["channels"]["inputs"]) == 2);
    CHECK (described["params"].size() == 4);
}

TEST_CASE ("a Faust node processes audio with its parameters by path", "[faust]")
{
    FaustRig rig (gainScript);
    auto out = rig.render();
    REQUIRE (rig.node() != nullptr);
    REQUIRE (rig.node()->program() != nullptr);

    // Default gain 0.5.
    CHECK_THAT (out.left().back(), WithinAbs (rig.reference.left().back() * 0.5f, 1.0e-4));

    const auto path = juce::String (rig.node()->descriptor().params[0].id);
    rig.edit ("setNodeParam",
              { { "trackId", rig.trackId }, { "nodeId", rig.nodeId }, { "param", path }, { "value", 9.0 } });
    CHECK_THAT (rig.track().graph.nodes.back().params.at (path),
                WithinAbs (2.0, 1.0e-9)); // clamped to the script's range

    out = rig.render();
    CHECK_THAT (out.left().back(), WithinAbs (rig.reference.left().back() * 2.0f, 1.0e-3));

    CHECK_THROWS_AS (
        apply (rig.project, "setNodeParam",
               { { "trackId", rig.trackId }, { "nodeId", rig.nodeId }, { "param", "/nope" }, { "value", 1.0 } }),
        CommandError);

    // Bypass passes the input through.
    rig.edit ("setNodeBypass", { { "trackId", rig.trackId }, { "nodeId", rig.nodeId }, { "bypassed", true } });
    out = rig.render();
    CHECK_THAT (out.left().back(), WithinAbs (rig.reference.left().back(), 1.0e-4));
}

TEST_CASE ("editing a Faust script crossfades to the new DSP", "[faust]")
{
    FaustRig rig ("process = _, _;");
    std::ignore = rig.render();
    auto* first = rig.node();
    REQUIRE (first != nullptr);

    rig.edit ("setNodeCode",
              { { "trackId", rig.trackId }, { "nodeId", rig.nodeId }, { "code", "process = *(0), *(0);" } });
    const auto out = rig.render (2048);
    REQUIRE (rig.node() != first);

    // 20 ms at 48 kHz: from the old output (the input) to the new one (silence) over 960 samples.
    const auto full = rig.reference.left().back();
    CHECK_THAT (out.left()[0], WithinAbs (full, 1.0e-3));
    CHECK_THAT (out.left()[480], WithinAbs (full * 0.5f, 0.02));
    CHECK_THAT (out.left()[1500], WithinAbs (0.0, 1.0e-6));

    // The replaced node is let go once the fade is over (the graph compiler does this on its next compilation).
    CHECK (rig.node()->releaseFinishedFade());
    CHECK_FALSE (rig.node()->releaseFinishedFade());
}

TEST_CASE ("a Faust script that fails to compile keeps the old DSP running", "[faust]")
{
    FaustRig rig (gainScript);
    std::ignore = rig.render();
    auto* node = rig.node();

    rig.edit ("setNodeCode", { { "trackId", rig.trackId }, { "nodeId", rig.nodeId }, { "code", "process = oops;" } });
    const auto out = rig.render();

    CHECK (rig.node() == node);
    CHECK_THAT (out.left().back(), WithinAbs (rig.reference.left().back() * 0.5f, 1.0e-4));

    const auto result = FaustCompiler::shared().find ("process = oops;");
    CHECK (result.program == nullptr);
    CHECK (result.error.isNotEmpty());

    // Undo brings the cached program back instantly.
    rig.edit ("setNodeCode", { { "trackId", rig.trackId }, { "nodeId", rig.nodeId }, { "code", gainScript } });
    std::ignore = rig.render();
    CHECK (rig.node()->program() == FaustCompiler::shared().find (gainScript).program);
}

TEST_CASE ("Faust nodes keep their code in the project", "[faust]")
{
    FaustRig rig (gainScript);
    const auto& node = rig.track().graph.nodes.back();
    CHECK (node.code == gainScript);
    CHECK (juce::JSON::toString (toVar (rig.project)).contains ("hslider"));

    CHECK_THROWS_AS (apply (rig.project, "setNodeCode",
                            { { "trackId", rig.trackId },
                              { "nodeId", rig.track().graph.nodes.front().id },
                              { "code", "process = _;" } }),
                     CommandError);

    // A new Faust node starts as a stereo pass-through.
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::faust } });
    CHECK (rig.track().graph.nodes.back().code == defaultFaustCode);
}

TEST_CASE ("background compiling reports through the listener", "[faust]")
{
    FaustCompiler compiler;
    std::atomic<int> finished { 0 };
    compiler.setListener ([&] { ++finished; });
    compiler.setBackground (true);

    const auto first = compiler.request ("process = *(0.25);");
    CHECK (first.pending);

    for (int i = 0; i < 500 && finished.load() == 0; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    REQUIRE (finished.load() == 1);
    CHECK (compiler.request ("process = *(0.25);").program != nullptr);
}

TEST_CASE ("the example scripts compile", "[faust]")
{
    const auto examples = juce::File (DAWG_FAUST_EXAMPLES_DIR).findChildFiles (juce::File::findFiles, false, "*.dsp");
    CHECK (examples.size() == 5);

    for (const auto& file : examples)
    {
        juce::String error;
        const auto program =
            FaustProgram::compile (file.loadFileAsString(), FaustCompiler::shared().libraryDirectory(), error);
        INFO (file.getFileName() << ": " << error);
        REQUIRE (program != nullptr);
        CHECK (program->numOutputs() == 2);
        CHECK (program->descriptor().params.size() >= 2);
    }
}

// A measurement, not a check: how long the example scripts take to compile with this build's backend (the LLVM backend
// compiles more slowly than the interpreter, and runs much faster; cmake/Faust.cmake).
TEST_CASE ("Faust example scripts: compile time", "[.][benchmark][faust]")
{
    const auto examples = juce::File (DAWG_FAUST_EXAMPLES_DIR).findChildFiles (juce::File::findFiles, false, "*.dsp");

    for (const auto& file : examples)
    {
        const auto started = std::chrono::steady_clock::now();
        juce::String error;
        const auto program =
            FaustProgram::compile (file.loadFileAsString(), FaustCompiler::shared().libraryDirectory(), error);
        const auto elapsed = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - started);
        CHECK (program != nullptr);
        WARN (file.getFileName() << ": " << elapsed.count() << " ms");
    }
}

TEST_CASE ("a Faust script's odd last channel is a mono port", "[faust][mono]")
{
    // docs/design/mono-ports.md, MP5.
    const auto mono = compileScript ("process = *(0.5);");
    REQUIRE (mono->descriptor().inputs.size() == 1 + maxModulationInputs);
    REQUIRE (mono->descriptor().outputs.size() == 1);
    CHECK (mono->descriptor().inputs[0].channels == 1);
    CHECK (mono->descriptor().outputs[0].channels == 1);

    const auto three = compileScript ("process = _ <: _, _, _;");
    REQUIRE (three->descriptor().outputs.size() == 2);
    CHECK (three->descriptor().outputs[0].channels == 2);
    CHECK (three->descriptor().outputs[1].channels == 1);

    const auto stereo = compileScript (gainScript);
    CHECK (stereo->descriptor().inputs[0].channels == 2);
    CHECK (stereo->descriptor().outputs[0].channels == 2);
}

TEST_CASE ("a mono Faust script runs on one channel and is heard on both sides", "[faust][mono]")
{
    FaustRig rig ("process = *(0.5);");
    const auto output = rig.render();
    CHECK_THAT (output.left().back(), WithinAbs (0.5 * level, 1.0e-6));
    CHECK_THAT (output.right().back(), WithinAbs (0.5 * level, 1.0e-6));
}
