#include "TestUtils.h"
#include "dawg/Commands.h"
#include "dawg/MidiFileIO.h"
#include "dawg/VarUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// MIDI files (docs/design/sampler-midi-delay-limiter.md, section 3).

namespace
{
constexpr int ppq = 480;

[[nodiscard]] juce::MidiMessage at (juce::MidiMessage message, int fileTick)
{
    message.setTimeStamp (fileTick);
    return message;
}

[[nodiscard]] juce::MemoryBlock fileOf (const std::vector<juce::MidiMessageSequence>& tracks, int format = 1)
{
    juce::MidiFile file;
    file.setTicksPerQuarterNote (ppq);

    for (const auto& track : tracks)
        file.addTrack (track);

    juce::MemoryOutputStream stream;
    file.writeTo (stream, format);
    return stream.getMemoryBlock();
}

void addNote (juce::MidiMessageSequence& track, int channel, int key, int start, int length, float velocity = 0.5f)
{
    track.addEvent (at (juce::MidiMessage::noteOn (channel, key, velocity), start));
    track.addEvent (at (juce::MidiMessage::noteOff (channel, key), start + length));
}

[[nodiscard]] juce::Array<juce::var>& notesOf (const juce::var& part)
{
    return *part["notes"].getArray();
}

// An instrument track's notes as (start, length, pitch) in ticks, in order.
[[nodiscard]] std::vector<std::tuple<Ticks, Ticks, double>> notesOf (const Project& project, size_t track)
{
    std::vector<std::tuple<Ticks, Ticks, double>> result;

    for (const auto& clip : project.tracks[track]->clips)
        for (const auto& note : clip->notes)
            result.emplace_back (clip->start + note->start, note->length, note->pitch);

    std::ranges::sort (result);
    return result;
}

[[nodiscard]] juce::var imported (const juce::var& parsed, bool useTempo = false)
{
    auto args = makeObject ({ { "tracks", parsed["tracks"] } });

    if (useTempo)
    {
        args.getDynamicObject()->setProperty ("tempo", parsed["tempo"]);
        args.getDynamicObject()->setProperty ("signatures", parsed["signatures"]);
    }

    return args;
}
} // namespace

TEST_CASE ("a MIDI file's notes come in on the DAW's time scale", "[midifile]")
{
    juce::MidiMessageSequence track;
    track.addEvent (at (juce::MidiMessage::textMetaEvent (3, "Piano"), 0));
    addNote (track, 1, 60, 0, ppq, 0.5f);
    addNote (track, 1, 64, ppq, ppq / 2);

    const auto parsed = parseMidiFile (fileOf ({ track }), "song");
    REQUIRE (parsed["tracks"].size() == 1);
    const auto& part = parsed["tracks"][0];
    CHECK (part["name"].toString() == "Piano");
    CHECK_FALSE (static_cast<bool> (part["drums"]));

    const auto& notes = notesOf (part);
    REQUIRE (notes.size() == 2);
    CHECK (static_cast<int> (notes[0]["start"]) == 0);
    CHECK (static_cast<juce::int64> (notes[0]["length"]) == ticksPerBeat);
    CHECK (static_cast<double> (notes[0]["pitch"]) == 60.0);
    CHECK_THAT (static_cast<double> (notes[0]["velocity"]), WithinAbs (0.5, 0.01));
    CHECK (static_cast<juce::int64> (notes[1]["start"]) == ticksPerBeat);
    CHECK (static_cast<juce::int64> (notes[1]["length"]) == ticksPerBeat / 2);
    CHECK (static_cast<juce::int64> (part["length"]) == 4 * ticksPerBeat); // one whole bar
}

TEST_CASE ("each channel of a MIDI track becomes a part, and channel 10 is drums", "[midifile]")
{
    juce::MidiMessageSequence track;
    addNote (track, 1, 60, 0, ppq);
    addNote (track, 10, 36, 0, ppq);
    track.addEvent (at (juce::MidiMessage::controllerEvent (3, 7, 100), 0)); // a channel without notes

    const auto parsed = parseMidiFile (fileOf ({ track }, 0), "song");
    REQUIRE (parsed["tracks"].size() == 2);
    CHECK (parsed["tracks"][0]["name"].toString() == "song ch 1");
    CHECK_FALSE (static_cast<bool> (parsed["tracks"][0]["drums"]));
    CHECK (static_cast<bool> (parsed["tracks"][1]["drums"]));
}

TEST_CASE ("tracks without notes don't make parts, and tempo and signatures are read", "[midifile]")
{
    juce::MidiMessageSequence meta;
    meta.addEvent (at (juce::MidiMessage::tempoMetaEvent (500000), 0));       // 120 bpm
    meta.addEvent (at (juce::MidiMessage::tempoMetaEvent (400000), 4 * ppq)); // 150 bpm
    meta.addEvent (at (juce::MidiMessage::timeSignatureMetaEvent (3, 4), 0));
    meta.addEvent (at (juce::MidiMessage::timeSignatureMetaEvent (6, 8), 6 * ppq));  // after two bars of 3/4
    meta.addEvent (at (juce::MidiMessage::timeSignatureMetaEvent (3, 32), 9 * ppq)); // no grid for this: left out

    juce::MidiMessageSequence notes;
    addNote (notes, 1, 60, 0, ppq);

    const auto parsed = parseMidiFile (fileOf ({ meta, notes }), "song");
    CHECK (parsed["tracks"].size() == 1);

    REQUIRE (parsed["tempo"].size() == 2);
    CHECK_THAT (static_cast<double> (parsed["tempo"][1]["bpm"]), WithinAbs (150.0, 0.01));
    CHECK (static_cast<juce::int64> (parsed["tempo"][1]["tick"]) == 4 * ticksPerBeat);

    REQUIRE (parsed["signatures"].size() == 2);
    CHECK (static_cast<int> (parsed["signatures"][0]["numerator"]) == 3);
    CHECK (static_cast<int> (parsed["signatures"][1]["bar"]) == 2);
    CHECK (static_cast<int> (parsed["signatures"][1]["denominator"]) == 8);
}

TEST_CASE ("a tempo is 120 bpm without one", "[midifile]")
{
    juce::MidiMessageSequence notes;
    addNote (notes, 1, 60, 0, ppq);
    const auto parsed = parseMidiFile (fileOf ({ notes }), "song");
    REQUIRE (parsed["tempo"].size() == 1);
    CHECK (static_cast<double> (parsed["tempo"][0]["bpm"]) == 120.0);
}

TEST_CASE ("pitch bend and pressure become note curves", "[midifile]")
{
    juce::MidiMessageSequence track;
    track.addEvent (at (juce::MidiMessage::noteOn (1, 60, 0.5f), 0));
    track.addEvent (at (juce::MidiMessage::pitchWheel (1, 16383), ppq)); // +2 semitones in plain MIDI
    track.addEvent (at (juce::MidiMessage::channelPressureChange (1, 127), ppq));
    track.addEvent (at (juce::MidiMessage::noteOff (1, 60), 2 * ppq));

    const auto parsed = parseMidiFile (fileOf ({ track }), "song");
    const auto expression = notesOf (parsed["tracks"][0])[0]["expression"];
    REQUIRE (expression.isObject());
    const auto* pitch = expression["pitch"].getArray();
    REQUIRE (pitch != nullptr);
    CHECK_THAT (static_cast<double> ((*pitch)[pitch->size() - 1][1]), WithinAbs (2.0, 0.01));
    const auto* pressure = expression["pressure"].getArray();
    REQUIRE (pressure != nullptr);
    CHECK_THAT (static_cast<double> ((*pressure)[pressure->size() - 1][1]), WithinAbs (1.0, 0.01));
}

TEST_CASE ("other controllers become clip controllers", "[midifile]")
{
    juce::MidiMessageSequence track;
    addNote (track, 1, 60, 0, 4 * ppq);
    track.addEvent (at (juce::MidiMessage::controllerEvent (1, 1, 127), ppq));
    track.addEvent (at (juce::MidiMessage::controllerEvent (1, 64, 127), ppq));

    const auto parsed = parseMidiFile (fileOf ({ track }), "song");
    const auto controllers = parsed["tracks"][0]["controllers"];
    CHECK (controllers["1"].isArray());
    CHECK (controllers["64"].isArray());
}

TEST_CASE ("an MPE file's notes come in on one part with their own curves", "[midifile]")
{
    juce::MidiMessageSequence track;
    track.addEvent (at (juce::MidiMessage::controllerEvent (1, 101, 0), 0));
    track.addEvent (at (juce::MidiMessage::controllerEvent (1, 100, 6), 0));
    track.addEvent (at (juce::MidiMessage::controllerEvent (1, 6, 15), 0));
    addNote (track, 2, 60, 0, 2 * ppq);
    addNote (track, 3, 64, 0, 2 * ppq);
    track.addEvent (at (juce::MidiMessage::pitchWheel (3, 8192 + 4096), ppq)); // +24 of ±48, this note only

    const auto parsed = parseMidiFile (fileOf ({ track }), "song");
    REQUIRE (parsed["tracks"].size() == 1);
    const auto& notes = notesOf (parsed["tracks"][0]);
    REQUIRE (notes.size() == 2);

    const auto& bent = static_cast<double> (notes[0]["pitch"]) == 64.0 ? notes[0] : notes[1];
    const auto& still = static_cast<double> (notes[0]["pitch"]) == 64.0 ? notes[1] : notes[0];
    const auto* pitch = bent["expression"]["pitch"].getArray();
    REQUIRE (pitch != nullptr);
    CHECK_THAT (static_cast<double> ((*pitch)[pitch->size() - 1][1]), WithinAbs (24.0, 0.05));
    CHECK_FALSE (still["expression"]["pitch"].isArray());
}

TEST_CASE ("what isn't a usable MIDI file is refused", "[midifile]")
{
    const juce::String text ("hello");
    CHECK_THROWS_AS (parseMidiFile (juce::MemoryBlock (text.toRawUTF8(), 5), "x"), CommandError);

    juce::MidiFile smpte;
    smpte.setSmpteTimeFormat (25, 40);
    juce::MidiMessageSequence track;
    addNote (track, 1, 60, 0, 100);
    smpte.addTrack (track);
    juce::MemoryOutputStream stream;
    smpte.writeTo (stream, 1);
    CHECK_THROWS_AS (parseMidiFile (stream.getMemoryBlock(), "x"), CommandError);
}

TEST_CASE ("importMidi adds tracks with clips in one step", "[midifile][commands]")
{
    juce::MidiMessageSequence piano;
    piano.addEvent (at (juce::MidiMessage::textMetaEvent (3, "Piano"), 0));
    addNote (piano, 1, 60, 0, ppq);
    juce::MidiMessageSequence kit;
    addNote (kit, 10, 36, 0, ppq);
    juce::MidiMessageSequence meta;
    meta.addEvent (at (juce::MidiMessage::tempoMetaEvent (400000), 0));
    const auto parsed = parseMidiFile (fileOf ({ meta, piano, kit }), "song");

    SECTION ("keeping the project's tempo, at a position")
    {
        auto args = imported (parsed);
        args.getDynamicObject()->setProperty ("start", 8 * ticksPerBeat);
        const auto project = applyCommand (Project {}, "importMidi", args);
        REQUIRE (project.tracks.size() == 2);
        CHECK (project.tracks[0]->name == "Piano");
        CHECK (project.tracks[0]->kind == TrackKind::instrument);
        CHECK (project.tracks[0]->clips[0]->start == 8 * ticksPerBeat);
        CHECK (project.tracks[0]->clips[0]->name == "Piano");
        CHECK (project.tracks[1]->drumMode == "drums");
        CHECK_THAT (project.tempoMap.bpmAt (0.0), WithinAbs (120.0, 1.0e-9));
    }

    SECTION ("taking the file's tempo")
    {
        const auto project = applyCommand (Project {}, "importMidi", imported (parsed, true));
        CHECK_THAT (project.tempoMap.bpmAt (0.0), WithinAbs (150.0, 0.01));
        CHECK (project.tracks[0]->drumMode == "auto");
    }
}

TEST_CASE ("plain notes survive a trip through a MIDI file", "[midifile]")
{
    auto project = apply ({}, "addTrack", { { "kind", "instrument" }, { "name", "Lead" } });
    const auto trackId = project.tracks[0]->id;
    const auto note = [] (Ticks start, Ticks length, int pitch)
    { return makeObject ({ { "start", start }, { "length", length }, { "pitch", pitch }, { "velocity", 0.5 } }); };
    project = apply (project, "addClip",
                     { { "trackId", trackId },
                       { "start", ticksPerBeat },
                       { "length", 4 * ticksPerBeat },
                       { "notes", juce::Array<juce::var> { note (0, ticksPerBeat, 60),
                                                           note (ticksPerBeat, ticksPerBeat / 2, 67) } } });
    project = apply (project, "setTempo", { { "bpm", 90.0 } });

    const auto parsed = parseMidiFile (writeMidiFile (project), "song");
    CHECK_THAT (static_cast<double> (parsed["tempo"][0]["bpm"]), WithinAbs (90.0, 0.01));
    REQUIRE (parsed["tracks"].size() == 1);
    CHECK (parsed["tracks"][0]["name"].toString() == "Lead");

    const auto back = applyCommand (Project {}, "importMidi", imported (parsed));
    CHECK (notesOf (back, 0) == notesOf (project, 0));
}

TEST_CASE ("notes with curves survive a trip through MPE", "[midifile]")
{
    auto project = apply ({}, "addTrack", { { "kind", "instrument" } });
    const auto trackId = project.tracks[0]->id;
    const auto curve =
        juce::Array<juce::var> { juce::Array<juce::var> { 0, 0.0 }, juce::Array<juce::var> { ticksPerBeat, 12.0 } };
    const auto bent = makeObject ({ { "start", 0 },
                                    { "length", 2 * ticksPerBeat },
                                    { "pitch", 60 },
                                    { "velocity", 0.5 },
                                    { "expression", makeObject ({ { "pitch", curve } }) } });
    const auto plain =
        makeObject ({ { "start", 0 }, { "length", 2 * ticksPerBeat }, { "pitch", 64 }, { "velocity", 0.5 } });
    project = apply (project, "addClip",
                     { { "trackId", trackId },
                       { "start", 0 },
                       { "length", 4 * ticksPerBeat },
                       { "notes", juce::Array<juce::var> { bent, plain } } });

    const auto parsed = parseMidiFile (writeMidiFile (project), "song");
    REQUIRE (parsed["tracks"].size() == 1); // MPE: one part, though each note has a channel
    const auto& notes = notesOf (parsed["tracks"][0]);
    REQUIRE (notes.size() == 2);

    const auto& back = static_cast<double> (notes[0]["pitch"]) == 60.0 ? notes[0] : notes[1];
    const auto* pitch = back["expression"]["pitch"].getArray();
    REQUIRE (pitch != nullptr);
    CHECK_THAT (static_cast<double> ((*pitch)[pitch->size() - 1][1]), WithinAbs (12.0, 0.05));
    const auto& other = static_cast<double> (notes[0]["pitch"]) == 60.0 ? notes[1] : notes[0];
    CHECK_FALSE (other["expression"]["pitch"].isArray());
}

TEST_CASE ("tracks without notes aren't exported", "[midifile]")
{
    const auto project = apply ({}, "addTrack", { { "kind", "instrument" } });
    const auto parsed = parseMidiFile (writeMidiFile (project), "song");
    CHECK (parsed["tracks"].size() == 0);
}
