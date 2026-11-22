#include "TestUtils.h"
#include "dawg/GraphCompiler.h"
#include "dawg/MpeEncoder.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"
#include "dawg/Sequence.h"
#include "dawg/VoiceAllocator.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>

// Linked notes (docs/design/linked-notes.md).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr auto samplesPerBeat = 24000; // 120 bpm at 48 kHz

[[nodiscard]] juce::var list (std::initializer_list<juce::var> items)
{
    juce::Array<juce::var> result;

    for (const auto& item : items)
        result.add (item);

    return result;
}

// Two notes in a clip, on the track's MPE Synth, the second linked from the first when asked.
struct LinkRig
{
    LinkRig (const juce::var& first, const juce::var& second)
    {
        auto setup = projectWithClip();
        trackId = setup.trackId;
        clipId = setup.clipId;
        project = apply (setup.project, "addNotes", { { "clipId", clipId }, { "notes", list ({ first, second }) } });
        const auto& notes = project.tracks[0]->clips[0]->notes;
        firstId = notes[0]->id;
        secondId = notes[1]->id;
    }

    void link (double glideMs)
    {
        project =
            apply (project, "setNoteLinks",
                   { { "clipId", clipId },
                     { "links",
                       list ({ makeObject ({ { "from", firstId }, { "to", secondId }, { "glideMs", glideMs } }) }) } });
    }

    [[nodiscard]] std::vector<float> play (int numSamples)
    {
        engine.setEngine (compiler.compile (project, spec));
        engine.play();
        return render (engine, numSamples).left();
    }

    [[nodiscard]] std::shared_ptr<const NoteClip> clip() const { return project.tracks[0]->clips[0]; }

    Project project;
    ObjectId trackId = 0;
    ObjectId clipId = 0;
    ObjectId firstId = 0;
    ObjectId secondId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};

[[nodiscard]] double frequencyOf (const std::vector<float>& samples, size_t from, size_t to)
{
    auto crossings = 0;

    for (auto i = from + 1; i < to; ++i)
        if (samples[i - 1] < 0.0f && samples[i] >= 0.0f)
            ++crossings;

    return crossings * 48000.0 / static_cast<double> (to - from);
}

[[nodiscard]] float peak (const std::vector<float>& samples, size_t from, size_t to)
{
    float result = 0.0f;

    for (auto i = from; i < to; ++i)
        result = std::max (result, std::abs (samples[i]));

    return result;
}
} // namespace

TEST_CASE ("links keep to one note each way, and follow edits", "[links][commands]")
{
    LinkRig rig (noteArgs (0, ticksPerBeat, 60), noteArgs (ticksPerBeat, ticksPerBeat, 72));
    rig.link (50.0);
    const auto& first = *rig.clip()->notes[0];
    REQUIRE (first.link.has_value());
    CHECK (first.link->to == rig.secondId);
    CHECK_THAT (first.link->glideMs, WithinAbs (50.0, 1e-9));

    SECTION ("a note can only link forward")
    {
        CHECK_THROWS_AS (applyCommand (rig.project, "setNoteLinks",
                                       makeObject ({ { "clipId", rig.clipId },
                                                     { "links", list ({ makeObject ({ { "from", rig.secondId },
                                                                                      { "to", rig.firstId } }) }) } })),
                         CommandError);
    }

    SECTION ("a new link to a note takes it from the old one")
    {
        auto project = apply (rig.project, "addNotes",
                              { { "clipId", rig.clipId }, { "notes", list ({ noteArgs (480, 480, 64) }) } });
        const auto& notes = project.tracks[0]->clips[0]->notes;
        const auto other = notes[1]->id;
        project = apply (project, "setNoteLinks",
                         { { "clipId", rig.clipId },
                           { "links", list ({ makeObject ({ { "from", other }, { "to", rig.secondId } }) }) } });
        const auto& clip = *project.tracks[0]->clips[0];
        CHECK_FALSE (clip.notes[static_cast<size_t> (clip.indexOfNote (rig.firstId))]->link.has_value());
        CHECK (clip.notes[static_cast<size_t> (clip.indexOfNote (other))]->link->to == rig.secondId);
    }

    SECTION ("removing the later note removes the link")
    {
        const auto project =
            apply (rig.project, "removeNotes", { { "clipId", rig.clipId }, { "ids", list ({ rig.secondId }) } });
        CHECK_FALSE (project.tracks[0]->clips[0]->notes[0]->link.has_value());
    }

    SECTION ("unlinking")
    {
        const auto project =
            apply (rig.project, "setNoteLinks",
                   { { "clipId", rig.clipId }, { "links", list ({ makeObject ({ { "from", rig.firstId } }) }) } });
        CHECK_FALSE (project.tracks[0]->clips[0]->notes[0]->link.has_value());
    }

    SECTION ("a split hands the link to the right half")
    {
        const auto project =
            apply (rig.project, "splitNotes",
                   { { "clipId", rig.clipId }, { "ids", list ({ rig.firstId }) }, { "at", ticksPerBeat / 2 } });
        const auto& clip = *project.tracks[0]->clips[0];
        CHECK_FALSE (clip.notes[0]->link.has_value());
        REQUIRE (clip.notes[1]->link.has_value());
        CHECK (clip.notes[1]->link->to == rig.secondId);
    }

    SECTION ("links survive saving")
    {
        std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> states;
        const auto saved = projectfile::toVar (rig.project, { "abc-id", "0.1.0" }, states);
        const auto loaded = projectfile::fromVar (saved, [] (const juce::String&) { return nullptr; });
        CHECK (loaded.project.tracks[0]->clips[0]->notes[0]->link == first.link);
    }
}

TEST_CASE ("pasted notes link to each other by their place in the call", "[links][commands]")
{
    LinkRig rig (noteArgs (0, ticksPerBeat, 60), noteArgs (ticksPerBeat, ticksPerBeat, 62));
    auto first = noteArgs (4 * ticksPerBeat, ticksPerBeat, 64);
    first.getDynamicObject()->setProperty ("link", makeObject ({ { "toIndex", 1 }, { "glideMs", 30.0 } }));
    auto backwards = noteArgs (5 * ticksPerBeat, ticksPerBeat, 66);
    backwards.getDynamicObject()->setProperty ("link", makeObject ({ { "toIndex", 0 }, { "glideMs", 30.0 } }));
    const auto project =
        apply (rig.project, "addNotes", { { "clipId", rig.clipId }, { "notes", list ({ first, backwards }) } });
    const auto& notes = project.tracks[0]->clips[0]->notes;

    REQUIRE (notes.size() == 4);
    REQUIRE (notes[2]->link.has_value());
    CHECK (notes[2]->link->to == notes[3]->id);
    CHECK_THAT (notes[2]->link->glideMs, WithinAbs (30.0, 1e-9));
    CHECK_FALSE (notes[3]->link.has_value()); // to an earlier note: no link
}

TEST_CASE ("a sequence plays a link only when it is valid", "[links][sequence]")
{
    LinkRig rig (noteArgs (0, ticksPerBeat, 60), noteArgs (2 * ticksPerBeat, ticksPerBeat, 72));
    rig.link (20.0);

    const TrackSequence sequence (rig.project.tracks[0]->clips);
    const auto& first = sequence.notes()[0];
    const auto& second = sequence.notes()[1];
    CHECK (first.linkTo == second.id);
    CHECK (second.linkFrom == first.id);
    CHECK (first.holdUntil == second.start); // across the gap
    CHECK_THAT (second.glideSeconds, WithinAbs (0.02, 1e-9));

    // Moving the later note before the earlier one keeps the link, but it doesn't play.
    const auto moved =
        apply (rig.project, "updateNotes",
               { { "clipId", rig.clipId },
                 { "notes", list ({ makeObject ({ { "id", rig.firstId }, { "start", 3 * ticksPerBeat } }) }) } });
    const TrackSequence broken (moved.tracks[0]->clips);

    for (const auto& note : broken.notes())
    {
        CHECK (note.linkTo == 0);
        CHECK (note.linkFrom == 0);
    }
}

TEST_CASE ("a linked note continues the voice: no new attack, and the pitch glides in milliseconds", "[links][engine]")
{
    const auto a = noteArgs (0, ticksPerBeat, 69);
    const auto b = noteArgs (ticksPerBeat, ticksPerBeat, 81);

    LinkRig rig (a, b);
    rig.link (100.0); // 4800 samples
    const auto linked = rig.play (2 * samplesPerBeat);

    // One voice, its envelope still up: the level carries straight on at the hand-over.
    const auto at = static_cast<size_t> (samplesPerBeat);
    const auto before = static_cast<double> (peak (linked, at - 600, at));
    CHECK_THAT (static_cast<double> (peak (linked, at, at + 600)), WithinAbs (before, before * 0.1));

    // It still has A's pitch right after the hand-over, and has B's once the glide is over.
    CHECK_THAT (frequencyOf (linked, at - 2400, at), WithinAbs (440.0, 25.0));
    CHECK_THAT (frequencyOf (linked, at + 4800, at + 12000), WithinAbs (880.0, 25.0));
    const auto middle = frequencyOf (linked, at + 1200, at + 3600);
    CHECK (middle > 500.0);
    CHECK (middle < 820.0);

    // The chain ends with the later note: the voice releases, and the earlier note's end did nothing.
    CHECK (peak (linked, 2 * samplesPerBeat - 100, 2 * samplesPerBeat) > 0.01f);
}

TEST_CASE ("a link holds the voice across a gap", "[links][engine]")
{
    LinkRig rig (noteArgs (0, ticksPerBeat / 2, 69), noteArgs (ticksPerBeat, ticksPerBeat, 69));
    rig.link (0.0);
    const auto output = rig.play (samplesPerBeat + 1000);

    // Where the earlier note has long ended, the synth would be silent.
    CHECK (peak (output, samplesPerBeat - 200, samplesPerBeat) > 0.05f);
    CHECK (peak (output, samplesPerBeat, samplesPerBeat + 200) > 0.05f);
}

TEST_CASE ("pressure, timbre and velocity crossfade over the glide", "[links][voices]")
{
    auto a = noteArgs (0, ticksPerBeat, 60);
    a.getDynamicObject()->setProperty ("velocity", 0.2);
    a.getDynamicObject()->setProperty ("expression", makeObject ({ { "pressure", list ({ list ({ 0, 0.0 }) }) } }));
    auto b = noteArgs (ticksPerBeat, ticksPerBeat, 60);
    b.getDynamicObject()->setProperty ("velocity", 1.0);
    b.getDynamicObject()->setProperty ("expression", makeObject ({ { "pressure", list ({ list ({ 0, 1.0 }) }) } }));

    LinkRig rig (a, b);
    rig.link (100.0);
    const TrackSequence sequence (rig.project.tracks[0]->clips);
    const auto& first = sequence.notes()[0];
    const auto& second = sequence.notes()[1];

    VoiceAllocator voices;
    voices.adoptSequence (sequence);
    NoteEvent on { 0, NoteEventType::noteOn, first.id, &first };
    const auto index = voices.handle (on);
    REQUIRE (index.has_value());

    Transport transport;
    std::ignore = voices.updateExpression (*index, static_cast<double> (ticksPerBeat) - 1.0, transport);

    NoteEvent legato { 0, NoteEventType::legato, second.id, &second };
    legato.fromNoteId = first.id;
    CHECK_FALSE (voices.handle (legato).has_value());
    CHECK (voices.voices()[*index].noteId == second.id);

    const auto map = transport.timeMap.tempoMap();
    const auto tickAfter = [&] (double seconds)
    { return map.tickAt (map.secondsAt (static_cast<double> (second.start)) + seconds); };

    CHECK_THAT (voices.updateExpression (*index, tickAfter (0.05), transport).pressure, WithinAbs (0.5, 0.02));
    CHECK_THAT (static_cast<double> (voices.voices()[*index].velocity), WithinAbs (0.6, 0.02));
    CHECK_THAT (voices.updateExpression (*index, tickAfter (0.2), transport).pressure, WithinAbs (1.0, 1e-6));
    CHECK_THAT (static_cast<double> (voices.voices()[*index].velocity), WithinAbs (1.0, 1e-6));

    // Then a note-off for the later note ends the voice.
    NoteEvent off { 0, NoteEventType::noteOff, second.id, nullptr };
    std::ignore = voices.handle (off);
    CHECK (voices.voices()[*index].state == VoiceState::releasing);
}

TEST_CASE ("a legato with no voice left starts its note", "[links][voices]")
{
    LinkRig rig (noteArgs (0, ticksPerBeat, 60), noteArgs (ticksPerBeat, ticksPerBeat, 72));
    rig.link (20.0);
    const TrackSequence sequence (rig.project.tracks[0]->clips);
    const auto& second = sequence.notes()[1];

    VoiceAllocator voices;
    NoteEvent legato { 0, NoteEventType::legato, second.id, &second };
    legato.fromNoteId = 123;
    CHECK (voices.handle (legato).has_value());
}

TEST_CASE ("a Voice Instrument's envelope doesn't retrigger on a link", "[links][engine]")
{
    const auto a = noteArgs (0, ticksPerBeat, 69);
    const auto b = noteArgs (ticksPerBeat, ticksPerBeat, 69);
    LinkRig rig (a, b);
    rig.link (0.0);

    for (const auto& node : rig.project.tracks[0]->graph.nodes)
        if (node.type == nodetype::mpeSynth)
        {
            rig.project =
                apply (rig.project, "convertToVoiceGraph", { { "trackId", rig.trackId }, { "nodeId", node.id } });
            break;
        }

    const auto output = rig.play (2 * samplesPerBeat);
    // The default ADSR has decayed to its sustain by the hand-over and stays there: no new peak after it.
    const auto before = peak (output, samplesPerBeat - 4800, samplesPerBeat);
    const auto after = peak (output, samplesPerBeat, samplesPerBeat + 4800);
    CHECK_THAT (static_cast<double> (after),
                WithinAbs (static_cast<double> (before), static_cast<double> (before) * 0.1));
}

TEST_CASE ("seeking into a linked note starts it fresh, and a loop wrap ends the chain", "[links][engine]")
{
    LinkRig rig (noteArgs (0, ticksPerBeat, 69), noteArgs (ticksPerBeat, ticksPerBeat, 81));
    rig.link (20.0);
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));
    rig.engine.seek (1.25);
    rig.engine.play();
    const auto output = render (rig.engine, samplesPerBeat / 2).left();

    CHECK_THAT (frequencyOf (output, 2400, 9600), WithinAbs (880.0, 25.0));
}
