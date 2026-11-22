#include "TestUtils.h"
#include "dawg/AudioEngine.h"
#include "dawg/ClapPlugin.h"
#include "dawg/GraphCompiler.h"
#include "dawg/NodeRegistry.h"
#include "dawg/PluginNode.h"
#include "dawg/Sequence.h"
#include "dawg/VarUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>

// The CLAP host (docs/design/clap-hosting.md), against the test plugins in tests/clap.

using namespace dawg;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr auto sampleRate = 48000.0;
constexpr auto blockSize = 512;

[[nodiscard]] juce::OwnedArray<juce::PluginDescription> scan()
{
    ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, DAWG_TEST_CLAP_PATH);
    return found;
}

[[nodiscard]] std::unique_ptr<juce::AudioPluginInstance> load (const juce::String& name)
{
    ClapPluginFormat format;
    const auto found = scan();

    const juce::PluginDescription* match = nullptr;
    for (const auto* description : found)
        if (description->name == name)
            match = description;

    INFO ("No plugin called " << name);
    REQUIRE (match != nullptr);
    juce::String error;
    auto instance = format.createInstanceFromDescription (*match, sampleRate, blockSize, error);
    CHECK (error.isEmpty());
    return instance;
}

[[nodiscard]] juce::String stateOf (juce::AudioProcessor& plugin)
{
    juce::MemoryBlock state;
    plugin.getStateInformation (state);
    return state.toString();
}

[[nodiscard]] float peakOf (const juce::AudioBuffer<float>& buffer, int channel)
{
    return buffer.getMagnitude (channel, 0, buffer.getNumSamples());
}
} // namespace

TEST_CASE ("scanning a CLAP file lists its plugins without opening them", "[clap]")
{
    const auto found = scan();
    REQUIRE (found.size() == 2);

    CHECK (found[0]->name == "Test Sine");
    CHECK (found[0]->pluginFormatName == "CLAP");
    CHECK (found[0]->manufacturerName == "DAWg");
    CHECK (found[0]->isInstrument);
    CHECK (found[0]->descriptiveName == "org.dawg.test.sine");

    // The category is the plugin's features, for the add menus' folders.
    CHECK (found[0]->category == "instrument|synthesizer|stereo");

    CHECK (found[1]->name == "Test Gain");
    CHECK_FALSE (found[1]->isInstrument);
    CHECK (found[1]->category == "audio-effect|utility");
    CHECK (found[0]->createIdentifierString() != found[1]->createIdentifierString());

    ClapPluginFormat format;
    CHECK (format.fileMightContainThisPluginType ("x/y.clap"));
    CHECK_FALSE (format.fileMightContainThisPluginType ("x/y.vst3"));
    CHECK (format.doesPluginStillExist (*found[0]));
    CHECK (format.getDefaultLocationsToSearch().getNumPaths() >= 2);
}

TEST_CASE ("a CLAP instrument plays notes that arrive as MIDI, on the audio thread's terms", "[clap]")
{
    auto plugin = load ("Test Sine");
    REQUIRE (plugin != nullptr);
    plugin->prepareToPlay (sampleRate, blockSize);
    CHECK (plugin->getLatencySamples() == 64);
    CHECK (plugin->getTotalNumOutputChannels() == 2);
    CHECK (plugin->getTotalNumInputChannels() == 0);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 69, 0.8f), 10);
    plugin->processBlock (buffer, midi);
    CHECK (peakOf (buffer, 0) > 0.2f);
    CHECK (midi.isEmpty());

    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOff (1, 69), 0);
    buffer.clear();
    plugin->processBlock (buffer, midi);
    CHECK (peakOf (buffer, 1) == 0.0f);

    const auto state = stateOf (*plugin);
    CHECK (state.contains ("on=1"));
    CHECK (state.contains ("off=1"));
    CHECK (state.contains ("midi=2"));
    CHECK (state.contains ("wrong=0"));

    plugin->releaseResources();
}

TEST_CASE ("a CLAP effect processes audio, and its parameter moves it", "[clap]")
{
    auto plugin = load ("Test Gain");
    REQUIRE (plugin != nullptr);
    plugin->prepareToPlay (sampleRate, blockSize);
    CHECK (plugin->getTotalNumInputChannels() == 2);

    const auto& parameters = plugin->getParameters();
    REQUIRE (parameters.size() == 1);
    auto* gain = parameters[0];
    CHECK (gain->getName (32) == "Gain");
    CHECK_THAT (gain->getValue(), WithinAbs (0.5, 1e-6));
    CHECK (gain->getText (0.5f, 32) == "1.00 x");
    CHECK (dynamic_cast<juce::HostedAudioProcessorParameter*> (gain)->getParameterID() == "7");

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    const auto fill = [&buffer]
    {
        for (int ch = 0; ch < 2; ++ch)
            buffer.clear (ch, 0, blockSize);

        for (int s = 0; s < blockSize; ++s)
            buffer.setSample (0, s, 0.5f);
    };

    fill();
    plugin->processBlock (buffer, midi);
    CHECK_THAT (buffer.getSample (0, 100), WithinAbs (0.5, 1e-6));

    gain->setValue (1.0f); // twice as loud, from the next block
    fill();
    plugin->processBlock (buffer, midi);
    CHECK_THAT (buffer.getSample (0, 100), WithinAbs (1.0, 1e-6));
    CHECK_THAT (buffer.getSample (1, 100), WithinAbs (0.0, 1e-6));

    plugin->releaseResources();
}

TEST_CASE ("a CLAP plugin's state round-trips, and shows in its parameters", "[clap]")
{
    auto first = load ("Test Gain");
    REQUIRE (first != nullptr);
    first->prepareToPlay (sampleRate, blockSize);
    first->getParameters()[0]->setValue (0.25f);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    first->processBlock (buffer, midi);

    juce::MemoryBlock saved;
    first->getStateInformation (saved);
    first->releaseResources();

    auto second = load ("Test Gain");
    REQUIRE (second != nullptr);
    second->setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    CHECK_THAT (second->getParameters()[0]->getValue(), WithinAbs (0.25, 1e-3));
}

TEST_CASE ("a parameter set before a CLAP plugin is active reaches it", "[clap]")
{
    auto plugin = load ("Test Gain");
    REQUIRE (plugin != nullptr);
    plugin->getParameters()[0]->setValue (1.0f);
    plugin->prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    buffer.clear();

    for (int s = 0; s < blockSize; ++s)
        buffer.setSample (0, s, 0.5f);

    plugin->processBlock (buffer, midi);
    CHECK_THAT (buffer.getSample (0, 100), WithinAbs (1.0, 1e-6));
    plugin->releaseResources();
}

TEST_CASE ("a plugin node runs a CLAP instrument", "[clap][engine]")
{
    auto plugin = load ("Test Sine");
    REQUIRE (plugin != nullptr);
    PluginNode node (true);
    node.prepare ({ sampleRate, blockSize });
    node.attach (std::move (plugin));
    CHECK (node.status() == PluginNode::Status::ready);
    CHECK (node.latencySamples() == 64);
    CHECK (node.parameterIndex ("7") == 0);
}

namespace
{
// A plugin node running the test instrument, with notes from a clip played by hand-made events.
struct NativeRig
{
    explicit NativeRig (std::initializer_list<juce::var> notes)
    {
        auto setup = test::projectWithClip();
        clipId = setup.clipId;
        juce::Array<juce::var> list;

        for (const auto& note : notes)
            list.add (note);

        project = test::apply (setup.project, "addNotes", { { "clipId", clipId }, { "notes", list } });
        sequence = std::make_unique<TrackSequence> (project.tracks[0]->clips);
        node.prepare ({ sampleRate, blockSize });
        auto plugin = load ("Test Sine");
        REQUIRE (plugin != nullptr);
        node.attach (std::move (plugin));
        transport.playing = true;
        transport.timeMap = TimeMap (test::constantTempo(), sampleRate);
        audio.setSize (2, blockSize);
    }

    // One block from a tick; returns the left channel.
    std::vector<float> block (double startTick, std::initializer_list<NoteEvent> events)
    {
        EventBuffer buffer;

        for (const auto& event : events)
            std::ignore = buffer.add (event);

        transport.startTick = startTick;
        transport.endTick = startTick + blockSize * static_cast<double> (ticksPerBeat) / 24000.0;

        const std::array<AudioInput, 1> inputs { AudioInput { nullptr, nullptr } };
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

    [[nodiscard]] NoteEvent off (size_t index) const
    {
        return { 0, NoteEventType::noteOff, sequence->notes()[index].id, nullptr };
    }

    [[nodiscard]] juce::String state() { return stateOf (*node.processor()); }

    Project project;
    ObjectId clipId = 0;
    std::unique_ptr<TrackSequence> sequence;
    PluginNode node { true };
    Transport transport;
    juce::AudioBuffer<float> audio;
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

[[nodiscard]] double frequencyOver (NativeRig& rig, double fromTick, int blocks)
{
    std::vector<float> all;

    for (int i = 0; i < blocks; ++i)
    {
        const auto part = rig.block (fromTick + i * blockSize * static_cast<double> (ticksPerBeat) / 24000.0, {});
        all.insert (all.end(), part.begin(), part.end());
    }

    return hertzOf (all);
}
} // namespace

TEST_CASE ("a CLAP instrument gets notes by id, not MIDI", "[clap][native]")
{
    NativeRig rig ({ test::noteArgs (0, 4 * ticksPerBeat, 69) });
    CHECK (rig.node.usesNativeNotes());
    std::ignore = rig.block (0.0, { rig.on (0) });

    const auto state = rig.state();
    CHECK (state.contains ("on=1"));
    CHECK (state.contains ("midi=0"));
    CHECK (state.contains ("wrong=0"));
    CHECK_THAT (frequencyOver (rig, static_cast<double> (blockSize), 8), WithinAbs (440.0, 15.0));
}

TEST_CASE ("a note's pitch curve reaches a CLAP plugin as tuning", "[clap][native]")
{
    auto note = test::noteArgs (0, 4 * ticksPerBeat, 45);
    juce::Array<juce::var> point;
    point.add (0);
    point.add (48.0); // four octaves, the most a curve holds
    juce::Array<juce::var> curve;
    curve.add (point);
    note.getDynamicObject()->setProperty ("expression", makeObject ({ { "pitch", curve } }));

    NativeRig rig ({ note });
    std::ignore = rig.block (0.0, { rig.on (0) });
    CHECK (rig.state().contains ("expr="));
    CHECK_THAT (frequencyOver (rig, static_cast<double> (blockSize), 8) / 1760.0, WithinAbs (1.0, 0.03));
}

TEST_CASE ("a linked note keeps its CLAP note and glides by tuning", "[clap][native][links]")
{
    NativeRig rig ({ test::noteArgs (0, ticksPerBeat, 69), test::noteArgs (ticksPerBeat, ticksPerBeat, 81) });
    const auto links = [&]
    {
        juce::Array<juce::var> list;
        list.add (makeObject ({ { "from", rig.project.tracks[0]->clips[0]->notes[0]->id },
                                { "to", rig.project.tracks[0]->clips[0]->notes[1]->id },
                                { "glideMs", 0.0 } }));
        return list;
    }();
    rig.project = test::apply (rig.project, "setNoteLinks", { { "clipId", rig.clipId }, { "links", links } });
    rig.sequence = std::make_unique<TrackSequence> (rig.project.tracks[0]->clips);

    std::ignore = rig.block (0.0, { rig.on (0) });
    const auto& second = rig.sequence->notes()[1];
    NoteEvent legato { 0, NoteEventType::legato, second.id, &second };
    legato.fromNoteId = rig.sequence->notes()[0].id;
    const auto start = static_cast<double> (ticksPerBeat);
    std::ignore = rig.block (start, { legato });

    const auto state = rig.state();
    CHECK (state.contains ("on=1")); // one note on
    CHECK (state.contains ("off=0"));
    CHECK_THAT (frequencyOver (rig, start + blockSize * static_cast<double> (ticksPerBeat) / 24000.0, 8),
                WithinAbs (880.0, 25.0));

    // The later note's note-off ends the one note.
    std::ignore = rig.block (2.0 * ticksPerBeat, { rig.off (1) });
    CHECK (rig.state().contains ("off=1"));
}

TEST_CASE ("a voice's state goes when the plugin says it has ended", "[clap][native]")
{
    ClapNoteEncoder encoder;
    encoder.prepare (sampleRate, blockSize);
    encoder.noteEnded (5); // nothing there: harmless
    CHECK (encoder.soundingNotes() == 0);

    NativeRig rig ({ test::noteArgs (0, ticksPerBeat, 60) });
    std::ignore = rig.block (0.0, { rig.on (0) });
    std::ignore = rig.block (static_cast<double> (ticksPerBeat), { rig.off (0) });
    CHECK (rig.state().contains ("off=1"));
}

TEST_CASE ("the sustain pedal holds a CLAP note back", "[clap][native]")
{
    NativeRig rig ({ test::noteArgs (0, ticksPerBeat, 60) });
    NoteEvent down { 0, NoteEventType::sustain, 0, nullptr };
    down.value = 1.0f;
    std::ignore = rig.block (0.0, { rig.on (0), down });
    std::ignore = rig.block (static_cast<double> (ticksPerBeat), { rig.off (0) });
    CHECK (rig.state().contains ("off=0"));

    NoteEvent up { 0, NoteEventType::sustain, 0, nullptr };
    up.value = 0.0f;
    std::ignore = rig.block (2.0 * ticksPerBeat, { up });
    CHECK (rig.state().contains ("off=1"));
}

TEST_CASE ("modulation reaches a modulatable CLAP parameter apart from its value", "[clap][native]")
{
    NativeRig rig ({ test::noteArgs (0, 4 * ticksPerBeat, 69) });
    const auto gain = rig.node.parameterIndex ("7");
    REQUIRE (gain == 0);
    CHECK (rig.node.isModulatable (gain));

    std::ignore = rig.block (0.0, { rig.on (0) });
    const auto loudest = [] (const std::vector<float>& samples)
    {
        float top = 0.0f;

        for (const auto sample : samples)
            top = std::max (top, std::abs (sample));

        return top;
    };

    const auto plain = loudest (rig.block (static_cast<double> (blockSize) * 2.0, {}));
    rig.node.modulateParameter (gain, 0.5f); // half the 0..2 range: +1
    const auto modulated = loudest (rig.block (static_cast<double> (blockSize) * 4.0, {}));
    CHECK_THAT (static_cast<double> (modulated / plain), WithinAbs (2.0, 0.1));

    // The value stays where it was, for automation and the plugin's own editor.
    CHECK_THAT (rig.node.parameterValue (gain), WithinAbs (0.5, 1e-6));
    CHECK (rig.state().contains ("mods=1"));

    rig.node.modulateParameter (gain, 0.0f);
    const auto back = loudest (rig.block (static_cast<double> (blockSize) * 6.0, {}));
    CHECK_THAT (static_cast<double> (back / plain), WithinAbs (1.0, 0.05));
}

TEST_CASE ("a note's pressure moves a CLAP parameter for that note alone, through Note Expression",
           "[clap][native][engine]")
{
    auto note = test::noteArgs (0, 4 * ticksPerBeat, 69);
    juce::Array<juce::var> point;
    point.add (0);
    point.add (0.75);
    juce::Array<juce::var> curve;
    curve.add (point);
    note.getDynamicObject()->setProperty ("expression", makeObject ({ { "pressure", curve } }));

    auto setup = test::projectWithClip();
    const auto trackId = setup.trackId;
    juce::Array<juce::var> notes;
    notes.add (note);
    auto project = test::apply (setup.project, "addNotes", { { "clipId", setup.clipId }, { "notes", notes } });

    // The test instrument in place of the track's, with Note Expression's pressure on its gain.
    const auto found = scan();
    const auto plugin = makeObject ({ { "identifier", found[0]->createIdentifierString() },
                                      { "name", found[0]->name },
                                      { "manufacturer", found[0]->manufacturerName },
                                      { "format", found[0]->pluginFormatName } });
    project = test::apply (project, "insertNode",
                           { { "trackId", trackId }, { "type", nodetype::pluginInstrument }, { "plugin", plugin } });
    const auto idOf = [&project] (const char* type)
    {
        for (const auto& node : project.tracks[0]->graph.nodes)
            if (node.type == type)
                return node.id;

        return ObjectId { 0 };
    };
    const auto instrument = idOf (nodetype::pluginInstrument);
    const auto sequencer = idOf (nodetype::sequencer);
    project = test::apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::noteExpression } });
    const auto expression = idOf (nodetype::noteExpression);
    project = test::apply (project, "connect",
                           { { "trackId", trackId },
                             { "fromNode", sequencer },
                             { "fromPort", 0 },
                             { "toNode", expression },
                             { "toPort", 0 } });
    project = test::apply (project, "connect",
                           { { "trackId", trackId },
                             { "fromNode", expression },
                             { "fromPort", 1 },
                             { "toNode", instrument },
                             { "toPort", 1 } });
    project = test::apply (
        project, "setModulation",
        { { "trackId", trackId }, { "nodeId", instrument }, { "port", 1 }, { "param", "7" }, { "depth", 0.5 } });

    GraphCompiler compiler;
    compiler.setPluginLoader ([] (ObjectId, const GraphNode&, const std::shared_ptr<PluginNode>& target)
                              { target->attach (load ("Test Sine")); });
    std::ignore = compiler.compile (project, test::spec);
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, test::spec));
    engine.play();
    const auto output = test::render (engine, 12000).left();

    auto* node = dynamic_cast<PluginNode*> (compiler.findInstance (trackId, instrument));
    REQUIRE (node != nullptr);
    const auto state = stateOf (*node->processor());
    CHECK (state.contains ("notemods=1"));
    CHECK (state.contains ("mods=1")); // no shared amount was sent besides

    // Gain 1 + pressure 0.75 x depth 0.5 x range 2 = 1.75, on a sine of 0.25.
    float top = 0.0f;

    for (size_t i = 4000; i < output.size(); ++i)
        top = std::max (top, std::abs (output[i]));

    CHECK_THAT (static_cast<double> (top), WithinAbs (0.25 * 1.75, 0.02));
}

TEST_CASE ("per-note amounts include what other slots add, and go back when the slot goes", "[clap][native]")
{
    NativeRig rig ({ test::noteArgs (0, 4 * ticksPerBeat, 69) });
    CHECK (rig.node.isModulatablePerNote (0));
    const std::array<NoteModulation, 1> slots {
        NoteModulation { .parameter = 0, .source = 1, .depth = 0.5f, .shared = 0.25f, .nodes = {}, .port = 0 }
    }; // pressure, which is 0 here
    const auto top = [] (const std::vector<float>& samples)
    {
        float result = 0.0f;

        for (const auto sample : samples)
            result = std::max (result, std::abs (sample));

        return result;
    };

    rig.noteModulations = slots;
    std::ignore = rig.block (0.0, { rig.on (0) });
    std::ignore = rig.block (static_cast<double> (blockSize), {});
    CHECK_THAT (static_cast<double> (top (rig.block (2.0 * blockSize, {}))), WithinAbs (0.25 * 1.5, 0.02));
    CHECK (rig.state().contains ("notemods=1"));

    rig.noteModulations = {};
    std::ignore = rig.block (3.0 * blockSize, {});
    CHECK_THAT (static_cast<double> (top (rig.block (4.0 * blockSize, {}))), WithinAbs (0.25, 0.02));
    CHECK (rig.state().contains ("notemods=2"));
}

namespace
{
// The test instrument with a modulation source on its gain, playing two overlapping notes through the engine.
[[nodiscard]] juce::String playWithSource (const char* type,
                                           std::initializer_list<std::pair<const char*, juce::var>> sourceParams)
{
    auto setup = test::projectWithClip();
    const auto trackId = setup.trackId;
    juce::Array<juce::var> notes;
    notes.add (test::noteArgs (0, 2 * ticksPerBeat, 69));
    notes.add (test::noteArgs (ticksPerBeat / 8, 2 * ticksPerBeat, 72));
    auto project = test::apply (setup.project, "addNotes", { { "clipId", setup.clipId }, { "notes", notes } });

    const auto found = scan();
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
    project = test::apply (project, "addNode", { { "trackId", trackId }, { "type", type } });
    const auto source = idOf (type);

    for (const auto& [param, value] : sourceParams)
        project =
            test::apply (project, "setNodeParam",
                         { { "trackId", trackId }, { "nodeId", source }, { "param", param }, { "value", value } });

    project = test::apply (project, "connect",
                           { { "trackId", trackId },
                             { "fromNode", idOf (nodetype::sequencer) },
                             { "fromPort", 0 },
                             { "toNode", source },
                             { "toPort", 0 } });
    project = test::apply (project, "connect",
                           { { "trackId", trackId },
                             { "fromNode", source },
                             { "fromPort", 0 },
                             { "toNode", instrument },
                             { "toPort", 1 } });
    project = test::apply (
        project, "setModulation",
        { { "trackId", trackId }, { "nodeId", instrument }, { "port", 1 }, { "param", "7" }, { "depth", 0.25 } });

    GraphCompiler compiler;
    compiler.setPluginLoader ([] (ObjectId, const GraphNode&, const std::shared_ptr<PluginNode>& target)
                              { target->attach (load ("Test Sine")); });
    std::ignore = compiler.compile (project, test::spec);
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, test::spec));
    engine.play();
    std::ignore = test::render (engine, 24000);

    auto* node = dynamic_cast<PluginNode*> (compiler.findInstance (trackId, instrument));
    REQUIRE (node != nullptr);
    return stateOf (*node->processor());
}

[[nodiscard]] int reported (const juce::String& state, const juce::String& field)
{
    return state.fromFirstOccurrenceOf (" " + field + "=", false, false).getIntValue();
}
} // namespace

TEST_CASE ("an ADSR on a CLAP instrument runs per note", "[clap][native][engine]")
{
    const auto state = playWithSource (nodetype::adsr, { { "attackMs", 100.0 } });
    CHECK (reported (state, "modids") == 2); // each note heard from its own envelope
    CHECK (reported (state, "notemods") > 10);
    CHECK (reported (state, "mods") >= reported (state, "notemods"));
    CHECK (reported (state, "wrong") == 0);
}

TEST_CASE ("an LFO on a CLAP instrument runs per note", "[clap][native][engine]")
{
    const auto state = playWithSource (nodetype::lfo, { { "retrigger", 1.0 } });
    CHECK (reported (state, "modids") == 2);
    CHECK (reported (state, "notemods") > 10);
}

TEST_CASE ("automation reaches a CLAP plugin inside a block", "[clap][native]")
{
    NativeRig rig ({ test::noteArgs (0, 4 * ticksPerBeat, 69) });
    REQUIRE (rig.node.takesParameterEvents());
    std::ignore = rig.block (0.0, { rig.on (0) });
    rig.node.automateParameter (0, 1.0f, 256); // to twice the gain, half way through the block
    const auto output = rig.block (static_cast<double> (blockSize), {});
    const auto top = [&output] (size_t from, size_t to)
    {
        float result = 0.0f;

        for (auto i = from; i < to; ++i)
            result = std::max (result, std::abs (output[i]));

        return static_cast<double> (result);
    };

    CHECK_THAT (top (0, 256), WithinAbs (0.25, 0.05));
    CHECK_THAT (top (256, 512), WithinAbs (0.5, 0.05));
    CHECK_THAT (rig.node.parameterValue (0), WithinAbs (1.0, 1e-6));
}

TEST_CASE ("a CLAP plugin is found by its id when its file has moved", "[clap]")
{
    const auto found = scan();
    juce::Array<juce::PluginDescription> known;
    known.add (*found[0]);
    known.add (*found[1]);

    auto moved = *found[1];
    moved.fileOrIdentifier = "D:/Somewhere/else/Old.clap";
    const auto saved = moved.createIdentifierString();
    CHECK (saved != found[1]->createIdentifierString());

    const auto again = findMovedClapPlugin (known, saved);
    REQUIRE (again.has_value());
    CHECK (again->descriptiveName == found[1]->descriptiveName);
    CHECK_FALSE (findMovedClapPlugin (known, "VST3-Other-1234-5678").has_value());
    CHECK_FALSE (findMovedClapPlugin (known, "CLAP-Other-1234-5678").has_value());
}

TEST_CASE ("exporting tells a CLAP plugin it is rendering offline", "[clap]")
{
    auto plugin = load ("Test Sine");
    REQUIRE (plugin != nullptr);
    plugin->setNonRealtime (true);
    CHECK (reported (" " + stateOf (*plugin), "render") == 1);
    plugin->setNonRealtime (false);
    CHECK (reported (" " + stateOf (*plugin), "render") == 0);
}

namespace
{
// The native path of an installed instrument: a note by id, then the same note tuned an octave up by a note
// expression, then a per-note amount on a gain.
void checkNativeNotes (juce::AudioPluginInstance& plugin, ClapNativeInput* native,
                       const juce::PluginDescription& description, juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midi)
{
    WARN (description.name << ": native notes " << (native->takesNativeNotes() ? "yes" : "no"));

    const auto hertz = [&]
    {
        std::vector<float> all;

        for (int block = 0; block < 40; ++block)
        {
            buffer.clear();
            plugin.processBlock (buffer, midi);
            all.insert (all.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        }

        return hertzOf (all);
    };

    int perNote = 0;

    for (int i = 0; i < plugin.getParameters().size(); ++i)
        perNote += native->isModulatablePerNote (i) ? 1 : 0;

    WARN (description.name << ": " << perNote << " parameters take modulation per note");

    if (native->takesNativeNotes())
    {
        native->chokeAll (0);
        native->noteOn (0, 1, 45, 0.8);
        std::ignore = hertz();
        const auto plain = hertz();
        native->noteExpression (0, 1, 45, ClapNativeInput::Expression::tuning, 12.0);
        std::ignore = hertz();
        const auto tuned = hertz();
        WARN (description.name << ": " << plain << " Hz, tuned " << tuned << " Hz");
        CHECK (tuned > plain * 1.7);
        native->noteOff (0, 1, 45, 0.5);
        std::ignore = hertz();

        // A per-note amount on a gain moves one note's level.
        const auto& parameters = plugin.getParameters();
        juce::String names;

        for (int i = 0; i < parameters.size() && names.length() < 700; ++i)
            if (native->isModulatablePerNote (i))
                names << parameters[i]->getName (64) << ", ";

        WARN ("Per note: " << names);
        int level = -1;

        for (int i = 0; i < parameters.size() && level < 0; ++i)
            if (native->isModulatablePerNote (i)
                && parameters[i]->getName (64).containsIgnoreCase (
                    juce::SystemStats::getEnvironmentVariable ("DAWG_CLAP_LEVEL", "VCA Gain")))
                level = i;

        if (level >= 0)
        {
            const auto loudness = [&]
            {
                float top = 0.0f;

                for (int block = 0; block < 40; ++block)
                {
                    buffer.clear();
                    plugin.processBlock (buffer, midi);

                    if (block > 20)
                        top = std::max (top, buffer.getMagnitude (0, blockSize));
                }

                return top;
            };

            native->noteOn (0, 2, 57, 0.8);
            const auto open = loudness();
            native->modulateNoteParameter (0, 2, 57, level, -0.5);
            const auto lowered = loudness();
            WARN (description.name << ": " << parameters[level]->getName (64) << " " << open << " -> " << lowered);
            CHECK (lowered < open * 0.8f);
            native->noteOff (0, 2, 57, 0.5);
            std::ignore = loudness();
        }
    }
}
} // namespace

// By hand, against an installed plugin: DAWG_CLAP_FILE=<file> dawg_tests "[.real-clap]"
TEST_CASE ("an installed CLAP plugin loads, plays and saves", "[.real-clap]")
{
    const auto file = juce::SystemStats::getEnvironmentVariable ("DAWG_CLAP_FILE", {});
    REQUIRE (file.isNotEmpty());

    ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, file);
    REQUIRE (! found.isEmpty());

    for (const auto* description : found)
    {
        INFO (description->name << " " << description->descriptiveName);
        juce::String error;
        auto plugin = format.createInstanceFromDescription (*description, sampleRate, blockSize, error);
        REQUIRE (plugin != nullptr);
        plugin->enableAllBuses();
        plugin->prepareToPlay (sampleRate, blockSize);
        WARN (description->name << ": " << plugin->getParameters().size() << " parameters, latency "
                                << plugin->getLatencySamples() << ", " << plugin->getTotalNumInputChannels() << " in, "
                                << plugin->getTotalNumOutputChannels() << " out");

        const auto channels = std::max ({ 2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels() });
        juce::AudioBuffer<float> buffer (channels, blockSize);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        float peak = 0.0f;

        for (int block = 0; block < 100; ++block)
        {
            buffer.clear();
            plugin->processBlock (buffer, midi);
            midi.clear();
            peak = std::max (peak, buffer.getMagnitude (0, blockSize));
        }

        WARN (description->name << ": peak " << peak);

        if (description->isInstrument)
            CHECK (peak > 0.001f);

        // The native path: a note by id, then the same note tuned an octave up by a note expression.
        if (auto* native = dynamic_cast<ClapNativeInput*> (plugin.get());
            native != nullptr && description->isInstrument)
            checkNativeNotes (*plugin, native, *description, buffer, midi);

        juce::MemoryBlock state;
        plugin->getStateInformation (state);
        WARN (description->name << ": state of " << state.getSize() << " bytes");
        plugin->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        plugin->releaseResources();
    }
}

TEST_CASE ("a CLAP plugin's parameter list follows a state before it runs, and asks for a new instance after", "[clap]")
{
    const juce::String extra ("gain=1.000 extra=1");

    // Before the first prepare, the instance reads its list again.
    auto fresh = load ("Test Gain");
    auto* source = dynamic_cast<ParameterListSource*> (fresh.get());
    REQUIRE (source != nullptr);
    int calls = 0;
    source->setParameterListCallback ([&] { ++calls; });
    REQUIRE (fresh->getParameters().size() == 1);
    fresh->setStateInformation (extra.toRawUTF8(), static_cast<int> (extra.getNumBytesAsUTF8()));
    REQUIRE (fresh->getParameters().size() == 2);
    CHECK (fresh->getParameters()[1]->getName (64) == "Extra");
    CHECK (calls == 0);

    // Running, it can't: it asks the host for a new instance.
    auto running = load ("Test Gain");
    dynamic_cast<ParameterListSource*> (running.get())->setParameterListCallback ([&] { ++calls; });
    running->prepareToPlay (sampleRate, blockSize);
    running->setStateInformation (extra.toRawUTF8(), static_cast<int> (extra.getNumBytesAsUTF8()));
    CHECK (calls == 1);
    CHECK (running->getParameters().size() == 1);
}

TEST_CASE ("a CLAP plugin saves for a copy through its state context", "[clap]")
{
    auto plugin = load ("Test Gain");
    CHECK (stateOf (*plugin).contains ("why=3")); // CLAP_STATE_CONTEXT_FOR_PROJECT

    auto* forCopy = dynamic_cast<DuplicateStateSource*> (plugin.get());
    REQUIRE (forCopy != nullptr);
    juce::MemoryBlock state;
    forCopy->getStateForDuplicate (state);
    CHECK (state.toString().contains ("why=2")); // CLAP_STATE_CONTEXT_FOR_DUPLICATE
}

TEST_CASE ("a timer a CLAP plugin registers while it initialises is kept", "[clap]")
{
    // Surge XT's editor registers its idle timer in init(), before the host has an instance for it.
    auto plugin = load ("Test Gain");
    CHECK (stateOf (*plugin).contains ("inittimer=1"));
}
