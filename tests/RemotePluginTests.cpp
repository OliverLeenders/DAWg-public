#include "TestUtils.h"
#include "dawg/AudioEngine.h"
#include "dawg/ClapPlugin.h"
#include "dawg/GraphCompiler.h"
#include "dawg/NodeRegistry.h"
#include "dawg/PluginNode.h"
#include "dawg/RemotePlugin.h"
#include "dawg/Sequence.h"
#include "dawg/VarUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstdlib>

// Plugins in helper processes (docs/design/out-of-process-plugins.md), with these tests' own executable as the helper.

using namespace dawg;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr auto sampleRate = 48000.0;
constexpr auto blockSize = 512;

[[nodiscard]] std::unique_ptr<juce::AudioPluginInstance> loadRemote (const juce::String& name,
                                                                     const juce::String& file = DAWG_TEST_CLAP_PATH)
{
    ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, file);

    const juce::PluginDescription* match = nullptr;
    for (const auto* description : found)
        if (description->name == name)
            match = description;

    INFO ("No plugin called " << name);
    REQUIRE (match != nullptr);
    juce::WaitableEvent done;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::String error;
    createRemotePlugin (*match, sampleRate, blockSize,
                        [&] (std::unique_ptr<juce::AudioPluginInstance> plugin, const juce::String& message)
                        {
                            instance = std::move (plugin);
                            error = message;
                            done.signal();
                        });
    REQUIRE (done.wait (30000));
    INFO (error);
    REQUIRE (instance != nullptr);
    return instance;
}

[[nodiscard]] juce::String stateOf (juce::AudioProcessor& plugin)
{
    juce::MemoryBlock state;
    plugin.getStateInformation (state);
    return state.toString();
}
} // namespace

TEST_CASE ("a plugin in another process plays notes", "[remote]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    auto plugin = loadRemote ("Test Sine");
    CHECK (plugin->getName() == "Test Sine");
    CHECK (plugin->getTotalNumOutputChannels() == 2);
    CHECK (plugin->getTotalNumInputChannels() == 0);
    plugin->prepareToPlay (sampleRate, blockSize);
    CHECK (plugin->getLatencySamples() == 64);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 69, 0.8f), 10);
    buffer.clear();
    plugin->processBlock (buffer, midi);
    CHECK (buffer.getMagnitude (0, 0, blockSize) > 0.2f);
    CHECK (buffer.getMagnitude (1, 0, blockSize) > 0.2f);
    CHECK (midi.isEmpty());

    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOff (1, 69), 0);
    buffer.clear();
    plugin->processBlock (buffer, midi);
    CHECK (buffer.getMagnitude (0, 0, blockSize) == 0.0f);

    const auto state = stateOf (*plugin);
    CHECK (state.contains ("on=1"));
    CHECK (state.contains ("off=1"));
    CHECK (state.contains ("wrong=0")); // the plugin was on what it counts as its audio thread
    plugin->releaseResources();
}

TEST_CASE ("an effect in another process processes audio, and its state round-trips", "[remote]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    auto plugin = loadRemote ("Test Gain");
    CHECK (plugin->getTotalNumInputChannels() == 2);
    plugin->prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    const auto run = [&]
    {
        buffer.clear();

        for (int s = 0; s < blockSize; ++s)
            buffer.setSample (0, s, 0.5f);

        plugin->processBlock (buffer, midi);
        return buffer.getSample (0, 100);
    };

    CHECK_THAT (run(), WithinAbs (0.5, 1e-6));
    CHECK_THAT (buffer.getSample (1, 100), WithinAbs (0.0, 1e-6));

    const juce::String doubled ("gain=2.000");
    plugin->setStateInformation (doubled.toRawUTF8(), static_cast<int> (doubled.getNumBytesAsUTF8()));
    CHECK_THAT (run(), WithinAbs (1.0, 1e-6));
    CHECK (stateOf (*plugin).startsWith ("gain=2.000"));
    plugin->releaseResources();
}

TEST_CASE ("instances of one plugin file share a helper and keep apart", "[remote]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    auto first = loadRemote ("Test Gain");
    auto second = loadRemote ("Test Gain");
    first->prepareToPlay (sampleRate, blockSize);
    second->prepareToPlay (sampleRate, blockSize);
    const juce::String doubled ("gain=2.000");
    first->setStateInformation (doubled.toRawUTF8(), static_cast<int> (doubled.getNumBytesAsUTF8()));

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    const auto run = [&] (juce::AudioProcessor& plugin)
    {
        buffer.clear();

        for (int s = 0; s < blockSize; ++s)
            buffer.setSample (0, s, 0.5f);

        plugin.processBlock (buffer, midi);
        return buffer.getSample (0, 100);
    };

    CHECK_THAT (run (*first), WithinAbs (1.0, 1e-6));
    CHECK_THAT (run (*second), WithinAbs (0.5, 1e-6));
}

TEST_CASE ("a plugin node runs a plugin in another process", "[remote][engine]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    PluginNode node (false);
    node.prepare ({ sampleRate, blockSize });
    node.attach (loadRemote ("Test Gain"));
    CHECK (node.status() == PluginNode::Status::ready);
    CHECK (node.latencySamples() == 64);
    CHECK (node.hasSidechain());
}

TEST_CASE ("a plugin file that won't load is an error, not a hang", "[remote]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    juce::PluginDescription description;
    description.name = "Nothing";
    description.pluginFormatName = ClapPluginFormat::formatName;
    description.fileOrIdentifier = "C:/does/not/exist.clap";
    juce::WaitableEvent done;
    juce::String error;
    bool loaded = false;
    createRemotePlugin (description, sampleRate, blockSize,
                        [&] (std::unique_ptr<juce::AudioPluginInstance> plugin, const juce::String& message)
                        {
                            loaded = plugin != nullptr;
                            error = message;
                            done.signal();
                        });
    REQUIRE (done.wait (30000));
    CHECK_FALSE (loaded);
    CHECK (error.isNotEmpty());
}

namespace
{
[[nodiscard]] int reported (const juce::String& state, const juce::String& field)
{
    return (" " + state).fromFirstOccurrenceOf (" " + field + "=", false, false).getIntValue();
}

// One plugin node with a plugin in another process, and hand-made events.
struct RemoteRig
{
    explicit RemoteRig (std::initializer_list<juce::var> notes, const juce::String& name = "Test Sine",
                        bool instrument = true)
        : node (instrument)
    {
        auto setup = test::projectWithClip();
        clipId = setup.clipId;
        juce::Array<juce::var> list;

        for (const auto& note : notes)
            list.add (note);

        project = list.isEmpty() ? setup.project
                                 : test::apply (setup.project, "addNotes", { { "clipId", clipId }, { "notes", list } });
        sequence = std::make_unique<TrackSequence> (project.tracks[0]->clips);
        node.prepare ({ sampleRate, blockSize });
        node.attach (loadRemote (name));
        transport.playing = true;
        transport.timeMap = TimeMap (test::constantTempo(), sampleRate);
        audio.setSize (2, blockSize);
        key.setSize (2, blockSize);
    }

    std::vector<float> block (double startTick, std::initializer_list<NoteEvent> events)
    {
        EventBuffer buffer;

        for (const auto& event : events)
            std::ignore = buffer.add (event);

        transport.startTick = startTick;
        transport.endTick = startTick + blockSize * static_cast<double> (ticksPerBeat) / 24000.0;
        const std::array<AudioInput, 2> inputs { AudioInput { input.getReadPointer (0), input.getReadPointer (1) },
                                                 AudioInput { key.getReadPointer (0), key.getReadPointer (1) } };
        const std::array<AudioOutput, 1> outputs { AudioOutput { audio.getWritePointer (0),
                                                                 audio.getWritePointer (1) } };
        ProcessContext context;
        context.numSamples = blockSize;
        context.audioInputs = inputs;
        context.audioOutputs = outputs;
        context.eventsIn = &buffer;
        context.transport = &transport;
        context.sequence = sequence.get();
        context.noteModulations = noteModulations;
        node.process (context);
        return { audio.getReadPointer (0), audio.getReadPointer (0) + blockSize };
    }

    [[nodiscard]] NoteEvent on (size_t index) const
    {
        const auto& note = sequence->notes()[index];
        return { 0, NoteEventType::noteOn, note.id, &note };
    }

    Project project;
    ObjectId clipId = 0;
    std::unique_ptr<TrackSequence> sequence;
    PluginNode node;
    Transport transport;
    juce::AudioBuffer<float> audio;
    juce::AudioBuffer<float> input { 2, blockSize };
    juce::AudioBuffer<float> key;
    std::span<const NoteModulation> noteModulations;
};

[[nodiscard]] double hertzOf (const std::vector<float>& samples)
{
    auto crossings = 0;

    for (size_t i = 1; i < samples.size(); ++i)
        if (samples[i - 1] < 0.0f && samples[i] >= 0.0f)
            ++crossings;

    return crossings * sampleRate / static_cast<double> (samples.size());
}

[[nodiscard]] double frequencyOver (RemoteRig& rig, double fromTick, int blocks)
{
    std::vector<float> all;

    for (int i = 0; i < blocks; ++i)
    {
        const auto part = rig.block (fromTick + i * blockSize * static_cast<double> (ticksPerBeat) / 24000.0, {});
        all.insert (all.end(), part.begin(), part.end());
    }

    return hertzOf (all);
}

[[nodiscard]] float loudest (const std::vector<float>& samples, size_t from = 0, size_t to = blockSize)
{
    float top = 0.0f;

    for (auto i = from; i < to; ++i)
        top = std::max (top, std::abs (samples[i]));

    return top;
}
} // namespace

TEST_CASE ("a plugin in another process shows its parameters", "[remote]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    auto plugin = loadRemote ("Test Gain");
    const auto& parameters = plugin->getParameters();
    REQUIRE (parameters.size() == 1);
    auto* gain = parameters[0];
    CHECK (gain->getName (32) == "Gain");
    CHECK (dynamic_cast<juce::HostedAudioProcessorParameter*> (gain)->getParameterID() == "7");
    CHECK_THAT (gain->getValue(), WithinAbs (0.5, 1e-6));
    CHECK (gain->getText (0.5f, 32) == "1.00 x");
    CHECK_THAT (gain->getValueForText ("2.0"), WithinAbs (1.0, 1e-6));

    plugin->prepareToPlay (sampleRate, blockSize);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    const auto run = [&]
    {
        buffer.clear();

        for (int s = 0; s < blockSize; ++s)
            buffer.setSample (0, s, 0.5f);

        plugin->processBlock (buffer, midi);
        return buffer.getSample (0, 100);
    };

    CHECK_THAT (run(), WithinAbs (0.5, 1e-6));
    gain->setValue (1.0f);
    CHECK_THAT (run(), WithinAbs (1.0, 1e-6));
    CHECK_THAT (gain->getValue(), WithinAbs (1.0, 1e-6));

    // State moves the parameter too.
    const juce::String half ("gain=0.500");
    plugin->setStateInformation (half.toRawUTF8(), static_cast<int> (half.getNumBytesAsUTF8()));
    CHECK_THAT (gain->getValue(), WithinAbs (0.25, 1e-3));
    plugin->releaseResources();
}

TEST_CASE ("native notes and tuning reach a CLAP plugin in another process", "[remote][native]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    auto note = test::noteArgs (0, 4 * ticksPerBeat, 57);
    juce::Array<juce::var> point;
    point.add (0);
    point.add (12.0);
    juce::Array<juce::var> curve;
    curve.add (point);
    note.getDynamicObject()->setProperty ("expression", makeObject ({ { "pitch", curve } }));

    RemoteRig rig ({ note });
    CHECK (rig.node.usesNativeNotes());
    std::ignore = rig.block (0.0, { rig.on (0) });
    CHECK_THAT (frequencyOver (rig, static_cast<double> (blockSize), 8), WithinAbs (440.0, 15.0));

    const auto state = stateOf (*rig.node.processor());
    CHECK (reported (state, "on") == 1);
    CHECK (reported (state, "midi") == 0);
    CHECK (reported (state, "expr") > 0);
}

TEST_CASE ("a linked note keeps its note across the process boundary", "[remote][native][links]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    RemoteRig rig ({ test::noteArgs (0, ticksPerBeat, 69), test::noteArgs (ticksPerBeat, ticksPerBeat, 81) });
    juce::Array<juce::var> links;
    links.add (makeObject ({ { "from", rig.project.tracks[0]->clips[0]->notes[0]->id },
                             { "to", rig.project.tracks[0]->clips[0]->notes[1]->id },
                             { "glideMs", 0.0 } }));
    rig.project = test::apply (rig.project, "setNoteLinks", { { "clipId", rig.clipId }, { "links", links } });
    rig.sequence = std::make_unique<TrackSequence> (rig.project.tracks[0]->clips);
    std::ignore = rig.block (0.0, { rig.on (0) });
    const auto& second = rig.sequence->notes()[1];
    NoteEvent legato { 0, NoteEventType::legato, second.id, &second };
    legato.fromNoteId = rig.sequence->notes()[0].id;
    const auto start = static_cast<double> (ticksPerBeat);
    std::ignore = rig.block (start, { legato });
    CHECK_THAT (frequencyOver (rig, start + blockSize * static_cast<double> (ticksPerBeat) / 24000.0, 8),
                WithinAbs (880.0, 25.0));

    const auto state = stateOf (*rig.node.processor());
    CHECK (reported (state, "on") == 1);
    CHECK (reported (state, "off") == 0);
}

TEST_CASE ("automation lands inside a block in another process", "[remote][native]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    RemoteRig rig ({ test::noteArgs (0, 4 * ticksPerBeat, 69) });
    REQUIRE (rig.node.takesParameterEvents());
    std::ignore = rig.block (0.0, { rig.on (0) });
    rig.node.automateParameter (0, 1.0f, 256);
    const auto output = rig.block (static_cast<double> (blockSize), {});
    CHECK_THAT (static_cast<double> (loudest (output, 0, 256)), WithinAbs (0.25, 0.05));
    CHECK_THAT (static_cast<double> (loudest (output, 256, 512)), WithinAbs (0.5, 0.05));
    CHECK_THAT (rig.node.parameterValue (0), WithinAbs (1.0, 1e-6));
}

TEST_CASE ("modulation crosses the boundary apart from the value, whole and per note", "[remote][native]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    RemoteRig rig ({ test::noteArgs (0, 4 * ticksPerBeat, 69) });
    REQUIRE (rig.node.isModulatable (0));
    REQUIRE (rig.node.isModulatablePerNote (0));
    std::ignore = rig.block (0.0, { rig.on (0) });
    const auto plain = loudest (rig.block (static_cast<double> (blockSize), {}));

    rig.node.modulateParameter (0, 0.5f);
    std::ignore = rig.block (2.0 * blockSize, {});
    CHECK_THAT (static_cast<double> (loudest (rig.block (3.0 * blockSize, {})) / plain), WithinAbs (2.0, 0.1));
    CHECK_THAT (rig.node.parameterValue (0), WithinAbs (0.5, 1e-6));
    rig.node.modulateParameter (0, 0.0f);

    // A voice's own amount: pressure is 0 here, so what the other slots add.
    const std::array<NoteModulation, 1> slots { NoteModulation {
        .parameter = 0, .source = 1, .depth = 0.5f, .shared = 0.25f, .nodes = {}, .port = 0 } };
    rig.noteModulations = slots;
    std::ignore = rig.block (4.0 * blockSize, {});
    CHECK_THAT (static_cast<double> (loudest (rig.block (5.0 * blockSize, {})) / plain), WithinAbs (1.5, 0.1));
    CHECK (reported (stateOf (*rig.node.processor()), "notemods") >= 1);
}

TEST_CASE ("a plugin's sidechain works across the boundary", "[remote]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    RemoteRig rig ({}, "Test Gain", false);
    REQUIRE (rig.node.hasSidechain());
    rig.input.clear();
    rig.key.clear();

    for (int s = 0; s < blockSize; ++s)
    {
        rig.input.setSample (0, s, 0.25f);
        rig.key.setSample (0, s, 0.5f);
    }

    const auto output = rig.block (0.0, {});
    CHECK_THAT (static_cast<double> (output[100]), WithinAbs (0.75, 1e-6));
}

TEST_CASE ("a per-note ADSR drives a plugin in another process through the engine", "[remote][native][engine]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    auto setup = test::projectWithClip();
    const auto trackId = setup.trackId;
    juce::Array<juce::var> notes;
    notes.add (test::noteArgs (0, 2 * ticksPerBeat, 69));
    notes.add (test::noteArgs (ticksPerBeat / 8, 2 * ticksPerBeat, 72));
    auto project = test::apply (setup.project, "addNotes", { { "clipId", setup.clipId }, { "notes", notes } });

    ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, DAWG_TEST_CLAP_PATH);
    const auto plugin = makeObject ({ { "identifier", found[0]->createIdentifierString() },
                                      { "name", found[0]->name },
                                      { "manufacturer", found[0]->manufacturerName },
                                      { "format", found[0]->pluginFormatName } });
    project = test::apply (project, "insertNode",
                           { { "trackId", trackId }, { "type", nodetype::pluginInstrument }, { "plugin", plugin } });
    const auto idOf = [&project] (const char* wanted)
    {
        for (const auto& node : project.tracks[0]->graph.nodes)
            if (node.type == wanted)
                return node.id;

        return ObjectId { 0 };
    };
    const auto instrument = idOf (nodetype::pluginInstrument);
    project = test::apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::adsr } });
    const auto adsr = idOf (nodetype::adsr);
    project = test::apply (project, "setNodeParam",
                           { { "trackId", trackId }, { "nodeId", adsr }, { "param", "attackMs" }, { "value", 100.0 } });
    project = test::apply (project, "connect",
                           { { "trackId", trackId },
                             { "fromNode", idOf (nodetype::sequencer) },
                             { "fromPort", 0 },
                             { "toNode", adsr },
                             { "toPort", 0 } });
    project = test::apply (
        project, "connect",
        { { "trackId", trackId }, { "fromNode", adsr }, { "fromPort", 0 }, { "toNode", instrument }, { "toPort", 1 } });
    project = test::apply (
        project, "setModulation",
        { { "trackId", trackId }, { "nodeId", instrument }, { "port", 1 }, { "param", "7" }, { "depth", 0.25 } });

    GraphCompiler compiler;
    compiler.setPluginLoader ([] (ObjectId, const GraphNode&, const std::shared_ptr<PluginNode>& target)
                              { target->attach (loadRemote ("Test Sine")); });
    std::ignore = compiler.compile (project, test::spec);
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, test::spec));
    engine.play();
    std::ignore = test::render (engine, 24000);

    auto* node = dynamic_cast<PluginNode*> (compiler.findInstance (trackId, instrument));
    REQUIRE (node != nullptr);
    const auto state = stateOf (*node->processor());
    CHECK (reported (state, "modids") == 2);
    CHECK (reported (state, "notemods") > 10);
    CHECK (reported (state, "wrong") == 0);
}

namespace
{
constexpr int crashKey = 1;
constexpr int hangKey = 2;
constexpr int slowKey = 3;

// A plugin in a helper, and a way to play keys on it by id.
struct Failing
{
    Failing()
        : plugin (loadRemote ("Test Sine")),
          native (dynamic_cast<ClapNativeInput*> (plugin.get())),
          remote (dynamic_cast<RemoteInstance*> (plugin.get()))
    {
        REQUIRE (native != nullptr);
        REQUIRE (remote != nullptr);
        plugin->prepareToPlay (sampleRate, blockSize);
        remote->setCrashCallback ([this] { crashCalls.fetch_add (1); });
    }

    // One block: how long it took, in milliseconds.
    double block()
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        buffer.clear();
        plugin->processBlock (buffer, midi);
        return juce::Time::getMillisecondCounterHiRes() - start;
    }

    [[nodiscard]] bool waitForCrash (double seconds)
    {
        const auto until = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;

        while (! remote->crashed() && juce::Time::getMillisecondCounterHiRes() < until)
        {
            std::ignore = block();
            juce::Thread::sleep (10);
        }

        return remote->crashed();
    }

    std::unique_ptr<juce::AudioPluginInstance> plugin;
    ClapNativeInput* native;
    RemoteInstance* remote;
    juce::AudioBuffer<float> buffer { 2, blockSize };
    juce::MidiBuffer midi;
    std::atomic<int> crashCalls { 0 };
};
} // namespace

TEST_CASE ("a plugin that crashes is known at once, and costs the audio thread nothing", "[remote][failure]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    Failing rig;
    const juce::String kept ("gain=1.500");
    rig.plugin->setStateInformation (kept.toRawUTF8(), static_cast<int> (kept.getNumBytesAsUTF8()));
    std::ignore = rig.block();
    CHECK_FALSE (rig.remote->crashed());

    rig.native->noteOn (0, 1, crashKey, 0.8);
    std::ignore = rig.block();
    REQUIRE (rig.waitForCrash (2.0)); // not the ten seconds of JUCE's ping
    juce::Thread::sleep (50);
    CHECK (rig.crashCalls.load() == 1);
    CHECK (rig.remote->crashReason().isNotEmpty());
    CHECK (rig.plugin->getLatencySamples() == 0);

    // Blocks go on, at no cost, and silently.
    double slowest = 0.0;

    for (int i = 0; i < 20; ++i)
        slowest = std::max (slowest, rig.block());

    CHECK (slowest < 5.0);
    CHECK (rig.buffer.getMagnitude (0, 0, blockSize) == 0.0f);

    // Saving now mustn't lose what the plugin had.
    CHECK (stateOf (*rig.plugin).startsWith ("gain=1.500"));

    // It comes back with that state, its latency, and playing.
    juce::String error;
    const auto reloadStart = juce::Time::getMillisecondCounterHiRes();
    REQUIRE (rig.remote->reload (error));
    CHECK (juce::Time::getMillisecondCounterHiRes() - reloadStart < 3000.0); // not held up by the dead helper's cleanup
    CHECK_FALSE (rig.remote->crashed());
    CHECK (rig.remote->crashReason().isEmpty());
    CHECK (rig.plugin->getLatencySamples() == 64);
    CHECK (stateOf (*rig.plugin).startsWith ("gain=1.500"));
    rig.native->noteOn (0, 2, 69, 0.8);
    std::ignore = rig.block();
    CHECK (rig.buffer.getMagnitude (0, 0, blockSize) > 0.1f);
}

TEST_CASE ("a plugin that hangs is closed, and never holds up a block for long", "[remote][failure]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    Failing rig;
    rig.native->noteOn (0, 1, hangKey, 0.8);
    const auto first = rig.block();
    CHECK (first < 30.0); // a block's own length, 10.7 ms, and some room

    double slowest = 0.0;
    const auto until = juce::Time::getMillisecondCounterHiRes() + 6000.0;

    while (! rig.remote->crashed() && juce::Time::getMillisecondCounterHiRes() < until)
    {
        slowest = std::max (slowest, rig.block());
        juce::Thread::sleep (10);
    }

    REQUIRE (rig.remote->crashed());
    CHECK (slowest < 5.0); // a helper already known to be busy isn't waited for at all
    CHECK (rig.remote->crashReason().contains ("stopped responding"));
    CHECK (rig.remote->lateBlocks() > 0);

    juce::String error;
    CHECK (rig.remote->reload (error));
}

TEST_CASE ("a late block loses no notes", "[remote][failure]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    Failing rig;
    rig.native->noteOn (0, 1, slowKey, 0.8);
    const auto late = rig.block();
    CHECK (late < 30.0);
    CHECK (rig.remote->lateBlocks() == 1);

    // While the plugin is still busy, a note-off and MIDI arrive. They wait.
    rig.native->noteOff (0, 1, slowKey, 0.5);
    rig.midi.addEvent (juce::MidiMessage::noteOn (1, 69, 0.8f), 100);
    std::ignore = rig.block();
    rig.midi.clear();
    juce::Thread::sleep (100);
    std::ignore = rig.block();
    std::ignore = rig.block();

    const auto state = stateOf (*rig.plugin);
    CHECK (reported (state, "off") == 1);
    CHECK (reported (state, "midi") == 1);
    CHECK (rig.buffer.getMagnitude (0, 0, blockSize) > 0.1f); // that note-on sounds
    CHECK_FALSE (rig.remote->crashed());
}

TEST_CASE ("a plugin with a dead sibling comes back in a new helper", "[remote][failure]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    Failing first;
    Failing second; // the same plugin file, so the same helper
    first.native->noteOn (0, 1, crashKey, 0.8);
    std::ignore = first.block();
    REQUIRE (first.waitForCrash (2.0));
    REQUIRE (second.waitForCrash (2.0));

    juce::String error;
    REQUIRE (second.remote->reload (error));
    REQUIRE (first.remote->reload (error));
    second.native->noteOn (0, 2, 69, 0.8);
    first.native->noteOn (0, 2, 72, 0.8);
    std::ignore = second.block();
    CHECK (second.buffer.getMagnitude (0, 0, blockSize) > 0.1f);
    std::ignore = first.block();
    CHECK (first.buffer.getMagnitude (0, 0, blockSize) > 0.1f);
}

TEST_CASE ("a crash keeps what the last snapshot saw", "[remote][failure]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    setRemotePluginSnapshotInterval (0.1);
    {
        Failing rig;
        REQUIRE (rig.plugin->getParameters().size() >= 1);
        rig.plugin->getParameters()[0]->setValueNotifyingHost (0.25f);
        std::ignore = rig.block();
        const auto expected = stateOf (*rig.plugin).upToFirstOccurrenceOf (" ", false, false);
        juce::Thread::sleep (500); // long enough for a snapshot to be taken, and answered

        rig.native->noteOn (0, 1, crashKey, 0.8);
        std::ignore = rig.block();
        REQUIRE (rig.waitForCrash (2.0));
        CHECK (stateOf (*rig.plugin).upToFirstOccurrenceOf (" ", false, false) == expected);
    }
    setRemotePluginSnapshotInterval (30.0);
}

TEST_CASE ("a plugin's editor window is the helper's", "[remote][editor]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    Failing rig;
    const auto ignored = [] (std::optional<RemoteEditorPosition>) {};

    // Reports where it is, whether placed or not.
    rig.remote->openEditor ("Editor", RemoteEditorPosition { 30, 40 }, false, ignored);
    auto where = rig.remote->closeEditor();
    REQUIRE (where.has_value());
    CHECK (where->x == 30);
    CHECK (where->y == 40);

    // Closed, there's nothing to report.
    CHECK_FALSE (rig.remote->closeEditor().has_value());

    // Is brought to the front when opened twice, and floats and hides as asked.
    rig.remote->openEditor ("Editor", std::nullopt, false, ignored);
    rig.remote->openEditor ("Editor", std::nullopt, false, ignored);
    where = rig.remote->closeEditor();
    REQUIRE (where.has_value());
    CHECK (where->x == 2);

    rig.remote->openEditor ("Editor", std::nullopt, false, ignored);
    rig.remote->showEditor (false, false);
    where = rig.remote->closeEditor();
    REQUIRE (where.has_value());
    CHECK (where->x == 200);
    CHECK (where->y == 20);
}

TEST_CASE ("an editor window going away is reported", "[remote][editor]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    Failing rig;
    std::atomic<int> gone { 0 };
    std::atomic<int> placedAt { 0 };
    const auto report = [&] (std::optional<RemoteEditorPosition> where)
    {
        placedAt.store (where.has_value() ? where->x : -1);
        gone.fetch_add (1);
    };
    const auto waitFor = [&] (int count)
    {
        const auto until = juce::Time::getMillisecondCounterHiRes() + 2000.0;

        while (gone.load() < count && juce::Time::getMillisecondCounterHiRes() < until)
            juce::Thread::sleep (5);

        return gone.load() == count;
    };

    // By the user, with its position.
    rig.remote->openEditor ("close me", RemoteEditorPosition { 77, 5 }, false, report);
    REQUIRE (waitFor (1));
    CHECK (placedAt.load() == 77);
    CHECK_FALSE (rig.remote->closeEditor().has_value());

    // Closed from here, no report.
    rig.remote->openEditor ("Editor", std::nullopt, false, report);
    CHECK (rig.remote->closeEditor().has_value());
    juce::Thread::sleep (50);
    CHECK (gone.load() == 1);

    // With a crash, once and with no position; and a reloaded plugin can open it again.
    rig.remote->openEditor ("Editor", std::nullopt, false, report);
    rig.native->noteOn (0, 1, crashKey, 0.8);
    std::ignore = rig.block();
    REQUIRE (rig.waitForCrash (2.0));
    REQUIRE (waitFor (2));
    CHECK (placedAt.load() == -1);
    CHECK_FALSE (rig.remote->closeEditor().has_value());

    juce::String error;
    REQUIRE (rig.remote->reload (error));
    rig.remote->openEditor ("Editor", RemoteEditorPosition { 9, 9 }, false, report);
    CHECK (rig.remote->closeEditor().has_value());
}

namespace
{
// The peak of a plugin's left output over some blocks, with a note held from the first.
[[nodiscard]] float playNote (juce::AudioPluginInstance& plugin, int blocks)
{
    const auto channels = std::max ({ 2, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels() });
    juce::AudioBuffer<float> buffer (channels, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
    auto peak = 0.0f;

    for (int block = 0; block < blocks; ++block)
    {
        buffer.clear();
        plugin.processBlock (buffer, midi);
        midi.clear();
        peak = std::max (peak, buffer.getMagnitude (0, blockSize));
    }

    buffer.clear();
    midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
    plugin.processBlock (buffer, midi);
    return peak;
}
} // namespace

// By hand, against an installed CLAP instrument (docs/design/out-of-process-plugins.md, step 7):
// DAWG_REAL_PLUGIN=<file> [DAWG_REAL_PLUGIN_NAME=<name>] dawg_tests "[.real-remote]". It loads, plays and keeps its state
// in a helper, and on Linux comes back with Reload after its helper is killed.
TEST_CASE ("an installed plugin plays, saves and reloads in another process", "[.real-remote]")
{
    const auto file = juce::SystemStats::getEnvironmentVariable ("DAWG_REAL_PLUGIN", {});
    REQUIRE (file.isNotEmpty());
    REQUIRE (remotePluginsAvailable());

    auto name = juce::SystemStats::getEnvironmentVariable ("DAWG_REAL_PLUGIN_NAME", {});

    if (name.isEmpty())
    {
        ClapPluginFormat format;
        juce::OwnedArray<juce::PluginDescription> found;
        format.findAllTypesForFile (found, file);
        REQUIRE (! found.isEmpty());
        name = found[0]->name;
    }

    auto plugin = loadRemote (name, file);
    auto* remote = dynamic_cast<RemoteInstance*> (plugin.get());
    REQUIRE (remote != nullptr);
    plugin->enableAllBuses();
    plugin->prepareToPlay (sampleRate, blockSize);
    WARN (name << ": " << plugin->getParameters().size() << " parameters, latency " << plugin->getLatencySamples());

    const auto peak = playNote (*plugin, 100);
    WARN (name << ": peak " << peak);
    CHECK (peak > 0.001f);
    CHECK (remote->lateBlocks() == 0);

    // A parameter moved here comes back in a second instance from the saved state.
    auto& parameters = plugin->getParameters();
    REQUIRE (! parameters.isEmpty());
    auto* moved = parameters[0];
    const auto target = moved->getValue() < 0.5f ? 0.8f : 0.2f;
    moved->setValueNotifyingHost (target);

    // A host's change reaches a CLAP plugin with its next block, as in a running session.
    std::ignore = playNote (*plugin, 20);
    juce::Thread::sleep (200);
    const auto value = moved->getValue();
    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    WARN (name << ": " << moved->getName (64) << " set to " << target << ", reads " << value << "; state of "
               << state.getSize() << " bytes");

    auto copy = loadRemote (name, file);
    copy->prepareToPlay (sampleRate, blockSize);
    copy->setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    // Some plugins (Surge XT) take a state in on their audio thread, and say the new values once they have.
    std::ignore = playNote (*copy, 20);
    juce::Thread::sleep (300);
    CHECK_THAT (copy->getParameters()[0]->getValue(), WithinAbs (value, 1.0e-3));
    copy.reset();

#ifdef __linux__
    // Killed from outside: known as a crash, and Reload brings it back with its state.
    // The bracket keeps pkill's own shell, whose command line holds the pattern, from matching it.
    const auto pattern = juce::String ("[") + remotePluginHelperId[0] + "]" + (remotePluginHelperId + 1);
    REQUIRE (std::system (("pkill -KILL -f -- '" + pattern + "'").toRawUTF8()) == 0);
    const auto until = juce::Time::getMillisecondCounterHiRes() + 3000.0;

    while (! remote->crashed() && juce::Time::getMillisecondCounterHiRes() < until)
    {
        std::ignore = playNote (*plugin, 1);
        juce::Thread::sleep (10);
    }

    REQUIRE (remote->crashed());
    WARN (name << ": " << remote->crashReason());
    juce::String error;
    REQUIRE (remote->reload (error));
    plugin->prepareToPlay (sampleRate, blockSize);
    CHECK (playNote (*plugin, 100) > 0.001f);
    juce::Thread::sleep (300);
    CHECK_THAT (plugin->getParameters()[0]->getValue(), WithinAbs (value, 1.0e-3));
#endif
}

TEST_CASE ("a plugin in another process changes its parameter list like one inside", "[remote]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    const juce::String extra ("gain=1.000 extra=1");

    auto fresh = loadRemote ("Test Gain");
    REQUIRE (fresh->getParameters().size() == 1);
    fresh->setStateInformation (extra.toRawUTF8(), static_cast<int> (extra.getNumBytesAsUTF8()));
    REQUIRE (fresh->getParameters().size() == 2);
    CHECK (fresh->getParameters()[1]->getName (64) == "Extra");

    auto running = loadRemote ("Test Gain");
    std::atomic<int> calls { 0 };
    dynamic_cast<ParameterListSource*> (running.get())->setParameterListCallback ([&] { calls.fetch_add (1); });
    running->prepareToPlay (sampleRate, blockSize);
    running->setStateInformation (extra.toRawUTF8(), static_cast<int> (extra.getNumBytesAsUTF8()));
    const auto until = juce::Time::getMillisecondCounterHiRes() + 3000.0;

    while (calls.load() == 0 && juce::Time::getMillisecondCounterHiRes() < until)
        juce::Thread::sleep (5);

    CHECK (calls.load() == 1);
    CHECK (running->getParameters().size() == 1);
}

TEST_CASE ("a plugin in another process saves for a copy through its state context", "[remote]")
{
    if (! remotePluginsAvailable())
        SKIP ("No plugin processes on this platform yet");

    auto plugin = loadRemote ("Test Gain");
    CHECK (stateOf (*plugin).contains ("why=3"));
    auto* forCopy = dynamic_cast<DuplicateStateSource*> (plugin.get());
    REQUIRE (forCopy != nullptr);
    juce::MemoryBlock state;
    forCopy->getStateForDuplicate (state);
    CHECK (state.toString().contains ("why=2"));
}
