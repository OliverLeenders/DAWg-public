#include "TestUtils.h"
#include "dawg/MidiInput.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Phase 9: MIDI input decoding, live expression and note recording (docs/design/phase9-export-midi-input.md).

namespace
{
// A decoder that collects its events, with note ids counting down from -1.
struct Decoder
{
    explicit Decoder (bool mpe)
        : decoder ([this] { return --nextId; }, MidiInputSettings { mpe })
    {
    }

    void send (const juce::MidiMessage& message) { changed = decoder.process (message, 0.0, sink) || changed; }

    ObjectId nextId = 0;
    std::vector<LiveInput> events;
    bool changed = false;
    MidiInputDecoder::Sink sink = [this] (const LiveInput& e) { events.push_back (e); };
    MidiInputDecoder decoder;
};

[[nodiscard]] juce::MidiMessage bendSemitones (int channel, double semitones, double range)
{
    return juce::MidiMessage::pitchWheel (channel,
                                          static_cast<int> (std::lround (8192.0 + semitones / range * 8192.0)));
}

// Zero crossings per second of a rendered buffer, from a given sample.
[[nodiscard]] double frequencyOf (const std::vector<float>& samples, size_t from)
{
    return zeroCrossings (samples, from, samples.size()) * sampleRate / 2.0
           / static_cast<double> (samples.size() - from);
}
} // namespace

// ---------------------------------------------------------------------------------------------------------
// Decoder

TEST_CASE ("MPE member channels give each note its own expression", "[midi]")
{
    Decoder d (true);
    d.send (bendSemitones (2, 1.0, 48.0)); // before the note: becomes its initial pitch
    d.send (juce::MidiMessage::noteOn (2, 60, 0.5f));
    d.send (juce::MidiMessage::noteOn (3, 64, 0.5f));
    REQUIRE (d.events.size() == 2);
    CHECK_THAT (d.events[0].initial[0], WithinAbs (1.0, 0.01));
    CHECK_THAT (d.events[1].initial[0], WithinAbs (0.0, 1e-6));

    d.events.clear();
    d.send (juce::MidiMessage::channelPressureChange (3, 127));
    d.send (juce::MidiMessage::controllerEvent (3, 74, 0));
    REQUIRE (d.events.size() == 2);
    CHECK (d.events[0].noteId == -2);
    CHECK (d.events[0].lane == static_cast<int> (Lane::pressure));
    CHECK_THAT (d.events[0].value, WithinAbs (1.0, 1e-6));
    CHECK (d.events[1].lane == static_cast<int> (Lane::timbre));

    // A master-channel bend moves every note (±2 semitones on the master channel).
    d.events.clear();
    d.send (bendSemitones (1, 1.0, 2.0));
    REQUIRE (d.events.size() == 2);
    CHECK_THAT (d.events[0].value, WithinAbs (2.0, 0.01)); // 1 (member) + 1 (master)
    CHECK_THAT (d.events[1].value, WithinAbs (1.0, 0.01));
}

TEST_CASE ("plain MIDI applies bend and pressure to the notes held on the channel", "[midi]")
{
    Decoder d (false);
    d.send (juce::MidiMessage::noteOn (1, 60, 0.5f));
    d.send (juce::MidiMessage::noteOn (1, 67, 0.5f));
    d.events.clear();

    d.send (bendSemitones (1, -2.0, 2.0));
    REQUIRE (d.events.size() == 2);
    CHECK (d.events[0].type == NoteEventType::expression);
    CHECK_THAT (d.events[0].value, WithinAbs (-2.0, 0.01));

    d.events.clear();
    d.send (juce::MidiMessage::controllerEvent (1, 1, 127)); // mod wheel: for plugins
    d.send (juce::MidiMessage::controllerEvent (1, 64, 127));
    d.send (juce::MidiMessage::controllerEvent (1, 64, 100)); // still down: nothing new
    REQUIRE (d.events.size() == 2);
    CHECK (d.events[0].type == NoteEventType::controller);
    CHECK (d.events[0].lane == 1);
    CHECK (d.events[1].type == NoteEventType::sustain);

    d.events.clear();
    d.send (juce::MidiMessage::noteOn (1, 60, 0.0f)); // velocity 0 is a note-off
    REQUIRE (d.events.size() == 1);
    CHECK (d.events[0].type == NoteEventType::noteOff);

    d.events.clear();
    d.decoder.releaseAll (0.0, d.sink);
    REQUIRE (d.events.size() == 2); // the other note and the pedal
    CHECK (d.events[0].noteId == -2);
    CHECK (d.events[1].type == NoteEventType::sustain);
}

TEST_CASE ("an MPE configuration message switches the decoder to MPE", "[midi]")
{
    Decoder d (false);

    for (const auto& message : juce::MidiRPNGenerator::generate (1, 6, 15, false, false))
        d.send (message.getMessage());

    CHECK (d.changed);
    CHECK (d.decoder.settings().mpe);

    for (const auto& message : juce::MidiRPNGenerator::generate (2, 0, 24, false, false))
        d.send (message.getMessage());

    CHECK (d.decoder.settings().memberBendRange == 24);
}

// ---------------------------------------------------------------------------------------------------------
// Live expression in the engine

TEST_CASE ("live expression bends a built-in synth note and the pedal sustains it", "[midi][engine]")
{
    auto setup = projectWithClip();
    GraphCompiler compiler;
    AudioEngine engine;
    engine.setEngine (compiler.compile (setup.project, spec));

    LiveInput on;
    on.trackId = setup.trackId;
    on.noteId = engine.nextLiveNoteId();
    on.pitch = 69.0;
    engine.sendLiveInput (on);
    const auto plain = render (engine, 8192);
    CHECK_THAT (frequencyOf (plain.left(), 2048), WithinAbs (440.0, 15.0));

    LiveInput bend;
    bend.trackId = setup.trackId;
    bend.noteId = on.noteId;
    bend.type = NoteEventType::expression;
    bend.lane = static_cast<int> (Lane::pitch);
    bend.value = 12.0f;
    engine.sendLiveInput (bend);
    const auto octave = render (engine, 8192);
    CHECK_THAT (frequencyOf (octave.left(), 2048), WithinAbs (880.0, 25.0));

    LiveInput pedal;
    pedal.trackId = setup.trackId;
    pedal.type = NoteEventType::sustain;
    pedal.value = 1.0f;
    engine.sendLiveInput (pedal);

    LiveInput off;
    off.trackId = setup.trackId;
    off.noteId = on.noteId;
    off.type = NoteEventType::noteOff;
    engine.sendLiveInput (off);
    std::ignore = render (engine, 48000);
    CHECK (firstAudible (render (engine, 1024).left()) >= 0); // held by the pedal

    pedal.value = 0.0f;
    engine.sendLiveInput (pedal);
    std::ignore = render (engine, 96000);
    CHECK (firstAudible (render (engine, 1024).left()) == -1);
}

// ---------------------------------------------------------------------------------------------------------
// Recording

TEST_CASE ("live events are recorded with their timeline positions while recording", "[midi][record]")
{
    auto setup = projectWithClip();
    GraphCompiler compiler;
    AudioEngine engine;
    engine.setEngine (compiler.compile (setup.project, spec));

    LiveInput on;
    on.trackId = setup.trackId;
    on.noteId = engine.nextLiveNoteId();
    engine.sendLiveInput (on);
    std::ignore = render (engine, 512);
    CHECK_FALSE (engine.popRecordedInput().has_value()); // not recording

    engine.setInputLatency (256);
    engine.startRecording ({}, 0.0);
    engine.play();
    std::ignore = render (engine, 24000); // one beat at 120 BPM
    on.noteId = engine.nextLiveNoteId();
    engine.sendLiveInput (on);
    std::ignore = render (engine, 512);

    const auto recorded = engine.popRecordedInput().value_or (RecordedInput {});
    CHECK (recorded.input.noteId == on.noteId);
    CHECK_THAT (recorded.tick, WithinAbs (ticksPerBeat * (24000.0 - 256.0) / 24000.0, 1.0));
    engine.stopRecording();
}

TEST_CASE ("the recorder builds notes with simplified curves", "[midi][record]")
{
    NoteRecorder recorder;
    LiveInput on;
    on.noteId = -1;
    on.pitch = 60.0;
    on.velocity = 0.7f;
    recorder.add ({ on, 1000.0 });

    // A straight pitch ramp: simplified to its ends.
    for (int i = 1; i <= 20; ++i)
    {
        LiveInput e;
        e.noteId = -1;
        e.type = NoteEventType::expression;
        e.lane = static_cast<int> (Lane::pitch);
        e.value = static_cast<float> (i) * 0.1f;
        recorder.add ({ e, 1000.0 + i * 100.0 });
    }

    LiveInput off;
    off.noteId = -1;
    off.type = NoteEventType::noteOff;
    off.velocity = 0.25f;
    recorder.add ({ off, 4000.0 });

    // A second note, still held when recording stops.
    on.noteId = -2;
    on.pitch = 64.0;
    recorder.add ({ on, 5000.0 });

    const auto notes = recorder.notes (0, 6000);
    REQUIRE (notes.size() == 2);
    CHECK (static_cast<int> (notes[0]["start"]) == 1000);
    CHECK (static_cast<int> (notes[0]["length"]) == 3000);
    CHECK_THAT (static_cast<double> (notes[0]["releaseVelocity"]), WithinAbs (0.25, 1e-6));

    const auto& pitch = notes[0]["expression"]["pitch"];
    REQUIRE (pitch.size() == 2);
    CHECK (static_cast<int> (pitch[1][0]) == 2000);
    CHECK_THAT (static_cast<double> (pitch[1][1]), WithinAbs (2.0, 1e-5));
    CHECK_FALSE (notes[0]["expression"].hasProperty ("pressure")); // default all along

    CHECK (static_cast<int> (notes[1]["length"]) == 1000);
    CHECK (! notes[1].hasProperty ("expression"));

    // Notes before the clip start are left out; positions are relative to it.
    const auto later = recorder.notes (2000, 6000);
    REQUIRE (later.size() == 1);
    CHECK (static_cast<int> (later[0]["start"]) == 3000);
}

TEST_CASE ("a note held across a loop wrap ends at the loop end", "[midi][record]")
{
    NoteRecorder recorder;
    LiveInput on;
    on.noteId = -1;
    recorder.add ({ on, 3000.0 });
    LiveInput off;
    off.noteId = -1;
    off.type = NoteEventType::noteOff;
    recorder.add ({ off, 500.0 }); // after the wrap

    const auto notes = recorder.notes (0, 4000, 4000);
    REQUIRE (notes.size() == 1);
    CHECK (static_cast<int> (notes[0]["length"]) == 1000);
}

// ---------------------------------------------------------------------------------------------------------
// Takes and MIDI mappings

TEST_CASE ("loop-recorded passes go into takes lanes, the last on top", "[midi][record][commands][takes]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    const auto file = makeObject ({ { "path", "Audio/take.wav" },
                                    { "name", "take" },
                                    { "sampleRate", 48000 },
                                    { "channels", 1 },
                                    { "frames", 48000 * 8 } });

    // 8 s at 120 BPM = 16 beats: three passes of one bar after a bar of lead-in, as the recording leaves them
    // (docs/design/takes-lanes.md, T4): one clip per pass, all from the same file.
    const auto bar = 4 * ticksPerBeat;
    juce::Array<juce::var> commands;

    for (int pass = 0; pass < 3; ++pass)
        commands.add (makeObject ({ { "command", "addAudioClip" },
                                    { "args", makeObject ({ { "trackId", trackId },
                                                            { "start", 0 },
                                                            { "file", file },
                                                            { "offset", bar + pass * bar },
                                                            { "length", bar },
                                                            { "recorded", true } }) } }));

    project = apply (project, "batch", { { "commands", commands } });
    const auto& track = *project.tracks[0];
    REQUIRE (track.clips.size() == 3);
    CHECK (track.lanes == 3);
    CHECK (project.audioFiles.size() == 1); // one file for every pass

    // The last pass is on the top row and plays; the first is at the bottom.
    const auto played = playedClips (track.clips);
    REQUIRE (played.size() == 1);
    CHECK (played[0]->offset == 3 * bar);
    CHECK (played[0]->lane == 0);
    const auto bottom = std::ranges::find_if (track.clips, [] (const ClipPtr& c) { return c->lane == 2; });
    REQUIRE (bottom != track.clips.end());
    CHECK ((*bottom)->offset == bar);
}

TEST_CASE ("the recorder keeps each loop pass apart", "[midi][record][takes]")
{
    NoteRecorder recorder;
    const auto event = [] (NoteEventType type, ObjectId id, double tick, int pass)
    {
        RecordedInput input;
        input.input.type = type;
        input.input.noteId = id;
        input.input.pitch = 60.0 + static_cast<double> (id);
        input.input.velocity = 0.8f;
        input.tick = tick;
        input.pass = pass;
        return input;
    };

    recorder.add (event (NoteEventType::noteOn, 1, 100, 0));
    recorder.add (event (NoteEventType::noteOff, 1, 200, 0));
    recorder.add (event (NoteEventType::noteOn, 2, 300, 1));
    recorder.add (event (NoteEventType::noteOff, 2, 400, 1));

    CHECK (recorder.passes() == 2);
    CHECK (recorder.notes (0, 1000, 1000, 0).size() == 1);
    CHECK (recorder.notes (0, 1000, 1000, 1).size() == 1);
    CHECK (recorder.notes (0, 1000, 1000).size() == 2); // overdub: every pass
}

TEST_CASE ("MIDI mappings are one per parameter and per controller", "[midi][commands]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;

    project = apply (project, "setMidiMapping",
                     { { "channel", 1 }, { "controller", 7 }, { "trackId", trackId }, { "param", "gain" } });
    project = apply (project, "setMidiMapping",
                     { { "channel", 1 }, { "controller", 10 }, { "trackId", trackId }, { "param", "gain" } });
    REQUIRE (project.midiMappings.size() == 1);
    CHECK (project.midiMappings[0].controller == 10);

    project = apply (project, "setMidiMapping",
                     { { "channel", 1 }, { "controller", 10 }, { "trackId", trackId }, { "param", "pan" } });
    REQUIRE (project.midiMappings.size() == 1);
    CHECK (project.midiMappings[0].param == "pan");

    CHECK_THROWS_AS (apply (project, "setMidiMapping",
                            { { "channel", 1 }, { "controller", 1 }, { "trackId", trackId }, { "param", "volume" } }),
                     CommandError);

    CHECK (static_cast<int> (toVar (project)["midiMappings"][0]["controller"]) == 10);
    CHECK (apply (project, "removeTrack", { { "id", trackId } }).midiMappings.empty());
    CHECK (apply (project, "removeMidiMapping", { { "trackId", trackId }, { "param", "pan" } }).midiMappings.empty());
}
