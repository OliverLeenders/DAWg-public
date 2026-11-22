#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/CompiledEngine.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

// Octave modulation, macro controls and how macro slots reach a track's nodes, components and voice graphs
// (docs/design/macros-and-modulation-column.md).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
[[nodiscard]] juce::var list (std::initializer_list<juce::var> items)
{
    juce::Array<juce::var> result;

    for (const auto& item : items)
        result.add (item);

    return result;
}

[[nodiscard]] const GraphNode& nodeOfType (const TrackGraph& graph, const char* type)
{
    const auto it = std::ranges::find_if (graph.nodes, [type] (const GraphNode& n) { return n.type == type; });
    REQUIRE (it != graph.nodes.end());
    return *it;
}

[[nodiscard]] const ParamInfo& paramOf (const char* type, const char* id)
{
    const auto* descriptor = NodeRegistry::builtin().find (type);
    REQUIRE (descriptor != nullptr);
    const auto index = descriptor->indexOfParam (id);
    REQUIRE (index >= 0);
    return descriptor->params[static_cast<size_t> (index)];
}

constexpr float level = 0.25f;
constexpr int frames = 9600;
constexpr size_t probe = 4000;

// An audio track monitoring a constant input, so the level after its Gain shows the amplitude.
struct Rig
{
    Rig()
        : input (frames, level)
    {
        project = apply ({}, "addTrack", { { "kind", "audio" } });
        trackId = project.tracks[0]->id;
        project = apply (project, "setTrackSwitches", { { "id", trackId }, { "inputChannel", 0 }, { "monitor", 2 } });
        gainId = nodeOfType (project.tracks[0]->graph, nodetype::gain).id;
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        auto object = makeObject (args);
        object.getDynamicObject()->setProperty ("trackId", trackId);
        project = applyCommand (project, command, object);
    }

    [[nodiscard]] ObjectId macrosId() const { return nodeOfType (project.tracks[0]->graph, nodetype::macros).id; }

    void setKnob (int knob, double percent, int neutral = 0)
    {
        edit ("setNodeParam",
              { { "nodeId", macrosId() }, { "param", "macro" + juce::String (knob + 1) }, { "value", percent } });
        edit ("setNodeParam",
              { { "nodeId", macrosId() }, { "param", "neutral" + juce::String (knob + 1) }, { "value", neutral } });
    }

    [[nodiscard]] float render()
    {
        if (auto compiled = compiler.update (project, spec); compiled != nullptr)
            engine.setEngine (compiled);

        engine.play();
        const std::array<const float*, 1> inputs { input.data() };
        StereoBuffer out (frames);
        engine.renderOffline (out.channels(), 2, frames, sampleRate, inputs.data(), 1);
        return out.left()[probe];
    }

    std::vector<float> input;
    Project project;
    ObjectId trackId = 0;
    ObjectId gainId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};

// Records the per-sample values the engine hands its one parameter, a frequency like a filter's cutoff.
constexpr std::array<PortInfo, 1> recorderInputs { { { PortType::modulation, "Mod" } } };
constexpr std::array<ParamInfo, 1> recorderParams { {
    { "cutoff", "Cutoff", 20.0f, 20000.0f, 1000.0f, ParamKind::continuous, "Hz", 1000.0f },
} };
const NodeDescriptor recorderDescriptor { "test.recorder", "Recorder", recorderInputs, {}, recorderParams };

class Recorder final : public Node
{
public:
    Recorder()
        : Node (recorderDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        const auto* values = context.modulatedValues (0);

        for (int s = 0; s < context.numSamples && values != nullptr; ++s)
            recorded[static_cast<size_t> (s)] = values[s];
    }

    std::array<float, blockSize> recorded {};
};
} // namespace

// ---------------------------------------------------------------------------------------------------------
// Octave modulation (MC1 to MC4)

TEST_CASE ("a frequency is modulated in octaves, the same at 200 Hz and at 8 kHz", "[macros][modulation]")
{
    const auto& cutoff = paramOf (nodetype::filter, "cutoff");
    REQUIRE (cutoff.modulationScale() == ModulationScale::octaves);
    CHECK_THAT (cutoff.modulationRange(), WithinRel (std::log2 (1000.0f), 1.0e-5f));

    // Half an octave (5 % of the near ten octaves), wherever the cutoff sits.
    const auto amount = 0.5f / cutoff.modulationRange();
    CHECK_THAT (modulatedParamValue (cutoff, 200.0f, amount) / 200.0f, WithinRel (std::sqrt (2.0f), 1.0e-3f));
    CHECK_THAT (modulatedParamValue (cutoff, 8000.0f, amount) / 8000.0f, WithinRel (std::sqrt (2.0f), 1.0e-3f));
    CHECK_THAT (modulatedParamValue (cutoff, 8000.0f, -amount) / 8000.0f, WithinRel (1.0f / std::sqrt (2.0f), 1.0e-3f));

    // Clamped at the range's edges.
    CHECK (modulatedParamValue (cutoff, 16000.0f, 0.5f) == 20000.0f);
    CHECK (modulatedParamValue (cutoff, 40.0f, -0.5f) == 20.0f);
    CHECK (modulatedParamValue (cutoff, 1000.0f, 0.0f) == 1000.0f);
}

TEST_CASE ("a skewed parameter moves along its knob; a linear one and a declared span keep their units",
           "[macros][modulation]")
{
    // The ADSR's attack is skewed (0..10 s, 100 ms in the middle): half the knob's travel from the bottom is its middle.
    const auto& attack = paramOf (nodetype::adsr, "attackMs");
    REQUIRE (attack.modulationScale() == ModulationScale::travel);
    CHECK_THAT (modulatedParamValue (attack, 0.0f, 0.5f), WithinRel (100.0f, 1.0e-3f));
    CHECK (modulatedParamValue (attack, 5000.0f, 1.0f) == 10000.0f);

    // The Gain's amplitude is linear: 0.25 of its 200 % range.
    const auto& amplitude = paramOf (nodetype::gain, "amplitude");
    REQUIRE (amplitude.modulationScale() == ModulationScale::linear);
    CHECK_THAT (modulatedParamValue (amplitude, 100.0f, 0.25f), WithinAbs (150.0f, 1.0e-4f));

    // The Feedback node's Time is skewed but declares a span in ms for chorus-sized moves, so stays linear.
    const auto& time = paramOf (nodetype::feedback, "time");
    CHECK (time.modulationScale() == ModulationScale::linear);
}

TEST_CASE ("the per-sample values follow the same octaves as the block-rate write", "[macros][modulation][engine]")
{
    Recorder recorder;
    recorder.params().set (0, 400.0f);

    // A source rising across the block from -1 to 1.
    std::vector<float> source (blockSize);

    for (size_t s = 0; s < source.size(); ++s)
        source[s] = -1.0f + 2.0f * static_cast<float> (s) / static_cast<float> (blockSize);

    CompiledStep step;
    step.node = &recorder;
    step.audioInputs = { AudioInput { source.data(), source.data() } };
    CompiledModulation modulation;
    modulation.port = 0;
    modulation.depth = 0.2f; // two octaves at full scale
    modulation.node = &recorder;
    modulation.param = 0;
    modulation.info = recorderParams[0];
    modulation.state = std::make_shared<ModulationState>();
    modulation.state->depth.store (modulation.depth); // the depth that plays lives in the shared state
    step.modulations.push_back (modulation);
    step.modulationValues = std::make_unique<float[]> (blockSize); // NOLINT(modernize-avoid-c-arrays)

    ProcessContext context;
    context.numSamples = blockSize;
    runSteps (std::span (&step, 1), nullptr, context, 0, blockSize);

    const auto octaves = 0.2f * recorderParams[0].modulationRange();

    for (size_t s = 0; s < blockSize; s += 64)
        CHECK_THAT (recorder.recorded[s], WithinRel (400.0f * std::exp2 (source[s] * octaves), 1.0e-4f));

    // The block-rate value is the source at the middle of the block, through the same function.
    CHECK_THAT (recorder.params().get (0), WithinRel (recorder.recorded[blockSize / 2], 1.0e-5f));
}

// ---------------------------------------------------------------------------------------------------------
// The Macros device (MC5, MC6)

TEST_CASE ("a macro knob sends 0 at its neutral position and ramps to a new value", "[macros][dsp]")
{
    auto node = NodeRegistry::builtin().create (nodetype::macros);
    REQUIRE (node != nullptr);
    node->prepare (spec);
    std::vector<std::array<float, blockSize>> left (maxMacros);
    std::vector<std::array<float, blockSize>> right (maxMacros);
    std::vector<AudioOutput> outputs;

    for (size_t k = 0; k < maxMacros; ++k)
        outputs.push_back ({ left[k].data(), right[k].data() });

    const auto process = [&]
    {
        ProcessContext context;
        context.numSamples = blockSize;
        context.audioOutputs = outputs;
        node->process (context);
    };

    // Knob 1 at 30 %: Bottom sends 0.3, Centre -0.4, Top -0.7. Knob 2 sits at its Centre.
    node->params().set (0, 30.0f);
    node->params().set (1, 50.0f);
    node->params().set (maxMacros + 1, 1.0f);
    process();
    CHECK_THAT (left[0][0], WithinAbs (0.3f, 1.0e-6f)); // the first block jumps
    CHECK_THAT (right[0][blockSize - 1], WithinAbs (0.3f, 1.0e-6f));
    CHECK_THAT (left[1][blockSize - 1], WithinAbs (0.0f, 1.0e-6f));

    node->params().set (maxMacros, 1.0f);
    process();
    CHECK (left[0][0] > -0.4f); // ramps from 0.3
    CHECK_THAT (left[0][blockSize - 1], WithinAbs (-0.4f, 1.0e-6f));

    node->params().set (maxMacros, 2.0f);
    process();
    CHECK_THAT (left[0][blockSize - 1], WithinAbs (-0.7f, 1.0e-6f));

    // Bypassed, every knob is at its neutral position.
    GraphNode model;
    model.bypassed = true;
    node->configure (model);
    process();
    CHECK_THAT (left[0][blockSize - 1], WithinAbs (0.0f, 1.0e-6f));
}

// ---------------------------------------------------------------------------------------------------------
// Commands (MC5, MC7, MC9, MC10)

TEST_CASE ("mapping a parameter adds the Macros device and a macro slot", "[macros][commands]")
{
    Rig rig;
    rig.edit ("mapMacro", { { "nodeId", rig.gainId }, { "param", "amplitude" }, { "macro", 2 }, { "depth", 0.25 } });

    const auto& graph = rig.project.tracks[0]->graph;
    const auto& gain = graph.nodes[static_cast<size_t> (graph.indexOfNode (rig.gainId))];
    REQUIRE (gain.modulations.size() == 1);
    // Copies: the edits below replace the project this graph belongs to.
    const auto [port, slot] = *gain.modulations.begin();
    CHECK (slot.param == "amplitude");
    CHECK (slot.macro == 2);
    CHECK (slot.depth == 0.25);
    CHECK (std::ranges::none_of (graph.connections,
                                 [&] (const Connection& c) { return c.toNode == rig.gainId && c.toPort == port; }));
    std::ignore = rig.macrosId();

    // Mapping it again to the same knob changes the depth of the same slot; another knob takes another slot.
    rig.edit ("mapMacro", { { "nodeId", rig.gainId }, { "param", "amplitude" }, { "macro", 2 }, { "depth", -0.5 } });
    rig.edit ("mapMacro", { { "nodeId", rig.gainId }, { "param", "amplitude" }, { "macro", 3 } });
    const auto& after = rig.project.tracks[0]->graph;
    const auto& mapped = after.nodes[static_cast<size_t> (after.indexOfNode (rig.gainId))];
    REQUIRE (mapped.modulations.size() == 2);
    CHECK (mapped.modulations.at (port).depth == -0.5);
    CHECK (std::ranges::count (after.nodes, juce::String (nodetype::macros), &GraphNode::type) == 1);

    // Every knob on one parameter fills the pool of eight. A parameter that can't be modulated is refused.
    for (int knob = 4; knob < maxMacros; ++knob)
        rig.edit ("mapMacro", { { "nodeId", rig.gainId }, { "param", "amplitude" }, { "macro", knob } });

    rig.edit ("mapMacro", { { "nodeId", rig.gainId }, { "param", "amplitude" }, { "macro", 0 } });
    rig.edit ("mapMacro", { { "nodeId", rig.gainId }, { "param", "amplitude" }, { "macro", 1 } });
    const auto& full = rig.project.tracks[0]->graph;
    CHECK (full.nodes[static_cast<size_t> (full.indexOfNode (rig.gainId))].modulations.size() == maxModulationInputs);
    CHECK_THROWS_AS (
        apply (rig.project, "mapMacro",
               { { "trackId", rig.trackId }, { "nodeId", rig.macrosId() }, { "param", "macro1" }, { "macro", 1 } }),
        CommandError);

    // Removing the Macros device drops every macro slot.
    rig.edit ("removeNodes", { { "nodeIds", list ({ rig.macrosId() }) } });
    const auto& cleared = rig.project.tracks[0]->graph;
    CHECK (cleared.nodes[static_cast<size_t> (cleared.indexOfNode (rig.gainId))].modulations.empty());
}

TEST_CASE ("a track has one Macros device, in its own graph, with named knobs", "[macros][commands]")
{
    Rig rig;
    rig.edit ("addNode", { { "type", nodetype::macros } });
    CHECK_THROWS_AS (apply (rig.project, "addNode", { { "trackId", rig.trackId }, { "type", nodetype::macros } }),
                     CommandError);
    CHECK_FALSE (nodetype::allowedInVoiceGraph (nodetype::macros));
    CHECK_FALSE (nodetype::allowedInComponent (nodetype::macros));
    CHECK (nodetype::isModulationSource (nodetype::macros));

    rig.edit ("renameMacro", { { "nodeId", rig.macrosId() }, { "index", 2 }, { "name", "  Brightness " } });
    const auto& node = nodeOfType (rig.project.tracks[0]->graph, nodetype::macros);
    REQUIRE (node.macroNames.size() == 3);
    CHECK (node.macroNames[2] == "Brightness");
    CHECK (node.macroNames[0].isEmpty());
    CHECK_THROWS_AS (apply (rig.project, "renameMacro",
                            { { "trackId", rig.trackId }, { "nodeId", rig.gainId }, { "index", 0 }, { "name", "x" } }),
                     CommandError);

    // The knob is a parameter like any other: MIDI mappings and automation lanes take it.
    rig.project = apply (rig.project, "setMidiMapping",
                         { { "channel", 1 },
                           { "controller", 21 },
                           { "trackId", rig.trackId },
                           { "nodeId", rig.macrosId() },
                           { "param", "macro3" } });
    REQUIRE (rig.project.midiMappings.size() == 1);
    rig.edit ("addAutomationLane",
              { { "target", makeObject ({ { "kind", "node" }, { "nodeId", rig.macrosId() }, { "param", "macro3" } }) },
                { "points", list ({ list ({ 0, 0.5 }) }) } });
    CHECK (rig.project.tracks[0]->automation.size() == 1);
}

TEST_CASE ("macro slots and knob names round-trip through project files", "[macros][file]")
{
    Rig rig;
    rig.edit ("mapMacro", { { "nodeId", rig.gainId }, { "param", "amplitude" }, { "macro", 5 }, { "depth", 0.4 } });
    rig.edit ("renameMacro", { { "nodeId", rig.macrosId() }, { "index", 5 }, { "name", "Drive" } });

    std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> states;
    const auto loaded = projectfile::fromVar (projectfile::toVar (rig.project, { "id", "v" }, states),
                                              [] (const juce::String& /*path*/) { return nullptr; })
                            .project;
    const auto& graph = loaded.tracks[0]->graph;
    const auto& gain = graph.nodes[static_cast<size_t> (graph.indexOfNode (rig.gainId))];
    REQUIRE (gain.modulations.size() == 1);
    CHECK (gain.modulations.begin()->second.macro == 5);
    CHECK (gain.modulations.begin()->second.depth == 0.4);
    CHECK (nodeOfType (graph, nodetype::macros).macroNames.back() == "Drive");
}

// ---------------------------------------------------------------------------------------------------------
// The engine (MC6, MC8)

TEST_CASE ("a macro moves its targets around their own values, by its neutral position", "[macros][engine]")
{
    Rig rig;
    rig.edit ("mapMacro", { { "nodeId", rig.gainId }, { "param", "amplitude" }, { "macro", 0 }, { "depth", 0.25 } });

    // At the default Bottom neutral and 0 %, the amplitude is its own 100 %.
    CHECK_THAT (rig.render(), WithinAbs (level, 1.0e-4f));

    // All the way up: +50 % (a quarter of the 200 % range).
    rig.setKnob (0, 100.0);
    CHECK_THAT (rig.render(), WithinAbs (level * 1.5f, 1.0e-4f));

    // Centre neutral: no change at 50 %, -50 % at the bottom.
    rig.setKnob (0, 50.0, 1);
    CHECK_THAT (rig.render(), WithinAbs (level, 1.0e-4f));
    rig.setKnob (0, 0.0, 1);
    CHECK_THAT (rig.render(), WithinAbs (level * 0.5f, 1.0e-4f));

    // Top neutral: no change at 100 %.
    rig.setKnob (0, 100.0, 2);
    CHECK_THAT (rig.render(), WithinAbs (level, 1.0e-4f));

    // A manual change to the target is its new value to move around.
    rig.setKnob (0, 0.0, 2);
    rig.edit ("setNodeParam", { { "nodeId", rig.gainId }, { "param", "amplitude" }, { "value", 160.0 } });
    CHECK_THAT (rig.render(), WithinAbs (level * 1.1f, 1.0e-4f));
}

TEST_CASE ("a MIDI-mapped macro follows the controller's value", "[macros][engine][midi]")
{
    Rig rig;
    rig.edit ("mapMacro", { { "nodeId", rig.gainId }, { "param", "amplitude" }, { "macro", 1 }, { "depth", 0.5 } });
    rig.project = apply (rig.project, "setMidiMapping",
                         { { "channel", 1 },
                           { "controller", 74 },
                           { "trackId", rig.trackId },
                           { "nodeId", rig.macrosId() },
                           { "param", "macro2" } });
    REQUIRE (rig.project.midiMappings.size() == 1);

    // What the app does with CC 74 at 64 (MainComponent::applyMidiMapping): the knob's value, 64/127 of its range.
    const auto& mapping = rig.project.midiMappings[0];
    rig.edit ("setNodeParam",
              { { "nodeId", mapping.nodeId }, { "param", mapping.param }, { "value", 100.0 * 64 / 127 } });
    CHECK_THAT (rig.render(), WithinAbs (level * (1.0f + 64.0f / 127.0f), 1.0e-4f));
}

TEST_CASE ("a macro reaches a parameter inside a component", "[macros][engine][components]")
{
    Rig rig;
    rig.edit ("addNode", { { "type", nodetype::component } });
    const auto componentId = rig.project.tracks[0]->graph.nodes.back().id;
    const auto& inside = rig.project.tracks[0]->graph.nodes.back().component->graph;
    const auto input = nodeOfType (inside, nodetype::componentInput).id;
    const auto output = nodeOfType (inside, nodetype::componentOutput).id;

    // In front of the Gain: input -> component -> gain. Inside: a Gain between the component's ports.
    const auto& track = *rig.project.tracks[0];
    const auto feed =
        std::ranges::find_if (track.graph.connections, [&] (const Connection& c) { return c.toNode == rig.gainId; });
    REQUIRE (feed != track.graph.connections.end());
    const auto from = *feed;
    rig.edit ("disconnect", { { "fromNode", from.fromNode },
                              { "fromPort", from.fromPort },
                              { "toNode", from.toNode },
                              { "toPort", from.toPort } });
    rig.edit (
        "connect",
        { { "fromNode", from.fromNode }, { "fromPort", from.fromPort }, { "toNode", componentId }, { "toPort", 0 } });
    rig.edit ("connect", { { "fromNode", componentId }, { "fromPort", 0 }, { "toNode", rig.gainId }, { "toPort", 0 } });
    rig.edit ("disconnect", { { "voiceNodeId", componentId },
                              { "fromNode", input },
                              { "fromPort", 0 },
                              { "toNode", output },
                              { "toPort", 0 } });
    rig.edit ("addNode", { { "voiceNodeId", componentId }, { "type", nodetype::gain } });
    const auto innerGain = rig.project.tracks[0]->graph.nodes.back().component->graph.nodes.back().id;
    rig.edit ("connect", { { "voiceNodeId", componentId },
                           { "fromNode", input },
                           { "fromPort", 0 },
                           { "toNode", innerGain },
                           { "toPort", 0 } });
    rig.edit ("connect", { { "voiceNodeId", componentId },
                           { "fromNode", innerGain },
                           { "fromPort", 0 },
                           { "toNode", output },
                           { "toPort", 0 } });
    CHECK_THAT (rig.render(), WithinAbs (level, 1.0e-4f));

    rig.edit ("mapMacro", { { "nodeId", innerGain }, { "param", "amplitude" }, { "macro", 0 }, { "depth", -0.25 } });
    rig.setKnob (0, 100.0);
    CHECK_THAT (rig.render(), WithinAbs (level * 0.5f, 1.0e-4f));
}

namespace
{
constexpr auto samplesPerBeat = 24000; // 120 bpm at 48 kHz

// A track whose synth became a Voice Instrument, its oscillator a sine, playing A4.
struct VoiceRig
{
    VoiceRig()
    {
        auto setup = projectWithClip();
        trackId = setup.trackId;
        project = apply (setup.project, "addNotes",
                         { { "clipId", setup.clipId }, { "notes", list ({ noteArgs (0, 8 * ticksPerBeat, 69) }) } });
        voiceId = nodeOfType (project.tracks[0]->graph, nodetype::mpeSynth).id;
        project = apply (project, "convertToVoiceGraph", { { "trackId", trackId }, { "nodeId", voiceId } });
        const auto& graph = project.tracks[0]->graph;
        oscillatorId = nodeOfType (*graph.nodes[static_cast<size_t> (graph.indexOfNode (voiceId))].voiceGraph,
                                   nodetype::oscillator)
                           .id;
        project =
            apply (project, "setNodeParam",
                   { { "trackId", trackId }, { "nodeId", oscillatorId }, { "param", "waveform" }, { "value", 0.0 } });
    }

    [[nodiscard]] double frequency (int numSamples)
    {
        if (auto compiled = compiler.update (project, spec); compiled != nullptr)
            engine.setEngine (compiled);

        engine.play();
        const auto output = render (engine, numSamples).left();
        const auto from = static_cast<size_t> (numSamples / 2);
        return static_cast<double> (zeroCrossings (output, from, from + samplesPerBeat / 2)) / 2.0 / 0.25;
    }

    Project project;
    ObjectId trackId = 0;
    ObjectId voiceId = 0;
    ObjectId oscillatorId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

TEST_CASE ("a macro reaches every voice of a Voice Instrument", "[macros][engine][voice]")
{
    VoiceRig rig;
    // Coarse tuning is -48..48 semitones: 12/96 moves it an octave.
    rig.project = apply (rig.project, "mapMacro",
                         { { "trackId", rig.trackId },
                           { "nodeId", rig.oscillatorId },
                           { "param", "coarse" },
                           { "macro", 0 },
                           { "depth", 0.125 } });
    CHECK_THAT (rig.frequency (samplesPerBeat), WithinAbs (440.0, 6.0));

    const auto macros = nodeOfType (rig.project.tracks[0]->graph, nodetype::macros).id;
    rig.project =
        apply (rig.project, "setNodeParam",
               { { "trackId", rig.trackId }, { "nodeId", macros }, { "param", "macro1" }, { "value", 100.0 } });
    CHECK_THAT (rig.frequency (samplesPerBeat), WithinAbs (880.0, 8.0));
}

TEST_CASE ("macros render without allocating", "[macros][engine][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    VoiceRig rig;
    rig.project =
        apply (rig.project, "mapMacro",
               { { "trackId", rig.trackId }, { "nodeId", rig.oscillatorId }, { "param", "coarse" }, { "macro", 0 } });
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));
    rig.engine.play();
    std::ignore = render (rig.engine, blockSize); // lazy setup outside the guard

    const auto before = allocationViolationCount();
    StereoBuffer buffer (blockSize);

    {
        const ScopedNoAllocation guard;

        for (int block = 0; block < 16; ++block)
            rig.engine.renderOffline (buffer.channels(), 2, blockSize, sampleRate);
    }

    CHECK (allocationViolationCount() == before);
}
