#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Sequence.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int samplesPerBeat = 24000; // 120 BPM at 48 kHz

[[nodiscard]] juce::var array (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

// A project with one note, plus a compiler and engine already running it.
struct Rig
{
    explicit Rig (const juce::var& note)
    {
        auto setup = projectWithClip();
        project = apply (setup.project, "addNotes", { { "clipId", setup.clipId }, { "notes", array ({ note }) } });
        clipId = setup.clipId;
        noteId = project.tracks[0]->clips[0]->notes[0]->id;
        engine.setEngine (compiler.compile (project, spec));
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);

        if (auto compiled = compiler.update (project, spec))
            engine.setEngine (std::move (compiled));
    }

    Project project;
    ObjectId clipId = 0;
    ObjectId noteId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};

[[nodiscard]] double frequencyOf (const std::vector<float>& samples, size_t from, size_t to)
{
    const auto seconds = static_cast<double> (to - from) / sampleRate;
    return zeroCrossings (samples, from, to) / (2.0 * seconds);
}

[[nodiscard]] float peak (const std::vector<float>& samples, size_t from, size_t to)
{
    float result = 0.0f;
    for (auto i = from; i < std::min (to, samples.size()); ++i)
        result = std::max (result, std::abs (samples[i]));
    return result;
}
} // namespace

TEST_CASE ("a stopped engine is silent", "[engine]")
{
    Rig rig (noteArgs (0, ticksPerBeat, 69));
    CHECK (firstAudible (render (rig.engine, 4096).left()) == -1);
}

TEST_CASE ("notes start at the exact sample", "[engine][sequencer]")
{
    Rig rig (noteArgs (ticksPerBeat, ticksPerBeat, 69)); // starts at beat 1 = sample 24000
    rig.engine.play();

    const auto buffer = render (rig.engine, 2 * samplesPerBeat);
    const auto first = firstAudible (buffer.left());

    CHECK (first >= samplesPerBeat);
    CHECK (first <= samplesPerBeat + 2); // sin(0) = 0 at the very first sample
}

TEST_CASE ("notes end with a release tail", "[engine][sequencer]")
{
    Rig rig (noteArgs (0, ticksPerBeat, 69));
    rig.engine.play();

    const auto buffer = render (rig.engine, 4 * samplesPerBeat);
    const auto& left = buffer.left();

    CHECK (firstAudible ({ left.begin(), left.begin() + samplesPerBeat }) >= 0); // sounding during the note
    CHECK (firstAudible ({ left.begin() + std::ptrdiff_t { 3 } * samplesPerBeat, left.end() })
           == -1); // fully released after 2 beats
}

TEST_CASE ("pitch curves bend the synth", "[engine][synth]")
{
    auto note = noteArgs (0, 4 * ticksPerBeat, 69); // A4 = 440 Hz
    note.getDynamicObject()->setProperty ("expression",
                                          makeObject ({ { "pitch", array ({ array ({ 0, 12.0 }) }) } })); // +1 octave
    Rig rig (note);
    rig.engine.play();

    const auto buffer = render (rig.engine, samplesPerBeat);
    const auto measured = frequencyOf (buffer.left(), 2400, samplesPerBeat);

    CHECK_THAT (measured, WithinAbs (880.0, 10.0));
}

TEST_CASE ("editing a sounding note keeps the voice and follows the edit", "[engine][synth]")
{
    Rig rig (noteArgs (0, 8 * ticksPerBeat, 69));
    rig.engine.play();
    std::ignore = render (rig.engine, samplesPerBeat);

    rig.edit ("updateNotes", { { "clipId", rig.clipId },
                               { "notes", array ({ makeObject ({ { "id", rig.noteId }, { "pitch", 81 } }) }) } });

    const auto buffer = render (rig.engine, samplesPerBeat);

    CHECK (firstAudible (buffer.left()) <= 2); // no gap: the voice carried over (sample 0 may be a zero crossing)
    CHECK_THAT (frequencyOf (buffer.left(), 2400, samplesPerBeat), WithinAbs (880.0, 10.0));
}

TEST_CASE ("deleting a sounding note chokes it", "[engine][synth]")
{
    Rig rig (noteArgs (0, 8 * ticksPerBeat, 69));
    rig.engine.play();
    std::ignore = render (rig.engine, samplesPerBeat);

    rig.edit ("removeNotes", { { "clipId", rig.clipId }, { "ids", array ({ rig.noteId }) } });

    const auto buffer = render (rig.engine, samplesPerBeat);
    CHECK (firstAudible ({ buffer.left().begin() + 2400, buffer.left().end() }) == -1); // silent within 50 ms
}

TEST_CASE ("stopping releases notes and returns to the start position", "[engine][transport]")
{
    Rig rig (noteArgs (0, 8 * ticksPerBeat, 69));
    rig.engine.seek (0.5);
    rig.engine.play();
    std::ignore = render (rig.engine, samplesPerBeat); // one beat, from 0.5 to 1.5

    rig.engine.stop();
    const auto buffer = render (rig.engine, 2 * samplesPerBeat);

    // The release tail (0.3 s default) has died away 1.5 beats (0.75 s) after stopping.
    CHECK (firstAudible ({ buffer.left().begin() + 3 * samplesPerBeat / 2, buffer.left().end() }) == -1);
    CHECK_THAT (rig.engine.getTelemetry().positionBeats, WithinAbs (0.5, 1.0e-6)); // back where it started
}

TEST_CASE ("seeking moves the playhead and chases notes", "[engine][transport]")
{
    Rig rig (noteArgs (4 * ticksPerBeat, 4 * ticksPerBeat, 69)); // a note from beat 4 to beat 8

    rig.engine.seek (6.0); // while stopped
    std::ignore = render (rig.engine, blockSize);
    CHECK_THAT (rig.engine.getTelemetry().positionBeats, WithinAbs (6.0, 1.0e-6));

    rig.engine.play(); // starts inside the note: it is chased
    const auto playing = render (rig.engine, samplesPerBeat / 2);
    CHECK (firstAudible (playing.left()) <= 2);

    rig.engine.seek (0.0); // jump out of the note while playing: it is choked
    const auto after = render (rig.engine, samplesPerBeat);
    CHECK (firstAudible ({ after.left().begin() + 2400, after.left().end() }) == -1);
    CHECK_THAT (rig.engine.getTelemetry().positionBeats, WithinAbs (1.0, 1.0e-6));
}

TEST_CASE ("tempo changes keep the playhead continuous", "[engine][transport]")
{
    Rig rig (noteArgs (0, ticksPerBeat, 69));
    rig.engine.play();
    std::ignore = render (rig.engine, static_cast<int> (sampleRate)); // 1 s at 120 BPM = 2 beats

    rig.edit ("setTempo", { { "bpm", 240.0 } });
    std::ignore = render (rig.engine, static_cast<int> (sampleRate)); // 1 s at 240 BPM = 4 beats

    CHECK_THAT (rig.engine.getTelemetry().positionBeats, WithinAbs (6.0, 1.0e-6));
}

TEST_CASE ("the metronome clicks on beats when enabled", "[engine][transport]")
{
    auto silentNote = noteArgs (0, ticksPerBeat, 60);
    silentNote.getDynamicObject()->setProperty ("velocity", 0.0);
    Rig rig (silentNote);
    rig.edit ("setMetronome", { { "enabled", true } });
    rig.engine.play();

    const auto buffer = render (rig.engine, samplesPerBeat + 100);
    const auto& left = buffer.left();

    CHECK (firstAudible (left, 1.0e-3f) <= 2);
    CHECK (firstAudible ({ left.begin() + samplesPerBeat - 100, left.end() }, 1.0e-3f) >= 100);
}

TEST_CASE ("the sequencer chases notes when playback starts inside them", "[sequencer]")
{
    auto setup = projectWithClip();
    const auto project =
        apply (setup.project, "addNotes",
               { { "clipId", setup.clipId }, { "notes", array ({ noteArgs (0, 4 * ticksPerBeat, 60) }) } });
    const TrackSequence sequence (project.tracks[0]->clips);

    auto sequencer = NodeRegistry::builtin().create (nodetype::sequencer);
    sequencer->prepare (spec);

    EventBuffer events;
    Transport transport;
    transport.playing = true;
    transport.started = true;
    transport.timeMap = TimeMap (constantTempo(), sampleRate);
    transport.startTick = 2.0 * ticksPerBeat; // inside the note
    transport.startSample = juce::int64 { 2 } * samplesPerBeat;
    transport.timeMap.anchor (transport.startTick, transport.startSample);
    transport.endTick = transport.timeMap.sampleToTick (static_cast<double> (transport.startSample + blockSize));

    ProcessContext context;
    context.numSamples = blockSize;
    context.eventsOut = &events;
    context.transport = &transport;
    context.sequence = &sequence;
    sequencer->process (context);

    REQUIRE (events.events().size() == 1);
    CHECK (events.events()[0].type == NoteEventType::noteOn);
    CHECK (events.events()[0].sampleOffset == 0);
}

TEST_CASE ("rendering a project never allocates", "[engine][realtime]")
{
    auto note = noteArgs (0, ticksPerBeat, 60);
    note.getDynamicObject()->setProperty (
        "expression", makeObject ({ { "pressure", array ({ array ({ 0, 0.0 }), array ({ 960, 1.0 }) }) } }));
    Rig rig (note);
    rig.edit ("setMetronome", { { "enabled", true } });
    rig.engine.play();

    const auto before = allocationViolationCount();
    StereoBuffer buffer (8192);

    for (int i = 0; i < 20; ++i)
        rig.engine.renderOffline (buffer.channels(), 2, buffer.size(), sampleRate);

    rig.edit ("updateNotes", { { "clipId", rig.clipId },
                               { "notes", array ({ makeObject ({ { "id", rig.noteId }, { "pitch", 64 } }) }) } });
    rig.engine.renderOffline (buffer.channels(), 2, buffer.size(), sampleRate);

    CHECK (allocationViolationCount() == before);
}

TEST_CASE ("auditioned notes play while the transport is stopped", "[engine][audition]")
{
    Rig rig (noteArgs (0, ticksPerBeat, 60));
    const auto trackId = rig.project.tracks[0]->id;

    const auto id = rig.engine.auditionNoteOn (trackId, 69.0, 0.8f);
    REQUIRE (id < 0);

    const auto held = render (rig.engine, samplesPerBeat);
    CHECK (firstAudible (held.left()) <= 2);
    CHECK_THAT (frequencyOf (held.left(), 2400, samplesPerBeat), WithinAbs (440.0, 10.0));

    rig.engine.auditionNoteOff (trackId, id);
    const auto released = render (rig.engine, 2 * samplesPerBeat);
    CHECK (firstAudible ({ released.left().begin() + samplesPerBeat * 3 / 2, released.left().end() }) == -1);
}

TEST_CASE ("auditioned notes survive edits to the track", "[engine][audition]")
{
    Rig rig (noteArgs (0, ticksPerBeat, 60));
    const auto id = rig.engine.auditionNoteOn (rig.project.tracks[0]->id, 69.0, 0.8f);
    std::ignore = render (rig.engine, blockSize);

    rig.edit ("removeNotes", { { "clipId", rig.clipId }, { "ids", array ({ rig.noteId }) } });
    const auto buffer = render (rig.engine, samplesPerBeat);

    CHECK (firstAudible ({ buffer.left().begin() + samplesPerBeat / 2, buffer.left().end() }) >= 0);
    rig.engine.auditionNoteOff (rig.project.tracks[0]->id, id);
}

// The tempo map in the engine (docs/design/tempo-map.md, TM7 and TM9).

TEST_CASE ("notes after a tempo change start at the map's sample", "[engine][tempo]")
{
    SECTION ("a step")
    {
        // 2 beats at 120 (48000 samples), then 4 beats at 60 (192000 samples).
        Rig rig (noteArgs (6 * ticksPerBeat, ticksPerBeat, 69));
        rig.edit ("addTempoPoint", { { "tick", 2 * ticksPerBeat }, { "bpm", 60.0 } });
        rig.engine.play();

        const auto first = firstAudible (render (rig.engine, 241000).left());
        CHECK (first >= 240000);
        CHECK (first <= 240002);
    }

    SECTION ("on a ramp")
    {
        // 60 to 120 BPM over 4 beats; beat 3 is at 4 ln (1.75) seconds.
        Rig rig (noteArgs (3 * ticksPerBeat, ticksPerBeat, 69));
        rig.edit ("setTempoPoint", { { "index", 0 }, { "bpm", 60.0 }, { "ramp", true } });
        rig.edit ("addTempoPoint", { { "tick", 4 * ticksPerBeat }, { "bpm", 120.0 } });
        rig.engine.play();

        const auto expected = static_cast<int> (std::ceil (4.0 * std::log (1.75) * sampleRate));
        const auto first = firstAudible (render (rig.engine, expected + 1000).left());
        CHECK (std::abs (first - expected) <= 2);
    }
}

TEST_CASE ("the metronome follows tempo changes and time signatures", "[engine][tempo]")
{
    auto silentNote = noteArgs (0, ticksPerBeat, 60);
    silentNote.getDynamicObject()->setProperty ("velocity", 0.0);
    Rig rig (silentNote);
    rig.edit ("setMetronome", { { "enabled", true } });

    const auto clickAt = [] (const std::vector<float>& left, int sample)
    { return peak (left, static_cast<size_t> (sample), static_cast<size_t> (sample) + 2000); };

    SECTION ("a tempo step")
    {
        // Clicks at 0, 24000, 48000, then a beat at 60 BPM later at 96000; nothing where 120 BPM would click.
        rig.edit ("addTempoPoint", { { "tick", 2 * ticksPerBeat }, { "bpm", 60.0 } });
        rig.engine.play();
        const auto buffer = render (rig.engine, 100000);
        const auto& left = buffer.left();

        CHECK (clickAt (left, 48000) > 0.1f);
        CHECK (clickAt (left, 72000) < 1.0e-3f);
        CHECK (clickAt (left, 96000 - 2100) < 1.0e-3f);
        CHECK (clickAt (left, 96000) > 0.1f);
    }

    SECTION ("6/8 clicks on dotted quarters, with quieter eighths")
    {
        // Eighths every 12000 samples; the bar's first is accented, the fourth is a full click.
        rig.edit ("setSignature", { { "bar", 0 }, { "numerator", 6 }, { "denominator", 8 } });
        rig.engine.play();
        const auto buffer = render (rig.engine, 40000);
        const auto& left = buffer.left();

        CHECK (clickAt (left, 12000) > 2.0f * peak (left, 10000, 12000)); // a new click, not the tail
        CHECK (clickAt (left, 12000) > 0.02f);
        CHECK (clickAt (left, 12000) < 0.5f * clickAt (left, 36000));
        CHECK (clickAt (left, 24000) < 0.5f * clickAt (left, 36000));
    }
}

TEST_CASE ("tracks rendering as soon as their buses are full match the serial path exactly", "[engine][render]")
{
    // Nested groups, a return fed from inside and outside them, a key send, and an instrument with an output track,
    // over blocks that don't divide the beat: every bus a reader sums while other tracks still render.
    auto project = apply ({}, "addTrack", { { "name", "A" } });

    for (const auto* name : { "B", "C", "D" })
        project = apply (project, "addTrack", { { "name", name } });

    project = apply (project, "addTrack", { { "kind", "return" }, { "name", "R" } });

    const auto idOf = [&project] (const juce::String& name)
    {
        for (const auto& track : project.tracks)
            if (track->name == name)
                return track->id;

        return ObjectId { 0 };
    };

    const auto nodeOf = [&project] (ObjectId trackId, const char* type) -> ObjectId
    {
        for (const auto& node : project.findTrack (trackId)->graph.nodes)
            if (node.type == type)
                return node.id;

        return 0;
    };

    double pitch = 55.0;

    for (const auto* name : { "A", "B", "C", "D" })
    {
        const auto trackId = idOf (name);
        project =
            apply (project, "addClip", { { "trackId", trackId }, { "start", 0 }, { "length", 4 * ticksPerBeat } });
        const auto clipId = project.findTrack (trackId)->clips.back()->id;
        project = apply (project, "addNotes",
                         { { "clipId", clipId },
                           { "notes", array ({ noteArgs (0, 3 * ticksPerBeat, pitch),
                                               noteArgs (ticksPerBeat / 3, ticksPerBeat, pitch + 7.0) }) } });
        pitch += 4.0;
    }

    project = apply (project, "groupTracks",
                     { { "ids", array ({ juce::var (idOf ("A")), juce::var (idOf ("B")) }) }, { "name", "G1" } });
    project = apply (project, "groupTracks",
                     { { "ids", array ({ juce::var (idOf ("G1")), juce::var (idOf ("C")) }) }, { "name", "G2" } });
    // Four sends into one return: with more than two inputs, a bus sums the same only in the same order.
    for (const auto* name : { "A", "B", "C", "D" })
        project = apply (project, "addSend", { { "trackId", idOf (name) }, { "target", idOf ("R") } });
    project = apply (project, "addSend",
                     { { "trackId", idOf ("D") },
                       { "target", idOf ("C") },
                       { "targetNode", nodeOf (idOf ("C"), nodetype::compressor) } });

    // D plays through an output track as well.
    const auto d = idOf ("D");
    project = apply (project, "addTrack", { { "kind", "output" }, { "source", d }, { "name", "D out" } });
    project = apply (project, "addNode", { { "trackId", d }, { "type", nodetype::busOutput } });
    const auto busOutput = nodeOf (d, nodetype::busOutput);
    const auto trackOutput = nodeOf (d, nodetype::trackOutput);
    const auto& graph = project.findTrack (d)->graph;
    const auto feed = std::ranges::find (graph.connections, trackOutput, &Connection::toNode);
    REQUIRE (feed != graph.connections.end());
    const auto instrument = feed->fromNode;
    project =
        apply (project, "setNodeTarget", { { "trackId", d }, { "nodeId", busOutput }, { "target", idOf ("D out") } });
    project = apply (
        project, "connect",
        { { "trackId", d }, { "fromNode", instrument }, { "fromPort", 0 }, { "toNode", busOutput }, { "toPort", 0 } });

    const auto renderWith = [&project] (bool parallel)
    {
        AudioEngine engine;
        engine.setParallelTracks (parallel);
        GraphCompiler compiler;
        engine.setEngine (compiler.compile (project, spec));
        engine.play();

        constexpr int length = 300;
        StereoBuffer buffer (length);
        std::vector<float> left;
        std::vector<float> right;

        for (int block = 0; block < 2 * samplesPerBeat / length; ++block)
        {
            engine.renderOffline (buffer.channels(), 2, length, sampleRate);
            left.insert (left.end(), buffer.left().begin(), buffer.left().end());
            right.insert (right.end(), buffer.right().begin(), buffer.right().end());
        }

        return std::pair { left, right };
    };

    const auto [serialLeft, serialRight] = renderWith (false);
    CHECK (std::ranges::any_of (serialLeft, [] (float sample) { return std::abs (sample) > 1.0e-4f; }));

    // Several times: which thread takes which track changes from run to run.
    for (int run = 0; run < 5; ++run)
    {
        const auto [parallelLeft, parallelRight] = renderWith (true);
        CHECK (serialLeft == parallelLeft);
        CHECK (serialRight == parallelRight);
    }
}

TEST_CASE ("parallel track rendering matches the serial path exactly", "[engine][render]")
{
    // A group bus, a return send and a key bus between them, so every kind of bus dependency is covered.
    auto project = apply ({}, "addTrack", { { "name", "A" } });
    project = apply (project, "addTrack", { { "name", "B" } });
    project = apply (project, "addTrack", { { "name", "C" } });
    project = apply (project, "addTrack", { { "kind", "return" }, { "name", "R" } });

    const auto idOf = [&project] (const juce::String& name)
    {
        for (const auto& track : project.tracks)
            if (track->name == name)
                return track->id;

        return ObjectId { 0 };
    };

    const auto addNote = [&project] (ObjectId trackId, double pitch)
    {
        project =
            apply (project, "addClip", { { "trackId", trackId }, { "start", 0 }, { "length", 4 * ticksPerBeat } });
        const auto clipId = project.findTrack (trackId)->clips.back()->id;
        project = apply (project, "addNotes",
                         { { "clipId", clipId }, { "notes", array ({ noteArgs (0, 4 * ticksPerBeat, pitch) }) } });
    };

    addNote (idOf ("A"), 60.0);
    addNote (idOf ("B"), 64.0);
    addNote (idOf ("C"), 67.0);

    project = apply (project, "groupTracks",
                     { { "ids", array ({ juce::var (idOf ("A")), juce::var (idOf ("B")) }) }, { "name", "G" } });
    project = apply (project, "addSend", { { "trackId", idOf ("A") }, { "target", idOf ("R") } });

    const auto compressor = [&project] (ObjectId trackId) -> ObjectId
    {
        for (const auto& node : project.findTrack (trackId)->graph.nodes)
            if (node.type == nodetype::compressor)
                return node.id;

        return 0;
    };

    project =
        apply (project, "addSend",
               { { "trackId", idOf ("A") }, { "target", idOf ("B") }, { "targetNode", compressor (idOf ("B")) } });

    AudioEngine serial;
    serial.setParallelTracks (false);
    GraphCompiler serialCompiler;
    serial.setEngine (serialCompiler.compile (project, spec));
    serial.play();

    AudioEngine parallel;
    parallel.setParallelTracks (true);
    GraphCompiler parallelCompiler;
    parallel.setEngine (parallelCompiler.compile (project, spec));
    parallel.play();

    const auto frames = 2 * samplesPerBeat;
    StereoBuffer serialOut (frames);
    StereoBuffer parallelOut (frames);
    serial.renderOffline (serialOut.channels(), 2, frames, sampleRate);
    parallel.renderOffline (parallelOut.channels(), 2, frames, sampleRate);

    CHECK (serialOut.left() == parallelOut.left());
    CHECK (serialOut.right() == parallelOut.right());
    CHECK (std::any_of (serialOut.left().begin(), serialOut.left().end(),
                        [] (float sample) { return std::abs (sample) > 1.0e-4f; }));
    CHECK (serial.parallelTracks() == false);
    CHECK (parallel.parallelTracks());
}
