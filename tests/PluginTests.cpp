#include "TestUtils.h"
#include "dawg/MpeEncoder.h"
#include "dawg/NodeRegistry.h"
#include "dawg/PluginNode.h"
#include "dawg/Sequence.h"
#include "dawg/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int samplesPerBeat = 24000; // 120 BPM at 48 kHz

[[nodiscard]] double offsetMsForSamples (int samples)
{
    return samples * 1000.0 / sampleRate;
}

[[nodiscard]] juce::var array (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

[[nodiscard]] juce::var pluginArgs (const char* name)
{
    return makeObject ({ { "identifier", juce::String ("test-") + name }, { "name", name }, { "format", "VST3" } });
}

[[nodiscard]] const GraphNode& nodeOfType (const Track& track, const char* type)
{
    const auto it = std::ranges::find_if (track.graph.nodes, [type] (const GraphNode& n) { return n.type == type; });
    REQUIRE (it != track.graph.nodes.end());
    return *it;
}

// The node ids along the chain, following port-0 connections from the given node.
[[nodiscard]] std::vector<ObjectId> chainFrom (const TrackGraph& graph, ObjectId start)
{
    std::vector<ObjectId> order { start };

    for (bool advanced = true; advanced;)
    {
        advanced = false;

        for (const auto& c : graph.connections)
        {
            if (c.fromNode == order.back())
            {
                order.push_back (c.toNode);
                advanced = true;
                break;
            }
        }
    }

    return order;
}

// A minimal processor: an effect scales its input, an instrument outputs a constant while any note is held.
// It records the MIDI it receives and can report latency.
class FakeProcessor final : public juce::AudioProcessor
{
public:
    FakeProcessor (bool instrument, float gain, int latency = 0)
        : juce::AudioProcessor (instrument ? BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo())
                                           : BusesProperties()
                                                 .withInput ("In", juce::AudioChannelSet::stereo())
                                                 .withOutput ("Out", juce::AudioChannelSet::stereo())),
          m_instrument (instrument),
          m_gain (gain)
    {
        setLatencySamples (latency);
    }

    const juce::String getName() const override { return "Fake"; }
    void prepareToPlay (double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override { ++prepareCount; }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        if (! m_instrument)
        {
            buffer.applyGain (m_gain);
            return;
        }

        // Sample-accurate: the output level follows the held-note count from each event's position on.
        int position = 0;

        const auto fill = [&] (int to)
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                juce::FloatVectorOperations::fill (buffer.getWritePointer (ch, position), held > 0 ? m_gain : 0.0f,
                                                   to - position);
            position = to;
        };

        for (const auto metadata : midi)
        {
            fill (metadata.samplePosition);
            const auto message = metadata.getMessage();
            received.push_back (message);

            if (message.isNoteOn())
                ++held;
            else if (message.isNoteOff())
                --held;
        }

        fill (buffer.getNumSamples());
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return m_instrument; }
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

    std::vector<juce::MidiMessage> received;
    int held = 0;
    int prepareCount = 0;

private:
    bool m_instrument;
    float m_gain;
};

// A project with one track, a constant "instrument" (an effect on silence would stay silent) and an engine.
struct PluginRig
{
    PluginRig()
    {
        auto setup = projectWithClip();
        project = setup.project;
        trackId = setup.trackId;
        clipId = setup.clipId;
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);

        if (auto compiled = compiler.update (project, spec))
            engine.setEngine (std::move (compiled));
    }

    [[nodiscard]] PluginNode& pluginNode (const char* type)
    {
        const auto& node = nodeOfType (*project.tracks[0], type);
        auto* instance = dynamic_cast<PluginNode*> (compiler.findInstance (trackId, node.id));
        REQUIRE (instance != nullptr);
        return *instance;
    }

    Project project;
    ObjectId trackId = 0;
    ObjectId clipId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

// ---------------------------------------------------------------------------------------------------------
// Chain commands

TEST_CASE ("effects are inserted into the chain before the gain", "[plugins][commands]")
{
    auto setup = projectWithClip();
    auto project =
        apply (setup.project, "insertNode",
               { { "trackId", setup.trackId }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("A") } });
    project = apply (project, "insertNode",
                     { { "trackId", setup.trackId },
                       { "type", nodetype::pluginEffect },
                       { "plugin", pluginArgs ("B") },
                       { "index", 0 } });

    const auto& track = *project.tracks[0];
    REQUIRE (track.graph.nodes.size() == 8);

    std::vector<juce::String> types;
    std::vector<juce::String> names;

    for (const auto id : chainFrom (track.graph, nodeOfType (track, nodetype::sequencer).id))
    {
        const auto& node = track.graph.nodes[static_cast<size_t> (track.graph.indexOfNode (id))];
        types.push_back (node.type);
        names.push_back (node.plugin.has_value() ? node.plugin->name : juce::String());
    }

    // New effects go after the default EQ and compressor; index 0 is the first effect slot.
    CHECK (types
           == std::vector<juce::String> { nodetype::sequencer, nodetype::mpeSynth, nodetype::pluginEffect, nodetype::eq,
                                          nodetype::compressor, nodetype::pluginEffect, nodetype::gain,
                                          nodetype::trackOutput });
    CHECK (names[2] == "B");
    CHECK (names[5] == "A");
}

TEST_CASE ("an instrument replaces the current one", "[plugins][commands]")
{
    auto setup = projectWithClip();
    const auto project = apply (
        setup.project, "insertNode",
        { { "trackId", setup.trackId }, { "type", nodetype::pluginInstrument }, { "plugin", pluginArgs ("Synth") } });
    const auto& track = *project.tracks[0];

    CHECK (track.graph.nodes.size() == 6);
    const auto order = chainFrom (track.graph, nodeOfType (track, nodetype::sequencer).id);
    REQUIRE (order.size() == 6);
    CHECK (order[1] == nodeOfType (track, nodetype::pluginInstrument).id);

    // The built-in synth can be put back the same way.
    const auto restored =
        apply (project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::mpeSynth } });
    CHECK (nodeOfType (*restored.tracks[0], nodetype::mpeSynth).id > 0);
    CHECK (restored.tracks[0]->graph.nodes.size() == 6);
}

TEST_CASE ("removing a chain member joins its neighbours", "[plugins][commands]")
{
    auto setup = projectWithClip();
    auto project =
        apply (setup.project, "insertNode",
               { { "trackId", setup.trackId }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("A") } });
    const auto effect = nodeOfType (*project.tracks[0], nodetype::pluginEffect).id;
    const auto synth = nodeOfType (*project.tracks[0], nodetype::mpeSynth).id;

    project = apply (project, "removeNode", { { "trackId", setup.trackId }, { "nodeId", effect } });
    const auto& track = *project.tracks[0];
    CHECK (chainFrom (track.graph, synth).size() == 5); // synth -> EQ -> compressor -> gain -> output

    // Without an instrument the track is silent but still a chain.
    project = apply (project, "removeNode", { { "trackId", setup.trackId }, { "nodeId", synth } });
    CHECK (project.tracks[0]->graph.nodes.size() == 5);
    CHECK (project.tracks[0]->graph.connections.size() == 3);

    project = apply (
        project, "insertNode",
        { { "trackId", setup.trackId }, { "type", nodetype::pluginInstrument }, { "plugin", pluginArgs ("S") } });
    CHECK (project.tracks[0]->graph.connections.size() == 5);
}

TEST_CASE ("effects can be reordered", "[plugins][commands]")
{
    auto setup = projectWithClip();
    auto project = setup.project;

    for (const auto* name : { "A", "B", "C" })
        project = apply (
            project, "insertNode",
            { { "trackId", setup.trackId }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs (name) } });

    const auto& before = project.tracks[0]->graph;
    const auto c = before.nodes.back().id;
    project = apply (project, "reorderNode", { { "trackId", setup.trackId }, { "nodeId", c }, { "index", 0 } });

    const auto& track = *project.tracks[0];
    const auto order = chainFrom (track.graph, nodeOfType (track, nodetype::mpeSynth).id);
    REQUIRE (order.size() == 8);
    CHECK (order[1] == c);

    const auto synth = nodeOfType (track, nodetype::mpeSynth).id;
    CHECK_THROWS_AS (
        apply (project, "reorderNode", { { "trackId", setup.trackId }, { "nodeId", synth }, { "index", 1 } }),
        CommandError);
}

TEST_CASE ("plugin nodes need a plugin reference", "[plugins][commands]")
{
    auto setup = projectWithClip();
    CHECK_THROWS_AS (
        apply (setup.project, "addNode", { { "trackId", setup.trackId }, { "type", nodetype::pluginEffect } }),
        CommandError);
    CHECK_THROWS_AS (
        apply (setup.project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::pluginEffect } }),
        CommandError);
}

TEST_CASE ("a Gain goes into a chain as an effect, before the chain's own Gain", "[plugins][commands]")
{
    auto setup = projectWithClip();
    const auto project =
        apply (setup.project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::gain } });
    const auto& track = *project.tracks[0];
    std::vector<juce::String> types;

    for (const auto id : chainFrom (track.graph, nodeOfType (track, nodetype::sequencer).id))
        types.push_back (track.graph.nodes[static_cast<size_t> (track.graph.indexOfNode (id))].type);

    CHECK (types
           == std::vector<juce::String> { nodetype::sequencer, nodetype::mpeSynth, nodetype::eq, nodetype::compressor,
                                          nodetype::gain, nodetype::gain, nodetype::trackOutput });

    // Both are in a simple chain still: the new one can be moved among the effects.
    const auto added = types.size() == 7 ? chainFrom (track.graph, nodeOfType (track, nodetype::sequencer).id)[4] : 0;
    const auto moved =
        apply (project, "reorderNode", { { "trackId", setup.trackId }, { "nodeId", added }, { "index", 0 } });
    const auto& after = *moved.tracks[0];
    CHECK (chainFrom (after.graph, nodeOfType (after, nodetype::sequencer).id)[2] == added);
}

TEST_CASE ("MIDI settings and bypass are stored and serialised", "[plugins][commands]")
{
    auto setup = projectWithClip();
    auto project = apply (
        setup.project, "insertNode",
        { { "trackId", setup.trackId }, { "type", nodetype::pluginInstrument }, { "plugin", pluginArgs ("S") } });
    const auto id = nodeOfType (*project.tracks[0], nodetype::pluginInstrument).id;

    project = apply (project, "setPluginMidi",
                     { { "trackId", setup.trackId }, { "nodeId", id }, { "mpe", false }, { "bendRange", 500 } });
    project =
        apply (project, "setNodeBypass", { { "trackId", setup.trackId }, { "nodeId", id }, { "bypassed", true } });

    const auto& node = nodeOfType (*project.tracks[0], nodetype::pluginInstrument);
    CHECK_FALSE (node.midi.mpe);
    CHECK (node.midi.bendRange == limits::maxBendRange);
    CHECK (node.bypassed);

    const auto json = toVar (*project.tracks[0])["graph"]["nodes"][juce::Identifier (juce::String (id))];
    CHECK (json["plugin"]["name"].toString() == "S");
    CHECK (static_cast<int> (json["midi"]["bendRange"]) == limits::maxBendRange);
    CHECK (static_cast<bool> (json["bypassed"]));

    const auto synth = nodeOfType (*setup.project.tracks[0], nodetype::mpeSynth).id;
    CHECK_THROWS_AS (
        apply (setup.project, "setPluginMidi", { { "trackId", setup.trackId }, { "nodeId", synth }, { "mpe", false } }),
        CommandError);
}

TEST_CASE ("a new plugin instrument gets plain MIDI, and each mode its own bend range", "[plugins][commands]")
{
    auto setup = projectWithClip();
    auto project = apply (
        setup.project, "insertNode",
        { { "trackId", setup.trackId }, { "type", nodetype::pluginInstrument }, { "plugin", pluginArgs ("S") } });
    const auto id = nodeOfType (*project.tracks[0], nodetype::pluginInstrument).id;
    const auto midi = [&] { return nodeOfType (*project.tracks[0], nodetype::pluginInstrument).midi; };

    CHECK_FALSE (midi().mpe);
    CHECK (midi().bendRange == 2);

    // Switching the mode moves to its default range; a range given with it wins; the same mode keeps the range.
    project = apply (project, "setPluginMidi", { { "trackId", setup.trackId }, { "nodeId", id }, { "mpe", true } });
    CHECK (midi().bendRange == 48);
    project = apply (project, "setPluginMidi",
                     { { "trackId", setup.trackId }, { "nodeId", id }, { "mpe", false }, { "bendRange", 12 } });
    CHECK (midi().bendRange == 12);
    project = apply (project, "setPluginMidi", { { "trackId", setup.trackId }, { "nodeId", id }, { "mpe", false } });
    CHECK (midi().bendRange == 12);
}

TEST_CASE ("plugin state is amended without an undo step", "[plugins][session]")
{
    auto setup = projectWithClip();
    Session session (setup.project);
    REQUIRE (session
                 .apply ("insertNode", makeObject ({ { "trackId", setup.trackId },
                                                     { "type", nodetype::pluginEffect },
                                                     { "plugin", pluginArgs ("A") } }))
                 .ok);

    const auto id = nodeOfType (*session.project()->tracks[0], nodetype::pluginEffect).id;
    const auto state = std::make_shared<const juce::MemoryBlock> ("abc", 3);
    const auto result =
        session.amend ([&] (const Project& p) { return withPluginState (p, setup.trackId, id, state); });

    REQUIRE (result.ok);
    CHECK (session.undoLabel() == "Insert plugin");
    CHECK (nodeOfType (*session.project()->tracks[0], nodetype::pluginEffect).pluginState == state);

    const auto synth = nodeOfType (*session.project()->tracks[0], nodetype::mpeSynth).id;
    CHECK_FALSE (
        session.amend ([&] (const Project& p) { return withPluginState (p, setup.trackId, synth, state); }).ok);
}

// ---------------------------------------------------------------------------------------------------------
// MPE encoder

namespace
{
struct EncoderRig
{
    explicit EncoderRig (std::initializer_list<juce::var> notes)
    {
        auto setup = projectWithClip();
        project = apply (setup.project, "addNotes", { { "clipId", setup.clipId }, { "notes", array (notes) } });
        sequence = std::make_unique<TrackSequence> (project.tracks[0]->clips);
        encoder.prepare (sampleRate);
        encoder.setSettings ({ true, MidiOutputSettings::mpeBendRange }); // tests that want plain MIDI set it
        transport.playing = true;
        transport.timeMap = TimeMap (constantTempo(), sampleRate);
    }

    // Renders one block starting at a tick, with the given events.
    std::vector<juce::MidiMessage> block (double startTick, std::initializer_list<NoteEvent> events)
    {
        EventBuffer buffer;

        for (const auto& event : events)
            std::ignore = buffer.add (event);

        transport.startTick = startTick;
        transport.endTick = startTick + blockSize * static_cast<double> (ticksPerBeat) / samplesPerBeat;

        ProcessContext context;
        context.numSamples = blockSize;
        context.eventsIn = &buffer;
        context.transport = &transport;
        context.sequence = sequence.get();

        juce::MidiBuffer midi;
        encoder.process (context, midi);

        std::vector<juce::MidiMessage> messages;
        for (const auto metadata : midi)
            messages.push_back (metadata.getMessage());

        return messages;
    }

    [[nodiscard]] NoteEvent on (size_t index, int offset = 0) const
    {
        const auto& note = sequence->notes()[index];
        return { offset, NoteEventType::noteOn, note.id, &note };
    }

    [[nodiscard]] NoteEvent legato (size_t index, size_t from, int offset = 0) const
    {
        const auto& note = sequence->notes()[index];
        NoteEvent event { offset, NoteEventType::legato, note.id, &note };
        event.fromNoteId = sequence->notes()[from].id;
        return event;
    }

    [[nodiscard]] NoteEvent off (size_t index, int offset = 0) const
    {
        return { offset, NoteEventType::noteOff, sequence->notes()[index].id, nullptr };
    }

    Project project;
    std::unique_ptr<TrackSequence> sequence;
    MpeEncoder encoder;
    Transport transport;
};

[[nodiscard]] std::vector<juce::MidiMessage> onlyNotes (const std::vector<juce::MidiMessage>& messages)
{
    std::vector<juce::MidiMessage> result;
    std::ranges::copy_if (messages, std::back_inserter (result),
                          [] (const juce::MidiMessage& m) { return m.isNoteOnOrOff(); });
    return result;
}

[[nodiscard]] const juce::MidiMessage* lastPitchWheel (const std::vector<juce::MidiMessage>& messages, int channel)
{
    const juce::MidiMessage* found = nullptr;

    for (const auto& m : messages)
        if (m.isPitchWheel() && m.getChannel() == channel)
            found = &m;

    return found;
}
} // namespace

TEST_CASE ("the encoder configures an MPE lower zone first", "[plugins][mpe]")
{
    EncoderRig rig ({ noteArgs (0, ticksPerBeat, 60) });
    const auto messages = rig.block (0.0, {});

    // RPN 6 (MPE configuration) = 15 member channels on channel 1.
    REQUIRE (messages.size() >= 3);
    CHECK (messages[0].isControllerOfType (101));
    CHECK (messages[1].getControllerValue() == 6);
    CHECK (messages[2].getControllerValue() == MpeEncoder::numMemberChannels);
    CHECK (messages[2].getChannel() == 1);

    // Every member channel gets the ±48 semitone bend range.
    const auto ranges = std::ranges::count_if (
        messages, [] (const juce::MidiMessage& m)
        { return m.isControllerOfType (6) && m.getChannel() > 1 && m.getControllerValue() == 48; });
    CHECK (ranges == MpeEncoder::numMemberChannels);

    // Only once.
    CHECK (rig.block (1000.0, {}).empty());
}

TEST_CASE ("each note gets its own channel with bend, pressure and timbre before the note-on", "[plugins][mpe]")
{
    EncoderRig rig ({ noteArgs (0, ticksPerBeat, 60.5), noteArgs (0, ticksPerBeat, 64) });
    std::ignore = rig.block (0.0, {});
    const auto messages = rig.block (0.0, { rig.on (0, 10), rig.on (1, 10) });
    const auto notes = onlyNotes (messages);

    REQUIRE (notes.size() == 2);
    CHECK (notes[0].getChannel() == 2);
    CHECK (notes[0].getNoteNumber() == 61); // 60.5 rounds up; the bend carries the rest
    CHECK_THAT (notes[0].getTimeStamp(), WithinAbs (10.0, 1.0e-9));
    CHECK (notes[1].getChannel() == 3);
    CHECK (notes[1].getNoteNumber() == 64);

    // The first messages on channel 2 set the expression, then the note starts.
    std::vector<juce::MidiMessage> channel2;
    std::ranges::copy_if (messages, std::back_inserter (channel2),
                          [] (const juce::MidiMessage& m) { return m.getChannel() == 2; });
    REQUIRE (channel2.size() == 4);
    CHECK (channel2[0].isPitchWheel());
    CHECK (channel2[0].getPitchWheelValue() == 8192 - 85); // -0.5 st of ±48 st: 8192 - 0.5 / 48 * 8192
    CHECK (channel2[1].isChannelPressure());
    CHECK (channel2[2].isControllerOfType (74));
    CHECK (channel2[2].getControllerValue() == 64); // timbre default 0.5
    CHECK (channel2[3].isNoteOn());

    // Note-offs go to the same channels; the next note reuses the least recently released channel.
    const auto offs = onlyNotes (rig.block (100.0, { rig.off (1), rig.off (0, 5) }));
    REQUIRE (offs.size() == 2);
    CHECK (offs[0].getChannel() == 3);
    CHECK (offs[1].getChannel() == 2);
    // Unused channels count as released longest ago.
    CHECK (onlyNotes (rig.block (200.0, { rig.on (0) }))[0].getChannel() == 4);
}

TEST_CASE ("pitch curves and vibrato become pitch bends", "[plugins][mpe]")
{
    auto note = noteArgs (0, 4 * ticksPerBeat, 60);
    note.getDynamicObject()->setProperty (
        "expression", makeObject ({ { "pitch", array ({ array ({ 0, 0 }), array ({ ticksPerBeat, 12 }) }) } }));
    EncoderRig rig ({ note });
    std::ignore = rig.block (0.0, {});
    std::ignore = rig.block (0.0, { rig.on (0) });

    // Half a beat in, the pitch curve is at +6 st.
    const auto messages = rig.block (0.5 * ticksPerBeat, {});
    const auto* bend = lastPitchWheel (messages, 2);
    REQUIRE (bend != nullptr);
    CHECK (std::abs (bend->getPitchWheelValue() - (8192 + 6.0 / 48 * 8192 + 21)) < 30); // within one block's motion

    // Nothing changes on a held flat curve: no messages at all.
    std::ignore = rig.block (2.0 * ticksPerBeat, {});
    CHECK (rig.block (2.0 * ticksPerBeat + 10, {}).empty());
}

TEST_CASE ("a linked note stays on its MPE channel and bends there", "[plugins][mpe][links]")
{
    EncoderRig rig ({ noteArgs (0, ticksPerBeat, 60), noteArgs (ticksPerBeat, ticksPerBeat, 72) });
    const auto channel = onlyNotes (rig.block (0.0, { rig.on (0) }))[0].getChannel();
    const auto messages = rig.block (static_cast<double> (ticksPerBeat), { rig.legato (1, 0) });

    CHECK (onlyNotes (messages).empty()); // no retrigger, no note-off

    const auto bend = std::ranges::find_if (messages, [] (const juce::MidiMessage& m) { return m.isPitchWheel(); });
    REQUIRE (bend != messages.end());
    CHECK (bend->getChannel() == channel);
    CHECK (bend->getPitchWheelValue() == 8192 + 8192 * 12 / MidiOutputSettings::mpeBendRange);

    // The later note's note-off ends the note the channel started with.
    const auto ended = onlyNotes (rig.block (2.0 * ticksPerBeat, { rig.off (1) }));
    REQUIRE (ended.size() == 1);
    CHECK (ended[0].isNoteOff());
    CHECK (ended[0].getChannel() == channel);
    CHECK (ended[0].getNoteNumber() == 60);
}

TEST_CASE ("a linked note past the bend range starts on a channel of its own", "[plugins][mpe][links]")
{
    EncoderRig rig ({ noteArgs (0, ticksPerBeat, 40), noteArgs (ticksPerBeat, ticksPerBeat, 100) });
    const auto channel = onlyNotes (rig.block (0.0, { rig.on (0) }))[0].getChannel();
    const auto notes = onlyNotes (rig.block (static_cast<double> (ticksPerBeat), { rig.legato (1, 0) }));

    REQUIRE (notes.size() == 2);
    CHECK (notes[0].isNoteOn());
    CHECK (notes[0].getNoteNumber() == 100);
    CHECK (notes[0].getChannel() != channel);
    CHECK (notes[1].isNoteOff());
    CHECK (notes[1].getNoteNumber() == 40);
    CHECK (notes[1].getChannel() == channel);
}

TEST_CASE ("in MIDI mode a linked note overlaps the note before it", "[plugins][mpe][links]")
{
    EncoderRig rig ({ noteArgs (0, ticksPerBeat, 60), noteArgs (ticksPerBeat, ticksPerBeat, 62),
                      noteArgs (2 * ticksPerBeat, ticksPerBeat, 62) });
    rig.encoder.setSettings ({ false, 48 });
    std::ignore = rig.block (0.0, { rig.on (0) });
    const auto notes = onlyNotes (rig.block (static_cast<double> (ticksPerBeat), { rig.legato (1, 0) }));

    REQUIRE (notes.size() == 2);
    CHECK (notes[0].isNoteOn());
    CHECK (notes[0].getNoteNumber() == 62);
    CHECK (notes[1].isNoteOff());
    CHECK (notes[1].getNoteNumber() == 60);

    // The same key just carries on: a note-on and note-off would cut it.
    CHECK (onlyNotes (rig.block (2.0 * ticksPerBeat, { rig.legato (2, 1) })).empty());
    const auto ended = onlyNotes (rig.block (3.0 * ticksPerBeat, { rig.off (2) }));
    REQUIRE (ended.size() == 1);
    CHECK (ended[0].getNoteNumber() == 62);
}

TEST_CASE ("a linked note with no note to continue starts as usual", "[plugins][mpe][links]")
{
    EncoderRig rig ({ noteArgs (0, ticksPerBeat, 60), noteArgs (ticksPerBeat, ticksPerBeat, 72) });
    const auto notes = onlyNotes (rig.block (static_cast<double> (ticksPerBeat), { rig.legato (1, 0) }));
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].isNoteOn());
    CHECK (notes[0].getNoteNumber() == 72);
}

TEST_CASE ("MIDI mode sends everything on channel 1 without bends", "[plugins][mpe]")
{
    EncoderRig rig ({ noteArgs (0, ticksPerBeat, 60.5) });
    rig.encoder.setSettings ({ false, 48 });
    const auto messages = rig.block (0.0, { rig.on (0) });

    CHECK (std::ranges::none_of (messages,
                                 [] (const juce::MidiMessage& m) { return m.isPitchWheel() || m.isController(); }));
    const auto notes = onlyNotes (messages);
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].getChannel() == 1);
}

TEST_CASE ("MIDI mode bends live pitch over the node's bend range", "[plugins][mpe]")
{
    for (const auto range : { 2, 12 })
    {
        INFO ("range " << range);
        EncoderRig rig ({ noteArgs (0, ticksPerBeat, 60) });
        rig.encoder.setSettings ({ false, range });
        std::ignore = rig.block (0.0, { rig.on (0) });

        // One semitone up on the keyboard's wheel.
        const auto noteId = rig.sequence->notes()[0].id;
        const auto messages = rig.block (
            100.0, { { 0, NoteEventType::expression, noteId, nullptr, static_cast<int> (Lane::pitch), 1.0f } });
        const auto* bend = lastPitchWheel (messages, 1);
        REQUIRE (bend != nullptr);
        CHECK (bend->getPitchWheelValue() == 8192 + static_cast<int> (std::lround (8192.0 / range)));
    }
}

TEST_CASE ("chokeAll ends notes and cuts tails", "[plugins][mpe]")
{
    EncoderRig rig ({ noteArgs (0, ticksPerBeat, 60) });
    std::ignore = rig.block (0.0, { rig.on (0) });
    const auto messages = rig.block (10.0, { { 0, NoteEventType::chokeAll, 0, nullptr } });

    CHECK (onlyNotes (messages).size() == 1);
    CHECK (std::ranges::count_if (messages, [] (const juce::MidiMessage& m) { return m.isAllSoundOff(); })
           == MpeEncoder::numMemberChannels);
}

// ---------------------------------------------------------------------------------------------------------
// Plugin nodes in the engine

TEST_CASE ("an effect plugin processes the track's audio", "[plugins][engine]")
{
    PluginRig rig;
    rig.edit ("addNotes", { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (0, 4 * ticksPerBeat, 69) }) } });
    rig.edit ("insertNode",
              { { "trackId", rig.trackId }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("Half") } });
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));

    auto& node = rig.pluginNode (nodetype::pluginEffect);
    CHECK (node.status() == PluginNode::Status::loading);

    rig.engine.play();
    const auto loading = render (rig.engine, 4096);

    // Still loading: audio passes through unchanged.
    REQUIRE (firstAudible (loading.left()) >= 0);

    node.attach (std::make_unique<FakeProcessor> (false, 0.0f));
    CHECK (node.status() == PluginNode::Status::ready);
    CHECK (firstAudible (render (rig.engine, 4096).left()) == -1); // gain 0: silent

    const auto effectId = nodeOfType (*rig.project.tracks[0], nodetype::pluginEffect).id;
    rig.edit ("setNodeBypass", { { "trackId", rig.trackId }, { "nodeId", effectId }, { "bypassed", true } });
    CHECK (firstAudible (render (rig.engine, 4096).left()) >= 0);
}

TEST_CASE ("an instrument plugin receives the track's notes", "[plugins][engine]")
{
    PluginRig rig;
    rig.edit ("addNotes",
              { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (ticksPerBeat, ticksPerBeat, 60) }) } });
    rig.edit (
        "insertNode",
        { { "trackId", rig.trackId }, { "type", nodetype::pluginInstrument }, { "plugin", pluginArgs ("Synth") } });
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));

    auto processor = std::make_unique<FakeProcessor> (true, 0.25f);
    auto* fake = processor.get();
    rig.pluginNode (nodetype::pluginInstrument).attach (std::move (processor));

    rig.engine.play();
    const auto output = render (rig.engine, 3 * samplesPerBeat);

    CHECK (firstAudible (output.left()) == samplesPerBeat);
    CHECK_THAT (output.left()[static_cast<size_t> (samplesPerBeat + 100)], WithinAbs (0.25, 1.0e-6));
    CHECK (fake->held == 0);
    CHECK (std::ranges::count_if (fake->received, [] (const juce::MidiMessage& m) { return m.isNoteOn(); }) == 1);
}

TEST_CASE ("removed plugins come back with their instance on undo", "[plugins][engine]")
{
    PluginRig rig;
    rig.edit ("insertNode",
              { { "trackId", rig.trackId }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("A") } });
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));

    auto& node = rig.pluginNode (nodetype::pluginEffect);
    node.attach (std::make_unique<FakeProcessor> (false, 1.0f));
    const auto withPlugin = rig.project;
    const auto effectId = nodeOfType (*withPlugin.tracks[0], nodetype::pluginEffect).id;

    rig.edit ("removeNode", { { "trackId", rig.trackId }, { "nodeId", effectId } });
    CHECK (rig.compiler.findInstance (rig.trackId, effectId) == nullptr);

    rig.project = withPlugin;
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));
    CHECK (&rig.pluginNode (nodetype::pluginEffect) == &node);
    CHECK (node.status() == PluginNode::Status::ready);
}

TEST_CASE ("plugins survive a device change and are prepared again", "[plugins][engine]")
{
    PluginRig rig;
    rig.edit ("insertNode",
              { { "trackId", rig.trackId }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("A") } });
    std::ignore = rig.compiler.compile (rig.project, spec);

    auto processor = std::make_unique<FakeProcessor> (false, 1.0f);
    auto* fake = processor.get();
    auto& node = rig.pluginNode (nodetype::pluginEffect);
    node.attach (std::move (processor));
    const auto prepared = fake->prepareCount;

    std::ignore = rig.compiler.compile (rig.project, { 44100.0, 256 });
    CHECK (&rig.pluginNode (nodetype::pluginEffect) == &node);
    CHECK (fake->prepareCount == prepared + 1);
}

TEST_CASE ("tracks are aligned to the slowest track's latency", "[plugins][engine]")
{
    constexpr int latency = 300;

    PluginRig rig;
    rig.edit ("addNotes",
              { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (ticksPerBeat, ticksPerBeat, 69) }) } });

    // A second track with a latent (but otherwise neutral) effect and no notes.
    rig.edit ("addTrack", {});
    const auto slowTrack = rig.project.tracks[1]->id;
    rig.edit ("insertNode",
              { { "trackId", slowTrack }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("Lookahead") } });
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));

    const auto effectId = nodeOfType (*rig.project.tracks[1], nodetype::pluginEffect).id;
    auto* node = dynamic_cast<PluginNode*> (rig.compiler.findInstance (slowTrack, effectId));
    REQUIRE (node != nullptr);
    node->attach (std::make_unique<FakeProcessor> (false, 1.0f, latency));

    const auto engine = rig.compiler.compile (rig.project, spec);
    CHECK (engine->latencySamples == latency);
    rig.engine.setEngine (engine);

    // The fast track is delayed by the latency but runs that far ahead: its note still starts on the beat.
    rig.engine.play();
    const auto output = render (rig.engine, 2 * samplesPerBeat);
    CHECK (firstAudible (output.left()) == samplesPerBeat + 1); // the synth's first sample is sin(0) = 0
}

TEST_CASE ("latency is aligned at a group's bus and through the group's chain", "[plugins][engine][groups]")
{
    constexpr int latency = 300;

    // A track with a note at the top level, and a silent track inside a group whose chain has a latent effect.
    PluginRig rig;
    rig.edit ("addNotes",
              { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (ticksPerBeat, ticksPerBeat, 69) }) } });
    rig.edit ("addTrack", {});
    const auto member = rig.project.tracks[1]->id;
    rig.edit ("groupTracks", { { "ids", array ({ member }) } });
    const auto group = rig.project.tracks[1]->id;
    REQUIRE (rig.project.tracks[1]->kind == TrackKind::group);

    rig.edit ("insertNode",
              { { "trackId", group }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("Lookahead") } });
    std::ignore = rig.compiler.compile (rig.project, spec);
    const auto effectId = nodeOfType (*rig.project.tracks[1], nodetype::pluginEffect).id;
    auto* node = dynamic_cast<PluginNode*> (rig.compiler.findInstance (group, effectId));
    REQUIRE (node != nullptr);
    node->attach (std::make_unique<FakeProcessor> (false, 1.0f, latency));

    SECTION ("a track outside the group is delayed to the group's latency and still starts on the beat")
    {
        const auto engine = rig.compiler.compile (rig.project, spec);
        CHECK (engine->latencySamples == latency);
        rig.engine.setEngine (engine);
        rig.engine.play();
        CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == samplesPerBeat + 1);
    }

    SECTION ("a track inside the group runs ahead by the group's latency")
    {
        // The fake reports the latency without delaying, so the note comes out that much early (a real lookahead
        // effect would put it back on the beat).
        rig.edit ("moveTracks", { { "ids", array ({ rig.project.tracks[0]->id }) }, { "parent", group } });
        const auto engine = rig.compiler.compile (rig.project, spec);
        CHECK (engine->latencySamples == latency);
        rig.engine.setEngine (engine);
        rig.engine.play();
        CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == samplesPerBeat + 1 - latency);
    }
}

TEST_CASE ("sends reach their return in step with its latest input", "[plugins][engine][sends]")
{
    constexpr int latency = 300;

    // Track B plays a note, heard only through a pre-fader send to return R; track A has a latent effect and sends to
    // R too, so R's input arrives `latency` samples late and B's send must be delayed to match.
    PluginRig rig;
    rig.edit ("addNotes",
              { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (ticksPerBeat, ticksPerBeat, 69) }) } });
    const auto b = rig.trackId;
    rig.edit ("addTrack", {});
    const auto a = rig.project.tracks[1]->id;
    rig.edit ("addTrack", { { "kind", "return" } });
    const auto r = rig.project.tracks[2]->id;
    rig.edit ("insertNode",
              { { "trackId", a }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("Lookahead") } });
    std::ignore = rig.compiler.compile (rig.project, spec);
    const auto effectId = nodeOfType (*rig.project.tracks[1], nodetype::pluginEffect).id;
    auto* node = dynamic_cast<PluginNode*> (rig.compiler.findInstance (a, effectId));
    REQUIRE (node != nullptr);
    node->attach (std::make_unique<FakeProcessor> (false, 1.0f, latency));

    rig.edit ("addSend", { { "trackId", a }, { "target", r } });
    rig.edit ("addSend", { { "trackId", b }, { "target", r }, { "preFader", true } });
    rig.edit ("setTrackGain", { { "id", b }, { "gainDb", -96.0 } });

    const auto engine = rig.compiler.compile (rig.project, spec);
    CHECK (engine->latencySamples == latency);
    rig.engine.setEngine (engine);
    rig.engine.play();
    CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == samplesPerBeat + 1);
}

TEST_CASE ("track offsets delay or pull a track against the playhead", "[plugins][engine][offset]")
{
    constexpr int offset = 200;

    const auto note = [&] (PluginRig& rig)
    {
        rig.edit ("addNotes",
                  { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (ticksPerBeat, ticksPerBeat, 69) }) } });
    };

    SECTION ("a positive offset delays the track and adds no latency")
    {
        PluginRig rig;
        note (rig);
        rig.edit ("setTrackOffset", { { "id", rig.trackId }, { "offsetMs", offsetMsForSamples (offset) } });

        const auto engine = rig.compiler.compile (rig.project, spec);
        CHECK (engine->latencySamples == 0);
        rig.engine.setEngine (engine);
        rig.engine.play();
        CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == samplesPerBeat + 1 + offset);
    }

    SECTION ("a negative offset pulls the track earlier and reports the advance as latency")
    {
        PluginRig rig;
        note (rig);
        rig.edit ("setTrackOffset", { { "id", rig.trackId }, { "offsetMs", -offsetMsForSamples (offset) } });

        const auto engine = rig.compiler.compile (rig.project, spec);
        CHECK (engine->latencySamples == offset);
        rig.engine.setEngine (engine);
        rig.engine.play();
        CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == samplesPerBeat + 1 - offset);
    }
}

TEST_CASE ("a group's offset moves every track inside it", "[plugins][engine][groups][offset]")
{
    constexpr int offset = 200;

    const auto groupOf = [] (PluginRig& rig)
    {
        rig.edit ("addNotes",
                  { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (ticksPerBeat, ticksPerBeat, 69) }) } });
        rig.edit ("groupTracks", { { "ids", array ({ rig.trackId }) } });

        const auto group = std::ranges::find_if (rig.project.tracks, [] (const TrackPtr& track)
                                                 { return track->kind == TrackKind::group; });
        REQUIRE (group != rig.project.tracks.end());
        return (*group)->id;
    };

    SECTION ("positive")
    {
        PluginRig rig;
        const auto group = groupOf (rig);
        rig.edit ("setTrackOffset", { { "id", group }, { "offsetMs", offsetMsForSamples (offset) } });

        const auto engine = rig.compiler.compile (rig.project, spec);
        CHECK (engine->latencySamples == 0);
        rig.engine.setEngine (engine);
        rig.engine.play();
        CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == samplesPerBeat + 1 + offset);
    }

    SECTION ("negative")
    {
        PluginRig rig;
        const auto group = groupOf (rig);
        rig.edit ("setTrackOffset", { { "id", group }, { "offsetMs", -offsetMsForSamples (offset) } });

        const auto engine = rig.compiler.compile (rig.project, spec);
        CHECK (engine->latencySamples == offset);
        rig.engine.setEngine (engine);
        rig.engine.play();
        CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == samplesPerBeat + 1 - offset);
    }
}

TEST_CASE ("send delay compensation follows the sender's offset", "[plugins][engine][sends][offset]")
{
    constexpr int latency = 300;
    constexpr int offset = 200;

    const auto addSendRig = [&] (PluginRig& rig)
    {
        rig.edit ("addNotes",
                  { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (ticksPerBeat, ticksPerBeat, 69) }) } });
        const auto b = rig.trackId;
        rig.edit ("addTrack", {});
        const auto a = rig.project.tracks[1]->id;
        rig.edit ("addTrack", { { "kind", "return" } });
        const auto r = rig.project.tracks[2]->id;
        rig.edit ("insertNode",
                  { { "trackId", a }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("Lookahead") } });
        std::ignore = rig.compiler.compile (rig.project, spec);

        const auto effectId = nodeOfType (*rig.project.tracks[1], nodetype::pluginEffect).id;
        auto* node = dynamic_cast<PluginNode*> (rig.compiler.findInstance (a, effectId));
        REQUIRE (node != nullptr);
        node->attach (std::make_unique<FakeProcessor> (false, 1.0f, latency));

        rig.edit ("addSend", { { "trackId", a }, { "target", r } });
        rig.edit ("addSend", { { "trackId", b }, { "target", r }, { "preFader", true } });
        rig.edit ("setTrackGain", { { "id", b }, { "gainDb", -96.0 } });
        return b;
    };

    SECTION ("positive")
    {
        PluginRig rig;
        const auto b = addSendRig (rig);
        rig.edit ("setTrackOffset", { { "id", b }, { "offsetMs", offsetMsForSamples (offset) } });

        const auto engine = rig.compiler.compile (rig.project, spec);
        CHECK (engine->latencySamples == latency);
        rig.engine.setEngine (engine);
        rig.engine.play();
        CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == samplesPerBeat + 1 + offset);
    }

    SECTION ("negative")
    {
        PluginRig rig;
        const auto b = addSendRig (rig);
        rig.edit ("setTrackOffset", { { "id", b }, { "offsetMs", -offsetMsForSamples (offset) } });

        const auto engine = rig.compiler.compile (rig.project, spec);
        CHECK (engine->latencySamples == latency);
        rig.engine.setEngine (engine);
        rig.engine.play();
        CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == samplesPerBeat + 1 - offset);
    }
}

TEST_CASE ("a key send follows its source's offset", "[plugins][engine][sidechain][offset]")
{
    constexpr int latency = 300;
    constexpr int offset = 200;

    PluginRig rig;
    rig.edit ("addTrack", {});
    const auto kick = rig.project.tracks[1]->id;
    const auto compressor = nodeOfType (*rig.project.tracks[0], nodetype::compressor).id;

    rig.edit ("insertNode",
              { { "trackId", kick }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs ("Lookahead") } });
    std::ignore = rig.compiler.compile (rig.project, spec);

    const auto effectId = nodeOfType (*rig.project.tracks[1], nodetype::pluginEffect).id;
    auto* plugin = dynamic_cast<PluginNode*> (rig.compiler.findInstance (kick, effectId));
    REQUIRE (plugin != nullptr);
    plugin->attach (std::make_unique<FakeProcessor> (false, 1.0f, latency));

    rig.edit ("addSend", { { "trackId", kick }, { "target", rig.trackId }, { "targetNode", compressor } });

    const auto keyLate = [&]
    {
        const auto engine = rig.compiler.compile (rig.project, spec);
        rig.engine.setEngine (engine);
        auto* node = rig.compiler.findInstance (rig.trackId, compressor);
        REQUIRE (node != nullptr);
        return static_cast<double> (node->meters()["keyLateMs"]);
    };

    CHECK_THAT (keyLate(), WithinAbs (1000.0 * latency / sampleRate, 1e-9));

    rig.edit ("setTrackOffset", { { "id", kick }, { "offsetMs", offsetMsForSamples (offset) } });
    CHECK_THAT (keyLate(), WithinAbs (1000.0 * (latency - offset) / sampleRate, 1e-9));

    rig.edit ("setTrackOffset", { { "id", kick }, { "offsetMs", -offsetMsForSamples (offset) } });
    CHECK_THAT (keyLate(), WithinAbs (1000.0 * (latency + offset) / sampleRate, 1e-9));
}

TEST_CASE ("master effects process the mix, and their latency moves the tracks ahead", "[plugins][engine][mixing]")
{
    constexpr int latency = 300;

    PluginRig rig;
    rig.edit ("addNotes",
              { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (ticksPerBeat, ticksPerBeat, 69) }) } });
    rig.project = withMaster (rig.project);
    const auto masterId = rig.project.master->id;

    const auto insert = [&] (const char* name, float gain, int delay)
    {
        rig.edit ("insertNode",
                  { { "trackId", masterId }, { "type", nodetype::pluginEffect }, { "plugin", pluginArgs (name) } });
        std::ignore = rig.compiler.compile (rig.project, spec);

        const auto effectId = nodeOfType (*rig.project.master, nodetype::pluginEffect).id;
        auto* node = dynamic_cast<PluginNode*> (rig.compiler.findInstance (masterId, effectId));
        REQUIRE (node != nullptr);
        node->attach (std::make_unique<FakeProcessor> (false, gain, delay));
        return effectId;
    };

    const auto effectId = insert ("Lookahead", 1.0f, latency);
    const auto engine = rig.compiler.compile (rig.project, spec);
    CHECK (engine->masterLatencySamples == latency);
    CHECK (engine->latencySamples == latency);
    rig.engine.setEngine (engine);

    // The tracks run ahead by the reported latency. The fake reports it without delaying, so the note comes out
    // that much early; a real lookahead effect would put it back on the beat.
    rig.engine.play();
    CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == samplesPerBeat + 1 - latency);

    // An effect that silences the mix silences everything.
    rig.edit ("removeNode", { { "trackId", masterId }, { "nodeId", effectId } });
    std::ignore = insert ("Mute", 0.0f, 0);
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));
    CHECK (firstAudible (render (rig.engine, 2 * samplesPerBeat).left()) == -1);
}

TEST_CASE ("live expression, sustain and controllers reach instrument plugins", "[plugins][midi]")
{
    PluginRig rig;
    rig.edit (
        "insertNode",
        { { "trackId", rig.trackId }, { "type", nodetype::pluginInstrument }, { "plugin", pluginArgs ("Synth") } });
    rig.edit ("setPluginMidi", { { "trackId", rig.trackId },
                                 { "nodeId", nodeOfType (*rig.project.tracks[0], nodetype::pluginInstrument).id },
                                 { "mpe", true } });
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));

    auto processor = std::make_unique<FakeProcessor> (true, 0.25f);
    auto* fake = processor.get();
    rig.pluginNode (nodetype::pluginInstrument).attach (std::move (processor));

    LiveInput on;
    on.trackId = rig.trackId;
    on.noteId = rig.engine.nextLiveNoteId();
    on.pitch = 60.0;
    on.initial = { 0.0f, 0.5f, 0.5f };
    rig.engine.sendLiveInput (on);
    std::ignore = render (rig.engine, 512);

    LiveInput bend = on;
    bend.type = NoteEventType::expression;
    bend.lane = static_cast<int> (Lane::pitch);
    bend.value = 4.8f; // a tenth of the ±48 range
    rig.engine.sendLiveInput (bend);

    LiveInput pedal;
    pedal.trackId = rig.trackId;
    pedal.type = NoteEventType::sustain;
    pedal.value = 1.0f;
    rig.engine.sendLiveInput (pedal);

    LiveInput wheel;
    wheel.trackId = rig.trackId;
    wheel.type = NoteEventType::controller;
    wheel.lane = 1;
    wheel.value = 1.0f;
    rig.engine.sendLiveInput (wheel);
    std::ignore = render (rig.engine, 512);

    const auto has = [&] (auto predicate) { return std::ranges::any_of (fake->received, predicate); };
    CHECK (std::ranges::count_if (fake->received, [] (const juce::MidiMessage& m) { return m.isNoteOn(); }) == 1);
    CHECK (has (
        [] (const juce::MidiMessage& m)
        { return m.isPitchWheel() && m.getChannel() > 1 && std::abs (m.getPitchWheelValue() - (8192 + 819)) <= 1; }));
    CHECK (has ([] (const juce::MidiMessage& m)
                { return m.isController() && m.getControllerNumber() == 64 && m.getControllerValue() == 127; }));
    CHECK (has ([] (const juce::MidiMessage& m)
                { return m.isController() && m.getControllerNumber() == 1 && m.getControllerValue() == 127; }));
}

TEST_CASE ("hosted plugins see the tempo, time signature and position", "[plugin][tempo]")
{
    // 8 beats at 120 (4 s), then 60 BPM; 7/8 from bar 2 (beat 8).
    const TempoMap map ({ { 0, 120.0 }, { 8 * ticksPerBeat, 60.0 } }, { { 0, 4, 4 }, { 2, 7, 8 } });
    Transport transport;
    transport.playing = true;
    transport.timeMap = TimeMap (map, sampleRate);
    transport.startTick = 9.0 * ticksPerBeat;

    const auto info = positionInfo (transport);
    CHECK_THAT (*info.getBpm(), WithinAbs (60.0, 1e-9));
    CHECK (info.getTimeSignature()->numerator == 7);
    CHECK (info.getTimeSignature()->denominator == 8);
    CHECK_THAT (*info.getPpqPosition(), WithinAbs (9.0, 1e-9));
    CHECK_THAT (*info.getPpqPositionOfLastBarStart(), WithinAbs (8.0, 1e-9));
    CHECK_THAT (*info.getTimeInSeconds(), WithinAbs (5.0, 1e-9));
    CHECK (*info.getTimeInSamples() == 240000);
    CHECK (info.getIsPlaying());
}
