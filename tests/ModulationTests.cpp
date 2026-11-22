#include "TestUtils.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/PluginNode.h"
#include "dawg/ProjectFile.h"
#include "dawg/Sequence.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Modulation routing (docs/design/modulation.md): sources feed a target node's modulation input pool, and
// the engine applies each slot around the parameter's current base value once per block.

namespace
{
constexpr float level = 0.25f;
constexpr int frames = 30000;
constexpr size_t probe = 4000;

[[nodiscard]] juce::var list (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

[[nodiscard]] juce::var point (Ticks time, double value, int shape = 0)
{
    return list ({ time, value, shape });
}

[[nodiscard]] juce::var pluginArgs (const char* name)
{
    return makeObject (
        { { "identifier", juce::String ("modulation-test-") + name }, { "name", name }, { "format", "VST3" } });
}

[[nodiscard]] const GraphNode& nodeOfType (const Track& track, const char* type)
{
    const auto it = std::ranges::find_if (track.graph.nodes, [type] (const GraphNode& n) { return n.type == type; });
    REQUIRE (it != track.graph.nodes.end());
    return *it;
}

[[nodiscard]] bool connected (const TrackGraph& graph, ObjectId from, int fromPort, ObjectId to, int toPort)
{
    return std::ranges::find (graph.connections, Connection { from, fromPort, to, toPort }) != graph.connections.end();
}

// An audio track that always monitors a constant input, so the level at the output is known.
struct Rig
{
    Rig()
        : input (frames, level)
    {
        project = apply ({}, "addTrack", { { "kind", "audio" } });
        trackId = project.tracks[0]->id;
        project = apply (project, "setTrackSwitches", { { "id", trackId }, { "inputChannel", 0 }, { "monitor", 2 } });
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);
    }

    [[nodiscard]] StereoBuffer render()
    {
        if (auto compiled = compiler.update (project, spec); compiled != nullptr)
            engine.setEngine (compiled);

        engine.play();
        const std::array<const float*, 1> inputs { input.data() };
        StereoBuffer out (frames);
        engine.renderOffline (out.channels(), 2, frames, sampleRate, inputs.data(), 1);
        return out;
    }

    std::vector<float> input;
    Project project;
    ObjectId trackId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};

[[nodiscard]] float peak (const std::vector<float>& samples, size_t from, size_t to)
{
    float result = 0.0f;

    for (auto i = from; i < std::min (to, samples.size()); ++i)
        result = std::max (result, std::abs (samples[i]));

    return result;
}

void setParam (Node& node, const char* id, float value)
{
    const auto index = node.descriptor().indexOfParam (id);
    REQUIRE (index >= 0);
    node.params().set (static_cast<size_t> (index), value);
}

struct SourceHarness
{
    explicit SourceHarness (const char* type)
        : node (NodeRegistry::builtin().create (type))
    {
        REQUIRE (node != nullptr);
        node->prepare (directSpec);
    }

    static constexpr ProcessSpec directSpec { sampleRate, 20000 };

    std::unique_ptr<Node> node;
};

[[nodiscard]] Transport transportAt (double startTick, int numSamples)
{
    Transport transport;
    transport.playing = true;
    transport.startTick = startTick;
    transport.endTick = startTick + static_cast<double> (numSamples) * ticksPerBeat / 24000.0;
    transport.timeMap = TimeMap (constantTempo(), sampleRate);
    return transport;
}

void processSource (Node& node, std::vector<float>& left, std::vector<float>& right, const Transport& transport,
                    const EventBuffer* events, const std::array<AudioInput, 2>* inputs)
{
    std::array<AudioOutput, 1> outputs { AudioOutput { left.data(), right.data() } };
    ProcessContext context;
    context.numSamples = static_cast<int> (left.size());
    context.audioOutputs = outputs;
    context.eventsIn = events;
    context.transport = &transport;

    if (inputs != nullptr)
        context.audioInputs = *inputs;

    node.process (context);
}

// A parameterised effect plugin: its first parameter is the gain it applies.
class ParameterProcessor final : public juce::AudioProcessor
{
public:
    ParameterProcessor()
        : juce::AudioProcessor (BusesProperties()
                                    .withInput ("In", juce::AudioChannelSet::stereo())
                                    .withOutput ("Out", juce::AudioChannelSet::stereo()))
    {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the processor takes ownership of the parameter
        addParameter (new juce::AudioParameterFloat ("gain", "Gain", 0.0f, 1.0f, 0.5f));
    }

    const juce::String getName() const override { return "Parameter"; }
    void prepareToPlay (double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        buffer.applyGain (getParameters()[0]->getValue());
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int /*index*/) override {}
    const juce::String getProgramName (int /*index*/) override { return {}; }
    void changeProgramName (int /*index*/, const juce::String& /*newName*/) override {}
    void getStateInformation (juce::MemoryBlock& /*destData*/) override {}
    void setStateInformation (const void* /*data*/, int /*sizeInBytes*/) override {}
};
} // namespace

// ---------------------------------------------------------------------------------------------------------
// Commands and model

TEST_CASE ("modulation slots validate their port and target, and go with their connection", "[modulation][commands]")
{
    Rig rig;
    const auto gain = nodeOfType (*rig.project.tracks[0], nodetype::gain).id;
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
    const auto lfo = rig.project.tracks[0]->graph.nodes.back().id;

    CHECK_THROWS_AS (
        apply (rig.project, "setModulation",
               { { "trackId", rig.trackId }, { "nodeId", gain }, { "port", 0 }, { "param", "amplitude" } }),
        CommandError);
    CHECK_THROWS_AS (apply (rig.project, "setModulation",
                            { { "trackId", rig.trackId }, { "nodeId", gain }, { "port", 1 }, { "param", "nope" } }),
                     CommandError);

    // The gain in dB is a trim, not a modulation target.
    CHECK_THROWS_AS (apply (rig.project, "setModulation",
                            { { "trackId", rig.trackId }, { "nodeId", gain }, { "port", 1 }, { "param", "gainDb" } }),
                     CommandError);

    rig.edit (
        "connect",
        { { "trackId", rig.trackId }, { "fromNode", lfo }, { "fromPort", 0 }, { "toNode", gain }, { "toPort", 1 } });
    rig.edit ("setModulation", { { "trackId", rig.trackId },
                                 { "nodeId", gain },
                                 { "port", 1 },
                                 { "param", "amplitude" },
                                 { "depth", 0.25 },
                                 { "active", false } });

    const auto& target = nodeOfType (*rig.project.tracks[0], nodetype::gain);
    REQUIRE (target.modulations.contains (1));
    CHECK (target.modulations.at (1).param == "amplitude");
    CHECK_THAT (target.modulations.at (1).depth, WithinAbs (0.25, 1e-9));
    CHECK_FALSE (target.modulations.at (1).active);

    // A disconnect removes the slot; an empty parameter removes it too.
    rig.edit (
        "disconnect",
        { { "trackId", rig.trackId }, { "fromNode", lfo }, { "fromPort", 0 }, { "toNode", gain }, { "toPort", 1 } });
    CHECK_FALSE (nodeOfType (*rig.project.tracks[0], nodetype::gain).modulations.contains (1));

    rig.edit (
        "connect",
        { { "trackId", rig.trackId }, { "fromNode", lfo }, { "fromPort", 0 }, { "toNode", gain }, { "toPort", 1 } });
    rig.edit ("setModulation",
              { { "trackId", rig.trackId }, { "nodeId", gain }, { "port", 1 }, { "param", "amplitude" } });
    CHECK_THAT (nodeOfType (*rig.project.tracks[0], nodetype::gain).modulations.at (1).depth, WithinAbs (0.0, 1e-9));
    rig.edit ("setModulation", { { "trackId", rig.trackId }, { "nodeId", gain }, { "port", 1 }, { "param", "" } });
    CHECK_FALSE (nodeOfType (*rig.project.tracks[0], nodetype::gain).modulations.contains (1));

    // Removing the source removes its edge and slot.
    rig.edit ("setModulation",
              { { "trackId", rig.trackId }, { "nodeId", gain }, { "port", 1 }, { "param", "amplitude" } });
    rig.edit ("removeNode", { { "trackId", rig.trackId }, { "nodeId", lfo } });
    CHECK_FALSE (nodeOfType (*rig.project.tracks[0], nodetype::gain).modulations.contains (1));
}

TEST_CASE ("chain edits preserve modulation edges and slots", "[modulation][commands]")
{
    Rig rig;
    const auto gain = nodeOfType (*rig.project.tracks[0], nodetype::gain).id;
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
    const auto lfo = rig.project.tracks[0]->graph.nodes.back().id;
    rig.edit (
        "connect",
        { { "trackId", rig.trackId }, { "fromNode", lfo }, { "fromPort", 0 }, { "toNode", gain }, { "toPort", 1 } });
    rig.edit ("setModulation",
              { { "trackId", rig.trackId }, { "nodeId", gain }, { "port", 1 }, { "param", "amplitude" } });

    // Inserting an effect rewires the chain, but modulation edges are not chain edges.
    rig.edit ("insertNode",
              { { "trackId", rig.trackId }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("Chain") } });
    CHECK (connected (rig.project.tracks[0]->graph, lfo, 0, gain, 1));
    CHECK (nodeOfType (*rig.project.tracks[0], nodetype::gain).modulations.contains (1));

    // Reordering and removing chain nodes leaves it alone too.
    const auto effectId = nodeOfType (*rig.project.tracks[0], nodetype::pluginEffect).id;
    rig.edit ("reorderNode", { { "trackId", rig.trackId }, { "nodeId", effectId }, { "index", 0 } });
    CHECK (connected (rig.project.tracks[0]->graph, lfo, 0, gain, 1));

    rig.edit ("removeNode", { { "trackId", rig.trackId }, { "nodeId", effectId } });
    CHECK (connected (rig.project.tracks[0]->graph, lfo, 0, gain, 1));
    CHECK (nodeOfType (*rig.project.tracks[0], nodetype::gain).modulations.contains (1));
}

TEST_CASE ("modulation slots and LFO shapes round-trip through project files", "[modulation][file]")
{
    Rig rig;
    const auto gain = nodeOfType (*rig.project.tracks[0], nodetype::gain).id;
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
    const auto lfo = rig.project.tracks[0]->graph.nodes.back().id;
    rig.edit ("setModulationShape", { { "trackId", rig.trackId },
                                      { "nodeId", lfo },
                                      { "points", list ({ point (0, 0.0), point (modulationShapeTicks / 2, 1.0),
                                                          point (modulationShapeTicks, 0.0) }) } });
    rig.edit (
        "connect",
        { { "trackId", rig.trackId }, { "fromNode", lfo }, { "fromPort", 0 }, { "toNode", gain }, { "toPort", 1 } });
    rig.edit ("setModulation", { { "trackId", rig.trackId },
                                 { "nodeId", gain },
                                 { "port", 1 },
                                 { "param", "amplitude" },
                                 { "depth", -0.35 },
                                 { "active", false } });

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("dawg-modulation-" + juce::Uuid().toString() + ".dawg");
    REQUIRE (projectfile::write (rig.project, { "id", "v" }, file, false).isEmpty());
    const auto loaded = projectfile::read (file).project;
    std::ignore = file.deleteFile();

    CHECK (juce::JSON::toString (toVar (loaded)) == juce::JSON::toString (toVar (rig.project)));
    const auto& loadedLfo = nodeOfType (*loaded.tracks[0], nodetype::lfo);
    REQUIRE (loadedLfo.modulationShape.size() == 3);
    CHECK (loadedLfo.modulationShape[1].value == 1.0);

    const auto& loadedGain = nodeOfType (*loaded.tracks[0], nodetype::gain);
    REQUIRE (loadedGain.modulations.contains (1));
    CHECK (loadedGain.modulations.at (1).param == "amplitude");
    CHECK_THAT (loadedGain.modulations.at (1).depth, WithinAbs (-0.35, 1e-9));
    CHECK_FALSE (loadedGain.modulations.at (1).active);

    // A slot keyed by anything but a port index is a damaged file, not port 0.
    std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> states;
    auto damaged = projectfile::toVar (rig.project, { "id", "v" }, states);
    auto* slots = damaged["project"]["tracks"][juce::Identifier (juce::String (rig.trackId))]["graph"]["nodes"]
                         [juce::Identifier (juce::String (gain))]["modulations"]
                             .getDynamicObject();
    REQUIRE (slots != nullptr);
    slots->setProperty ("x", slots->getProperty ("1"));
    CHECK_THROWS_AS (projectfile::fromVar (damaged, [] (const juce::String& /*path*/) { return nullptr; }),
                     CommandError);

    CHECK_THROWS_AS (apply (rig.project, "setModulationShape",
                            { { "trackId", rig.trackId }, { "nodeId", gain }, { "points", juce::Array<juce::var>() } }),
                     CommandError);
    CHECK_THROWS_AS (
        apply (rig.project, "setModulationShape",
               { { "trackId", rig.trackId }, { "nodeId", lfo }, { "points", list ({ point (-1, 0.0) }) } }),
        CommandError);
}

// ---------------------------------------------------------------------------------------------------------
// Sources

// The LFO's cycle choices (ModulationNodes.cpp): a quarter note, and an eighth-note triplet.
constexpr float quarterNoteCycle = 13.0f;
constexpr float eighthTripletCycle = 8.0f;

TEST_CASE ("the LFO follows its phase, rate and tempo sync", "[modulation][dsp]")
{
    SourceHarness freeLfo { nodetype::lfo };
    setParam (*freeLfo.node, "sync", 0.0f);
    setParam (*freeLfo.node, "rateMs", 1000.0f);
    setParam (*freeLfo.node, "phase", 0.0f);
    freeLfo.node->prepare (SourceHarness::directSpec);

    std::vector<float> left (12001);

    std::vector<float> right (12001);
    processSource (*freeLfo.node, left, right, transportAt (0.0, 12001), nullptr, nullptr);
    CHECK_THAT (left[0], WithinAbs (0.0, 1e-5));
    CHECK_THAT (left[12000], WithinAbs (1.0, 1e-4));

    SourceHarness synced { nodetype::lfo };
    setParam (*synced.node, "sync", 1.0f);
    setParam (*synced.node, "cycle", quarterNoteCycle);
    setParam (*synced.node, "retrigger", 0.0f);
    setParam (*synced.node, "phase", 0.0f);
    synced.node->prepare (SourceHarness::directSpec);

    // At a quarter beat into the timeline a one-beat sine cycle is at its peak, whatever the phase parameter.
    std::vector<float> syncedLeft (64);
    std::vector<float> syncedRight (64);
    processSource (*synced.node, syncedLeft, syncedRight, transportAt (static_cast<double> (ticksPerBeat) / 4.0, 64),
                   nullptr, nullptr);
    CHECK_THAT (syncedLeft[0], WithinAbs (1.0, 1e-4));
}

TEST_CASE ("a free-running LFO starts at its phase when the phase is set after prepare", "[modulation][dsp]")
{
    // The graph compiler prepares a new node first and applies its parameters afterwards.
    SourceHarness lfo { nodetype::lfo };
    lfo.node->prepare (SourceHarness::directSpec);
    setParam (*lfo.node, "sync", 0.0f);
    setParam (*lfo.node, "rateMs", 1000.0f);
    setParam (*lfo.node, "phase", 0.25f);

    // A quarter of the way into the sine, the LFO is at its peak.
    std::vector<float> left (1);
    std::vector<float> right (1);
    processSource (*lfo.node, left, right, transportAt (0.0, 1), nullptr, nullptr);
    CHECK_THAT (left[0], WithinAbs (1.0, 1e-4));
}

TEST_CASE ("the LFO cycle is a note length, and no shape plays the Sine preset", "[modulation][dsp]")
{
    SourceHarness lfo { nodetype::lfo };
    setParam (*lfo.node, "sync", 1.0f);
    setParam (*lfo.node, "cycle", eighthTripletCycle);
    setParam (*lfo.node, "retrigger", 0.0f);
    lfo.node->prepare (SourceHarness::directSpec);

    // An eighth-note triplet is a third of a beat: a quarter of that cycle in, the shape is at its peak.
    std::vector<float> left (1);
    std::vector<float> right (1);
    processSource (*lfo.node, left, right, transportAt (static_cast<double> (ticksPerBeat) / 12.0, 1), nullptr,
                   nullptr);
    CHECK_THAT (left[0], WithinAbs (1.0, 1e-4));

    // An eighth of the cycle in, it plays the four-segment Sine preset, which stays close to a true sine.
    processSource (*lfo.node, left, right, transportAt (static_cast<double> (ticksPerBeat) / 24.0, 1), nullptr,
                   nullptr);
    const auto preset = evaluateCurve (lfoSineShape, static_cast<double> (modulationShapeTicks) / 8.0, 0.0);
    CHECK_THAT (left[0], WithinAbs (preset, 1e-4));
    CHECK_THAT (preset, WithinAbs (std::sin (juce::MathConstants<double>::pi / 4.0), 0.013));
}

TEST_CASE ("the LFO maps its shape around the selected zero position", "[modulation][dsp]")
{
    const auto atZero = [] (int zero)
    {
        SourceHarness lfo { nodetype::lfo };
        GraphNode config;
        config.type = nodetype::lfo;
        config.modulationShape = { CurvePoint { 0, 0.0, CurveShape::linear, 0.0 },
                                   CurvePoint { modulationShapeTicks, 0.0, CurveShape::linear, 0.0 } };
        lfo.node->configure (config);
        setParam (*lfo.node, "sync", 0.0f);
        setParam (*lfo.node, "rateMs", 1000.0f);
        setParam (*lfo.node, "phase", 0.0f);
        setParam (*lfo.node, "zero", static_cast<float> (zero));
        lfo.node->prepare (SourceHarness::directSpec);

        std::vector<float> left (1);

        std::vector<float> right (1);
        processSource (*lfo.node, left, right, transportAt (0.0, 1), nullptr, nullptr);
        return left[0];
    };

    CHECK_THAT (atZero (0), WithinAbs (-0.5, 1e-6));
    CHECK_THAT (atZero (1), WithinAbs (0.0, 1e-6));
    CHECK_THAT (atZero (2), WithinAbs (0.5, 1e-6));
}

TEST_CASE ("a ramp-down LFO jumps back to the top in one sample", "[modulation][dsp]")
{
    // The Ramp preset at 250 ms a cycle, 0 at the bottom: the cycle restarts at sample 12000.
    SourceHarness lfo { nodetype::lfo };
    GraphNode config;
    config.type = nodetype::lfo;
    config.modulationShape = { CurvePoint { 0, 1.0, CurveShape::linear, 0.0 },
                               CurvePoint { modulationShapeTicks, -1.0, CurveShape::linear, 0.0 } };
    lfo.node->configure (config);
    setParam (*lfo.node, "sync", 0.0f);
    setParam (*lfo.node, "rateMs", 250.0f);
    setParam (*lfo.node, "zero", 2.0f);
    lfo.node->prepare (SourceHarness::directSpec);

    std::vector<float> left (12100);
    std::vector<float> right (12100);
    processSource (*lfo.node, left, right, transportAt (0.0, 12100), nullptr, nullptr);

    const auto jump = std::ranges::adjacent_find (left, [] (float a, float b) { return b - a > 0.99f; });
    REQUIRE (jump != left.end());
    CHECK (std::abs (std::distance (left.begin(), jump) - 11999) <= 1);
}

TEST_CASE ("the ADSR follows note and audio triggers", "[modulation][dsp]")
{
    SourceHarness midiAdsr { nodetype::adsr };
    setParam (*midiAdsr.node, "attackMs", 0.0f);
    setParam (*midiAdsr.node, "decayMs", 0.0f);
    setParam (*midiAdsr.node, "sustain", 100.0f);
    setParam (*midiAdsr.node, "releaseMs", 1.0f);
    midiAdsr.node->prepare (SourceHarness::directSpec);

    EventBuffer events;
    REQUIRE (events.add ({ 0, NoteEventType::noteOn, 1, nullptr }));
    REQUIRE (events.add ({ 100, NoteEventType::noteOff, 1, nullptr }));

    std::vector<float> midiLeft (200);

    std::vector<float> midiRight (200);
    processSource (*midiAdsr.node, midiLeft, midiRight, transportAt (0.0, 200), &events, nullptr);
    CHECK_THAT (midiLeft[50], WithinAbs (1.0, 1e-4));
    CHECK_THAT (midiLeft[150], WithinAbs (0.0, 1e-4));

    SourceHarness audioAdsr { nodetype::adsr };
    setParam (*audioAdsr.node, "attackMs", 0.0f);
    setParam (*audioAdsr.node, "decayMs", 0.0f);
    setParam (*audioAdsr.node, "sustain", 100.0f);
    setParam (*audioAdsr.node, "releaseMs", 1.0f);
    audioAdsr.node->prepare (SourceHarness::directSpec);

    std::vector<float> trigger (200, 0.0f);
    std::fill_n (trigger.begin(), 100, 0.5f);
    const std::array<AudioInput, 2> inputs { AudioInput { nullptr, nullptr },
                                             AudioInput { trigger.data(), trigger.data() } };

    std::vector<float> audioLeft (200);

    std::vector<float> audioRight (200);
    processSource (*audioAdsr.node, audioLeft, audioRight, transportAt (0.0, 200), nullptr, &inputs);
    CHECK_THAT (audioLeft[50], WithinAbs (1.0, 1e-4));
    CHECK_THAT (audioLeft[150], WithinAbs (0.0, 1e-4));
}

TEST_CASE ("the ADSR's handles bend its segments", "[modulation][dsp]")
{
    // Each segment lasts 10 ms (480 samples) and is checked halfway through.
    SourceHarness adsr { nodetype::adsr };
    setParam (*adsr.node, "attackMs", 10.0f);
    setParam (*adsr.node, "decayMs", 10.0f);
    setParam (*adsr.node, "sustain", 50.0f);
    setParam (*adsr.node, "releaseMs", 10.0f);
    setParam (*adsr.node, "attackHandleTime", 0.0f); // a fast start
    setParam (*adsr.node, "attackHandleLevel", 100.0f);
    setParam (*adsr.node, "releaseHandleTime", 100.0f); // a slow start
    setParam (*adsr.node, "releaseHandleLevel", 0.0f);
    adsr.node->prepare (SourceHarness::directSpec);

    EventBuffer events;
    REQUIRE (events.add ({ 0, NoteEventType::noteOn, 1, nullptr }));
    REQUIRE (events.add ({ 1200, NoteEventType::noteOff, 1, nullptr }));

    std::vector<float> left (2000);
    std::vector<float> right (2000);
    processSource (*adsr.node, left, right, transportAt (0.0, 2000), &events, nullptr);

    CHECK_THAT (left[239], WithinAbs (freeSegment (0.5, 0.0, 1.0), 1e-3));
    CHECK_THAT (left[719], WithinAbs (0.75, 1e-3)); // the decay is straight by default
    CHECK_THAT (left[1100], WithinAbs (0.5, 1e-4));
    CHECK_THAT (left[1439], WithinAbs (0.5 * (1.0 - freeSegment (0.5, 1.0, 0.0)), 1e-3));
    CHECK_THAT (left[1800], WithinAbs (0.0, 1e-4));
    CHECK (left[239] > 0.75f);
    CHECK (left[1439] > 0.4f);
}

// ---------------------------------------------------------------------------------------------------------
// Applying modulation

TEST_CASE ("an LFO modulates a built-in parameter around its manual value", "[modulation][engine]")
{
    Rig rig;
    const auto gain = nodeOfType (*rig.project.tracks[0], nodetype::gain).id;
    rig.edit ("setNodeParam",
              { { "trackId", rig.trackId }, { "nodeId", gain }, { "param", "amplitude" }, { "value", 50.0 } });

    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.5f, 1e-4));

    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
    const auto lfo = rig.project.tracks[0]->graph.nodes.back().id;
    rig.edit ("setModulationShape", { { "trackId", rig.trackId },
                                      { "nodeId", lfo },
                                      { "points", list ({ point (0, 1.0), point (modulationShapeTicks, 1.0) }) } });
    rig.edit (
        "connect",
        { { "trackId", rig.trackId }, { "fromNode", lfo }, { "fromPort", 0 }, { "toNode", gain }, { "toPort", 1 } });
    rig.edit (
        "setModulation",
        { { "trackId", rig.trackId }, { "nodeId", gain }, { "port", 1 }, { "param", "amplitude" }, { "depth", 0.1 } });

    // The amplitude's range is 200 %, so a depth of 0.1 adds 20 % around 50 %.
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.7f, 1e-4));

    // A manual change is adopted as the new base.
    rig.edit ("setNodeParam",
              { { "trackId", rig.trackId }, { "nodeId", gain }, { "param", "amplitude" }, { "value", 30.0 } });
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.5f, 1e-4));
}

TEST_CASE ("modulation moves a gain's amplitude both ways, up to double and down to silence", "[modulation][engine]")
{
    Rig rig;
    const auto gain = nodeOfType (*rig.project.tracks[0], nodetype::gain).id;
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
    const auto lfo = rig.project.tracks[0]->graph.nodes.back().id;
    rig.edit (
        "connect",
        { { "trackId", rig.trackId }, { "fromNode", lfo }, { "fromPort", 0 }, { "toNode", gain }, { "toPort", 1 } });
    rig.edit (
        "setModulation",
        { { "trackId", rig.trackId }, { "nodeId", gain }, { "port", 1 }, { "param", "amplitude" }, { "depth", 0.5 } });

    // Around the default 100 %, half the 200 % range each way.
    for (const auto& [value, amplitude] : { std::pair { 1.0, 2.0f }, { -1.0, 0.0f }, { 0.25, 1.25f } })
    {
        rig.edit ("setModulationShape",
                  { { "trackId", rig.trackId },
                    { "nodeId", lfo },
                    { "points", list ({ point (0, value), point (modulationShapeTicks, value) }) } });
        INFO ("source " << value);
        CHECK_THAT (rig.render().left()[probe], WithinAbs (level * amplitude, 1e-4));
    }
}

TEST_CASE ("a modulation's depth changes without a recompile", "[modulation][engine]")
{
    Rig rig;
    const auto gain = nodeOfType (*rig.project.tracks[0], nodetype::gain).id;
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
    const auto lfo = rig.project.tracks[0]->graph.nodes.back().id;
    rig.edit (
        "connect",
        { { "trackId", rig.trackId }, { "fromNode", lfo }, { "fromPort", 0 }, { "toNode", gain }, { "toPort", 1 } });
    rig.edit (
        "setModulation",
        { { "trackId", rig.trackId }, { "nodeId", gain }, { "port", 1 }, { "param", "amplitude" }, { "depth", 0.5 } });
    rig.edit ("setModulationShape", { { "trackId", rig.trackId },
                                      { "nodeId", lfo },
                                      { "points", list ({ point (0, 1.0), point (modulationShapeTicks, 1.0) }) } });
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 2.0f, 1e-4));

    // A depth drag only stores the depth: no new engine, and the next block plays it.
    for (const auto& [depth, amplitude] : { std::pair { 0.25, 1.5f }, { -0.5, 0.0f }, { 0.0, 1.0f } })
    {
        rig.edit ("setModulation", { { "trackId", rig.trackId },
                                     { "nodeId", gain },
                                     { "port", 1 },
                                     { "param", "amplitude" },
                                     { "depth", depth } });
        INFO ("depth " << depth);
        CHECK (rig.compiler.update (rig.project, spec) == nullptr);
        CHECK_THAT (rig.render().left()[probe], WithinAbs (level * amplitude, 1e-4));
    }

    // Turning the slot off does recompile, and plays no modulation.
    rig.edit ("setModulation", { { "trackId", rig.trackId },
                                 { "nodeId", gain },
                                 { "port", 1 },
                                 { "param", "amplitude" },
                                 { "depth", 0.5 },
                                 { "active", false } });
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level, 1e-4));
}

TEST_CASE ("slots targeting one parameter sum before it is written", "[modulation][engine]")
{
    Rig rig;
    const auto gain = nodeOfType (*rig.project.tracks[0], nodetype::gain).id;
    rig.edit ("setNodeParam",
              { { "trackId", rig.trackId }, { "nodeId", gain }, { "param", "amplitude" }, { "value", 50.0 } });

    for (const auto port : { 1, 2 })
    {
        rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
        const auto lfo = rig.project.tracks[0]->graph.nodes.back().id;
        rig.edit ("setModulationShape", { { "trackId", rig.trackId },
                                          { "nodeId", lfo },
                                          { "points", list ({ point (0, 1.0), point (modulationShapeTicks, 1.0) }) } });
        rig.edit ("connect", { { "trackId", rig.trackId },
                               { "fromNode", lfo },
                               { "fromPort", 0 },
                               { "toNode", gain },
                               { "toPort", port } });
        rig.edit ("setModulation", { { "trackId", rig.trackId },
                                     { "nodeId", gain },
                                     { "port", port },
                                     { "param", "amplitude" },
                                     { "depth", 0.05 } });
    }

    // Two slots of 0.05 add 20 % in total, the same as one slot of 0.1.
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.7f, 1e-4));
}

TEST_CASE ("an ADSR triggered by notes opens a gain like a VCA", "[modulation][engine]")
{
    auto setup = projectWithClip();
    auto project = apply (setup.project, "addNotes",
                          { { "clipId", setup.clipId }, { "notes", list ({ noteArgs (0, ticksPerBeat, 60) }) } });
    const auto sequencer = nodeOfType (*project.tracks[0], nodetype::sequencer).id;
    const auto gain = nodeOfType (*project.tracks[0], nodetype::gain).id;
    project = apply (project, "setNodeParam",
                     { { "trackId", setup.trackId }, { "nodeId", gain }, { "param", "amplitude" }, { "value", 0.0 } });
    project = apply (project, "addNode", { { "trackId", setup.trackId }, { "type", nodetype::adsr } });
    const auto adsr = project.tracks[0]->graph.nodes.back().id;

    project = apply (project, "connect",
                     { { "trackId", setup.trackId },
                       { "fromNode", sequencer },
                       { "fromPort", 0 },
                       { "toNode", adsr },
                       { "toPort", 0 } });
    project = apply (
        project, "connect",
        { { "trackId", setup.trackId }, { "fromNode", adsr }, { "fromPort", 0 }, { "toNode", gain }, { "toPort", 1 } });
    project = apply (project, "setNodeParam",
                     { { "trackId", setup.trackId }, { "nodeId", adsr }, { "param", "attackMs" }, { "value", 0.0 } });
    project = apply (project, "setNodeParam",
                     { { "trackId", setup.trackId }, { "nodeId", adsr }, { "param", "decayMs" }, { "value", 0.0 } });
    project = apply (project, "setNodeParam",
                     { { "trackId", setup.trackId }, { "nodeId", adsr }, { "param", "sustain" }, { "value", 100.0 } });
    project = apply (project, "setNodeParam",
                     { { "trackId", setup.trackId }, { "nodeId", adsr }, { "param", "releaseMs" }, { "value", 1.0 } });
    project = apply (project, "setModulation",
                     { { "trackId", setup.trackId },
                       { "nodeId", gain },
                       { "port", 1 },
                       { "param", "amplitude" },
                       { "depth", 0.5 } });

    GraphCompiler compiler;
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    engine.play();
    const auto output = render (engine, 2 * 24000);

    // The note opens the envelope from 0 % to 100 % (a depth of 0.5 of the 200 % range); after it releases the gain
    // closes again.
    const auto open = peak (output.left(), 500, 2500);
    const auto closed = peak (output.left(), 30000, 32000);
    CHECK (open > 0.01f);
    CHECK (closed < 1.0e-6f);
}

TEST_CASE ("a plugin parameter can be modulated", "[modulation][engine][plugins]")
{
    Rig rig;
    rig.edit (
        "insertNode",
        { { "trackId", rig.trackId }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("Modulated") } });
    const auto pluginId = nodeOfType (*rig.project.tracks[0], nodetype::pluginEffect).id;

    // Compile once so the plugin node exists, then attach a processor with an automatable gain parameter.
    std::ignore = rig.compiler.compile (rig.project, spec);
    auto* plugin = dynamic_cast<PluginNode*> (rig.compiler.findInstance (rig.trackId, pluginId));
    REQUIRE (plugin != nullptr);
    auto processor = std::make_unique<ParameterProcessor>();
    auto* rawProcessor = processor.get();
    plugin->attach (std::move (processor));
    rawProcessor->getParameters()[0]->setValue (0.5f);

    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
    const auto lfo = rig.project.tracks[0]->graph.nodes.back().id;
    rig.edit ("setModulationShape", { { "trackId", rig.trackId },
                                      { "nodeId", lfo },
                                      { "points", list ({ point (0, 1.0), point (modulationShapeTicks, 1.0) }) } });
    rig.edit ("connect", { { "trackId", rig.trackId },
                           { "fromNode", lfo },
                           { "fromPort", 0 },
                           { "toNode", pluginId },
                           { "toPort", 2 } });
    rig.edit (
        "setModulation",
        { { "trackId", rig.trackId }, { "nodeId", pluginId }, { "port", 2 }, { "param", "gain" }, { "depth", 0.25 } });

    // 0.5 base + 0.25 modulation, applied to the constant input.
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.75f, 1e-4));
}

TEST_CASE ("a plugin parameter keeps its base across recompiles and gets it back when the slot goes",
           "[modulation][engine][plugins]")
{
    Rig rig;
    rig.edit (
        "insertNode",
        { { "trackId", rig.trackId }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("Restored") } });
    const auto pluginId = nodeOfType (*rig.project.tracks[0], nodetype::pluginEffect).id;

    std::ignore = rig.compiler.compile (rig.project, spec);
    auto* plugin = dynamic_cast<PluginNode*> (rig.compiler.findInstance (rig.trackId, pluginId));
    REQUIRE (plugin != nullptr);
    auto processor = std::make_unique<ParameterProcessor>();
    auto* rawProcessor = processor.get();
    plugin->attach (std::move (processor));
    rawProcessor->getParameters()[0]->setValue (0.5f);

    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
    const auto lfo = rig.project.tracks[0]->graph.nodes.back().id;
    rig.edit ("setModulationShape", { { "trackId", rig.trackId },
                                      { "nodeId", lfo },
                                      { "points", list ({ point (0, 1.0), point (modulationShapeTicks, 1.0) }) } });
    rig.edit ("connect", { { "trackId", rig.trackId },
                           { "fromNode", lfo },
                           { "fromPort", 0 },
                           { "toNode", pluginId },
                           { "toPort", 2 } });
    rig.edit (
        "setModulation",
        { { "trackId", rig.trackId }, { "nodeId", pluginId }, { "port", 2 }, { "param", "gain" }, { "depth", 0.25 } });
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.75f, 1e-4));

    // A structural edit recompiles. The base must stay 0.5, not become the modulated 0.75.
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::adsr } });
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.75f, 1e-4));

    // No depth writes the base again.
    rig.edit (
        "setModulation",
        { { "trackId", rig.trackId }, { "nodeId", pluginId }, { "port", 2 }, { "param", "gain" }, { "depth", 0.0 } });
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.5f, 1e-4));

    rig.edit (
        "setModulation",
        { { "trackId", rig.trackId }, { "nodeId", pluginId }, { "port", 2 }, { "param", "gain" }, { "depth", 0.25 } });
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.75f, 1e-4));

    // Removing the slot hands the parameter back at its base.
    rig.edit ("setModulation", { { "trackId", rig.trackId }, { "nodeId", pluginId }, { "port", 2 }, { "param", "" } });
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.5f, 1e-4));
    CHECK_THAT (rawProcessor->getParameters()[0]->getValue(), WithinAbs (0.5, 1e-6));
}

TEST_CASE ("a ramp-down LFO's cycle start opens a modulated filter at once", "[modulation][engine][measure]")
{
    // A 4 kHz tone through a low-pass whose cutoff a ramp-down LFO sweeps from 5195 Hz down to 200 Hz every 250 ms.
    // The second cycle starts 500 ms in, where the cutoff should jump from 200 Hz back to 5195 Hz.
    // Rendered in engine blocks, as live: the filter follows the per-sample modulation, so the jump lands on its
    // sample and the short smoothing lets the level reach that of the same filter held at 5195 Hz within 2 ms.
    // On failure it prints the level per millisecond around the jump.
    constexpr int tone = 4000;
    constexpr int jumpAt = 24000;
    constexpr int perMs = 48;

    const auto render = [&] (bool modulated)
    {
        Rig rig;

        for (size_t i = 0; i < rig.input.size(); ++i)
            rig.input[i] = level
                           * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * tone
                                                           * static_cast<double> (i) / sampleRate));

        rig.edit ("insertNode", { { "trackId", rig.trackId }, { "type", nodetype::filter } });
        const auto filter = nodeOfType (*rig.project.tracks[0], nodetype::filter).id;
        rig.edit ("setNodeParam", { { "trackId", rig.trackId },
                                    { "nodeId", filter },
                                    { "param", "cutoff" },
                                    { "value", modulated ? 200.0 : 5195.0 } });

        if (modulated)
        {
            rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
            const auto lfo = rig.project.tracks[0]->graph.nodes.back().id;
            rig.edit ("setModulationShape",
                      { { "trackId", rig.trackId },
                        { "nodeId", lfo },
                        { "points", list ({ point (0, 1.0), point (modulationShapeTicks, -1.0) }) } });

            for (const auto& [param, value] :
                 { std::pair { "sync", 0.0 }, std::pair { "rateMs", 250.0 }, std::pair { "zero", 2.0 } })
                rig.edit ("setNodeParam",
                          { { "trackId", rig.trackId }, { "nodeId", lfo }, { "param", param }, { "value", value } });

            rig.edit ("connect", { { "trackId", rig.trackId },
                                   { "fromNode", lfo },
                                   { "fromPort", 0 },
                                   { "toNode", filter },
                                   { "toPort", 1 } });
            rig.edit ("setModulation", { { "trackId", rig.trackId },
                                         { "nodeId", filter },
                                         { "port", 1 },
                                         { "param", "cutoff" },
                                         // In octaves (macros-and-modulation-column.md, MC1): 200 Hz up to 5195 Hz.
                                         { "depth", std::log2 (5195.0 / 200.0) / std::log2 (1000.0) } });
        }

        if (auto compiled = rig.compiler.update (rig.project, spec); compiled != nullptr)
            rig.engine.setEngine (compiled);

        rig.engine.play();
        std::vector<float> left (frames);
        std::vector<float> right (frames);

        for (int start = 0; start < frames; start += blockSize)
        {
            const std::array<float*, 2> out { left.data() + start, right.data() + start };
            const std::array<const float*, 1> in { rig.input.data() + start };
            rig.engine.renderOffline (out.data(), 2, std::min (blockSize, frames - start), sampleRate, in.data(), 1);
        }

        return left;
    };

    const auto swept = render (true);
    const auto held = render (false);
    const auto reference = peak (held, 20000, frames);
    const auto db = [&] (size_t from)
    { return 20.0f * std::log10 (std::max (peak (swept, from, from + perMs), 1e-9f) / reference); };

    juce::String table;
    int firstWithin3Db = -1;

    for (int ms = -2; ms < 20; ++ms)
    {
        const int from = jumpAt + (ms * perMs);
        const auto value = db (static_cast<size_t> (from));
        table << ms << " ms: " << juce::String (value, 1) << " dB\n";

        if (firstWithin3Db < 0 && ms >= 0 && value > -3.0f)
            firstWithin3Db = ms;
    }

    INFO ("Level per 1 ms window against the held filter:\n" << table);
    CHECK (db (static_cast<size_t> (jumpAt - 2 * perMs)) < -30.0f);
    CHECK (firstWithin3Db >= 0);
    CHECK (firstWithin3Db <= 2);
}

TEST_CASE ("a reset node sounds like a freshly prepared one", "[modulation][dsp][voice]")
{
    constexpr int warmUp = 2000;
    constexpr int probeLength = 512;
    std::vector<float> tone (warmUp);

    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = std::sin (static_cast<float> (i) * 0.07f);

    const std::array<AudioInput, 2> inputs { AudioInput { tone.data(), tone.data() }, AudioInput { nullptr, nullptr } };

    EventBuffer noteOn;
    REQUIRE (noteOn.add ({ 0, NoteEventType::noteOn, 1, nullptr }));
    EventBuffer noteOnAgain;
    REQUIRE (noteOnAgain.add ({ 10, NoteEventType::noteOn, 2, nullptr }));

    // Each type with parameters that differ from where the warm-up left its state.
    const std::vector<std::pair<const char*, std::vector<std::pair<const char*, float>>>> cases {
        { nodetype::lfo, { { "sync", 0.0f }, { "rateMs", 100.0f }, { "phase", 0.3f } } },
        { nodetype::adsr, { { "attackMs", 5.0f }, { "decayMs", 5.0f }, { "sustain", 50.0f } } },
        { nodetype::filter, { { "cutoff", 300.0f }, { "resonance", 60.0f } } },
        { nodetype::saturator, { { "drive", 30.0f }, { "bias", 80.0f }, { "mix", 60.0f } } },
        { nodetype::clipper, { { "drive", 18.0f }, { "ceiling", -6.0f }, { "knee", 40.0f }, { "mix", 60.0f } } },
        { nodetype::waveshaper, { { "shape", 3.0f }, { "drive", 18.0f }, { "bias", 30.0f }, { "tone", 6.0f } } },
        { nodetype::bitcrusher,
          { { "bits", 5.0f }, { "rate", 6000.0f }, { "jitter", 40.0f }, { "dither", 1.0f }, { "smooth", 1.0f } } },
        { nodetype::bitcrusher, { { "rateMode", 1.0f }, { "note", 30.0f }, { "antiAlias", 1.0f }, { "bias", 50.0f } } },
        { nodetype::chorus, { { "delay", 8.0f }, { "voices", 3.0f }, { "feedback", 40.0f }, { "shape", 1.0f } } },
        { nodetype::flanger, { { "delay", 4.0f }, { "feedback", -60.0f }, { "spread", 180.0f } } },
        { nodetype::phaser, { { "stages", 4.0f }, { "centre", 2000.0f }, { "feedback", 70.0f } } },
        { nodetype::gate, { { "threshold", -30.0f }, { "hysteresis", 3.0f }, { "hold", 5.0f } } },
        { nodetype::expander, { { "ratio", 4.0f }, { "range", -60.0f }, { "release", 30.0f } } },
        { nodetype::gain, { { "gainDb", -12.0f } } },
        { nodetype::utility, { { "gain", -6.0f }, { "pan", 50.0f }, { "width", 0.0f } } },
        { nodetype::oscillator, { { "waveform", 3.0f }, { "phase", 90.0f }, { "unison", 3.0f } } },
        { nodetype::oscillator, { { "waveform", 4.0f } } },
    };

    for (const auto& [type, params] : cases)
    {
        INFO (type);
        std::vector<float> left (probeLength);
        std::vector<float> right (probeLength);

        // Used: played with other settings, then reset with the new ones.
        SourceHarness used { type };
        std::vector<float> warmLeft (warmUp);
        std::vector<float> warmRight (warmUp);
        processSource (*used.node, warmLeft, warmRight, transportAt (0.0, warmUp), &noteOn, &inputs);

        for (const auto& [id, value] : params)
            setParam (*used.node, id, value);

        (*used.node).reset();
        processSource (*used.node, left, right, transportAt (0.0, probeLength), &noteOnAgain, &inputs);

        // Fresh: the same settings before prepare.
        SourceHarness fresh { type };

        for (const auto& [id, value] : params)
            setParam (*fresh.node, id, value);

        fresh.node->prepare (SourceHarness::directSpec);
        std::vector<float> freshLeft (probeLength);
        std::vector<float> freshRight (probeLength);
        processSource (*fresh.node, freshLeft, freshRight, transportAt (0.0, probeLength), &noteOnAgain, &inputs);

        float difference = 0.0f;

        for (size_t i = 0; i < left.size(); ++i)
            difference =
                std::max ({ difference, std::abs (left[i] - freshLeft[i]), std::abs (right[i] - freshRight[i]) });

        CHECK (difference < 1.0e-6f);
    }
}

TEST_CASE ("the gain and utility follow per-sample modulation", "[modulation][dsp]")
{
    // A modulated parameter that steps at sample 300 of the block, as an envelope's attack does.
    constexpr int blockLength = 1024;
    constexpr int step = 300;
    constexpr int settled = step + 240; // 5 ms, five smoothing time constants
    std::vector<float> ones (blockLength, 1.0f);
    const std::array<AudioInput, 2> inputs { AudioInput { ones.data(), ones.data() }, AudioInput { nullptr, nullptr } };

    const auto render = [&] (const char* type, size_t param, float before, float after)
    {
        // Prepared at the starting value, so the node is settled there.
        SourceHarness harness { type };
        harness.node->params().set (param, before);
        harness.node->prepare (SourceHarness::directSpec);
        std::vector<float> values (blockLength, before);
        std::fill (values.begin() + step, values.end(), after);
        const std::array modulated { ModulatedParam { param, values.data() } };

        std::vector<float> left (blockLength);
        std::vector<float> right (blockLength);
        std::array<AudioOutput, 1> outputs { AudioOutput { left.data(), right.data() } };
        const auto transport = transportAt (0.0, blockLength);
        ProcessContext context;
        context.numSamples = blockLength;
        context.audioInputs = inputs;
        context.audioOutputs = outputs;
        context.transport = &transport;
        context.modulatedParams = modulated;
        harness.node->process (context);
        return std::pair { left, right };
    };

    // The gain's amplitude opens from 0 to 100 % at the step, not at the next block.
    const auto [gainLeft, gainRight] = render (nodetype::gain, 1, 0.0f, 100.0f);
    CHECK_THAT (gainLeft[step - 1], WithinAbs (0.0, 1e-6));
    CHECK (gainLeft[step + 5] > 0.05f);
    CHECK_THAT (gainLeft[settled], WithinAbs (1.0, 0.01));
    CHECK_THAT (gainRight[settled], WithinAbs (1.0, 0.01));

    // The utility's pan moves from hard left to hard right at the step.
    const auto [panLeft, panRight] = render (nodetype::utility, 1, -100.0f, 100.0f);
    CHECK_THAT (panRight[step - 1], WithinAbs (0.0, 1e-3));
    CHECK_THAT (panLeft[settled], WithinAbs (0.0, 0.01));
    CHECK (panRight[settled] > 0.9f);
}

// ---------------------------------------------------------------------------------------------------------
// Note Expression (docs/design/phase12-voice-scope.md, V2)

namespace
{
// A Note Expression node with its five outputs, rendered one block at a time from tick 0.
struct ExpressionHarness
{
    ExpressionHarness()
        : node (NodeRegistry::builtin().create (nodetype::noteExpression))
    {
        REQUIRE (node != nullptr);
        node->prepare (SourceHarness::directSpec);
    }

    // Renders a block; returns each output's left channel.
    std::array<std::vector<float>, 5> render (int numSamples, std::initializer_list<NoteEvent> events)
    {
        EventBuffer buffer;

        for (const auto& event : events)
            REQUIRE (buffer.add (event));

        std::array<std::vector<float>, 5> left;
        std::array<std::vector<float>, 5> right;
        std::array<AudioOutput, 5> outputs {};

        for (size_t i = 0; i < outputs.size(); ++i)
        {
            left[i].resize (static_cast<size_t> (numSamples));
            right[i].resize (static_cast<size_t> (numSamples));
            outputs[i] = { left[i].data(), right[i].data() };
        }

        const auto transport = transportAt (tick, numSamples);
        tick = transport.endTick;
        ProcessContext context;
        context.numSamples = numSamples;
        context.audioOutputs = outputs;
        context.eventsIn = &buffer;
        context.transport = &transport;
        node->process (context);
        return left;
    }

    std::unique_ptr<Node> node;
    double tick = 0.0;
};

enum Output : size_t
{
    pitchOut,
    pressureOut,
    timbreOut,
    velocityOut,
    gateOut,
};

[[nodiscard]] CompiledNote expressionNote (ObjectId id, double pitch, float velocity)
{
    CompiledNote note;
    note.id = id;
    note.pitch = pitch;
    note.velocity = velocity;
    return note;
}
} // namespace

TEST_CASE ("Note Expression follows a note's curves, velocity and gate", "[modulation][dsp][expression]")
{
    ExpressionHarness harness;
    auto note = expressionNote (1, 72.0, 0.6f);
    const std::array<CurvePoint, 2> pressure { CurvePoint { 0, 0.0 }, CurvePoint { ticksPerBeat, 1.0 } };
    note.lanes[static_cast<size_t> (Lane::pressure)] = pressure;

    // 24000 samples is one beat in the test transport.
    const auto out = harness.render (
        24000, { { 100, NoteEventType::noteOn, 1, &note }, { 20000, NoteEventType::noteOff, 1, nullptr } });

    // Before the note: the gate is closed and the values rest at C4, no pressure, middle timbre.
    CHECK_THAT (out[gateOut][50], WithinAbs (0.0, 1e-6));
    CHECK_THAT (out[pitchOut][50], WithinAbs (0.0, 1e-6));
    CHECK_THAT (out[timbreOut][50], WithinAbs (0.5, 1e-6));

    // During it: an octave above C4 is 12 / 240, and the pressure follows its curve.
    CHECK_THAT (out[gateOut][100], WithinAbs (1.0, 1e-6));
    CHECK_THAT (out[velocityOut][100], WithinAbs (0.6, 1e-6));
    CHECK_THAT (out[pitchOut][100], WithinAbs (12.0 / 240.0, 1e-6));
    CHECK_THAT (out[pressureOut][12000], WithinAbs (0.5, 0.01));

    // After it the gate closes and the values hold.
    CHECK_THAT (out[gateOut][20001], WithinAbs (0.0, 1e-6));
    CHECK_THAT (out[pitchOut][23000], WithinAbs (12.0 / 240.0, 1e-6));
    CHECK_THAT (out[pressureOut][23000], WithinAbs (out[pressureOut][20001], 1e-6));
}

TEST_CASE ("Note Expression follows the newest held note, then the one before", "[modulation][dsp][expression]")
{
    ExpressionHarness harness;
    auto low = expressionNote (1, 60.0, 0.5f);
    auto high = expressionNote (2, 67.0, 0.9f);

    auto out =
        harness.render (512, { { 0, NoteEventType::noteOn, 1, &low }, { 200, NoteEventType::noteOn, 2, &high } });
    CHECK_THAT (out[pitchOut][199], WithinAbs (0.0, 1e-6));
    CHECK_THAT (out[pitchOut][200], WithinAbs (7.0 / 240.0, 1e-6));
    CHECK_THAT (out[velocityOut][200], WithinAbs (0.9, 1e-6));

    // The newest note ends: back to the held one at once.
    out = harness.render (512, { { 10, NoteEventType::noteOff, 2, nullptr } });
    CHECK_THAT (out[pitchOut][10], WithinAbs (0.0, 1e-6));
    CHECK_THAT (out[gateOut][10], WithinAbs (1.0, 1e-6));

    // A note released with the pedal down keeps the gate open until the pedal lifts.
    out = harness.render (512, { { 0, NoteEventType::sustain, 0, nullptr, 0, 1.0f },
                                 { 10, NoteEventType::noteOff, 1, nullptr },
                                 { 300, NoteEventType::sustain, 0, nullptr, 0, 0.0f } });
    CHECK_THAT (out[gateOut][299], WithinAbs (1.0, 1e-6));
    CHECK_THAT (out[gateOut][300], WithinAbs (0.0, 1e-6));
}

TEST_CASE ("Note Expression follows live expression", "[modulation][dsp][expression]")
{
    ExpressionHarness harness;
    auto note = expressionNote (-1, 64.0, 0.7f);
    std::ignore = harness.render (64, { { 0, NoteEventType::noteOn, -1, &note } });

    // Two semitones of live bend, and live timbre.
    const auto pitchLane = static_cast<int> (Lane::pitch);
    const auto timbreLane = static_cast<int> (Lane::timbre);
    const auto out = harness.render (64, { { 0, NoteEventType::expression, -1, nullptr, pitchLane, 2.0f },
                                           { 0, NoteEventType::expression, -1, nullptr, timbreLane, 0.9f } });
    CHECK_THAT (out[pitchOut][63], WithinAbs (6.0 / 240.0, 1e-6));
    CHECK_THAT (out[timbreOut][63], WithinAbs (0.9, 1e-6));
}

TEST_CASE ("Note Expression drives an oscillator's pitch at 100 % depth", "[modulation][engine][expression]")
{
    // Sequencer -> Note Expression -> (pitch) Oscillator -> the chain, playing A4.
    auto setup = projectWithClip();
    auto project = apply (setup.project, "addNotes",
                          { { "clipId", setup.clipId }, { "notes", list ({ noteArgs (0, 4 * ticksPerBeat, 69) }) } });
    const auto trackId = setup.trackId;
    const auto sequencer = nodeOfType (*project.tracks[0], nodetype::sequencer).id;
    const auto synth = nodeOfType (*project.tracks[0], nodetype::mpeSynth).id;
    const auto eq = nodeOfType (*project.tracks[0], nodetype::eq).id;

    project = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::noteExpression } });
    const auto expression = project.tracks[0]->graph.nodes.back().id;
    project = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::oscillator } });
    const auto oscillator = project.tracks[0]->graph.nodes.back().id;

    const auto connect = [&] (const char* command, ObjectId from, int fromPort, ObjectId to, int toPort)
    {
        project = apply (project, command,
                         { { "trackId", trackId },
                           { "fromNode", from },
                           { "fromPort", fromPort },
                           { "toNode", to },
                           { "toPort", toPort } });
    };

    connect ("disconnect", synth, 0, eq, 0);
    connect ("connect", sequencer, 0, expression, 0);
    connect ("connect", expression, 0, oscillator, 0);
    connect ("connect", oscillator, 0, eq, 0);
    project = apply (project, "setNodeParam",
                     { { "trackId", trackId }, { "nodeId", oscillator }, { "param", "waveform" }, { "value", 0.0 } });
    project = apply (
        project, "setModulation",
        { { "trackId", trackId }, { "nodeId", oscillator }, { "port", 0 }, { "param", "pitch" }, { "depth", 1.0 } });

    GraphCompiler compiler;
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    engine.play();
    const auto output = render (engine, 48000);

    // Half a second of A4, after the first blocks.
    const auto crossings = zeroCrossings (output.left(), 2048, 2048 + 24000);
    CHECK_THAT (static_cast<double> (crossings) / 2.0 / 0.5, WithinAbs (440.0, 3.0));
}

TEST_CASE ("Note Expression is a modulation source, not an instrument", "[modulation][commands][expression]")
{
    // An audio track holds no instruments, but takes modulation sources.
    Rig rig;
    REQUIRE_NOTHROW (rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::noteExpression } }));
    CHECK (nodetype::isModulationSource (nodetype::noteExpression));
}
