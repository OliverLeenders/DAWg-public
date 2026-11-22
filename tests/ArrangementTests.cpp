#include "TestUtils.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"
#include "dawg/Sequence.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <set>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr Ticks beat = ticksPerBeat;
constexpr int samplesPerBeat = 24000; // 120 BPM at 48 kHz

[[nodiscard]] juce::var array (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

[[nodiscard]] juce::var ids (std::initializer_list<ObjectId> values)
{
    juce::Array<juce::var> result;

    for (const auto id : values)
        result.add (id);

    return result;
}

// One track with one clip [start, start + length) holding one quarter note at each given content beat.
struct Arrangement
{
    Arrangement (Ticks start, Ticks length, std::initializer_list<int> noteBeats)
    {
        project = apply ({}, "addTrack", {});
        trackId = project.tracks[0]->id;

        juce::Array<juce::var> notes;

        for (const auto b : noteBeats)
            notes.add (noteArgs (b * beat, beat, 60 + b));

        project = apply (project, "addClip",
                         { { "trackId", trackId }, { "start", start }, { "length", length }, { "notes", notes } });
        clipId = project.tracks[0]->clips[0]->id;
    }

    [[nodiscard]] const std::vector<ClipPtr>& clips() const { return project.tracks[0]->clips; }

    Project project;
    ObjectId trackId = 0;
    ObjectId clipId = 0;
};

struct Played
{
    Ticks start;
    Ticks end;
    double pitch;
};

[[nodiscard]] std::vector<Played> played (const Track& track)
{
    std::vector<Played> result;

    // What plays: stacked takes top first (docs/design/takes-lanes.md, T8).
    for (const auto& clip : playedClips (track.clips))
        forEachAudibleNote (*clip, [&] (const Note& n, Ticks start, Ticks end, int)
                            { result.push_back ({ start, end, n.pitch }); });

    std::ranges::sort (result, {}, &Played::start);
    return result;
}

[[nodiscard]] std::set<ObjectId> allNoteIds (const Project& project, size_t& count)
{
    std::set<ObjectId> result;
    count = 0;

    for (const auto& track : project.tracks)
        for (const auto& clip : track->clips)
            for (const auto& note : clip->notes)
            {
                result.insert (note->id);
                ++count;
            }

    return result;
}
} // namespace

TEST_CASE ("the content offset hides notes before the left edge", "[arrangement]")
{
    Arrangement a (4 * beat, 4 * beat, { 0, 1, 2, 3 });
    const auto project =
        apply (a.project, "resizeClip", { { "id", a.clipId }, { "edge", "start" }, { "to", 5 * beat } });
    const auto& clip = *project.tracks[0]->clips[0];

    CHECK (clip.start == 5 * beat);
    CHECK (clip.offset == beat);
    CHECK (clip.length == 3 * beat);

    // The notes stay where they were on the timeline; the first one is hidden.
    const auto notes = played (*project.tracks[0]);
    REQUIRE (notes.size() == 3);
    CHECK (notes[0].start == 5 * beat);
    CHECK_THAT (notes[0].pitch, WithinAbs (61.0, 1e-9));

    // Dragging the edge back reveals it again, but never before the content's beginning.
    const auto back = apply (project, "resizeClip", { { "id", a.clipId }, { "edge", "start" }, { "to", 0 } });
    CHECK (back.tracks[0]->clips[0]->start == 4 * beat);
    CHECK (played (*back.tracks[0]).size() == 4);
}

TEST_CASE ("a looped clip repeats its loop range and cuts notes at each repeat", "[arrangement]")
{
    Arrangement a (0, beat, { 0 });
    auto project = apply (
        a.project, "updateNotes",
        { { "clipId", a.clipId },
          { "notes", array ({ makeObject ({ { "id", a.clips()[0]->notes[0]->id }, { "length", 2 * beat } }) }) } });
    project = apply (project, "setClipLoop", { { "id", a.clipId }, { "loopLength", beat } });
    project = apply (project, "resizeClip", { { "id", a.clipId }, { "edge", "end" }, { "to", 3 * beat } });

    const auto notes = played (*project.tracks[0]);
    REQUIRE (notes.size() == 3);

    for (size_t i = 0; i < 3; ++i)
    {
        CHECK (notes[i].start == static_cast<Ticks> (i) * beat);
        CHECK (notes[i].end == static_cast<Ticks> (i + 1) * beat);
    }

    // Each repeat sounds under its own id, the first under the note's own id.
    const TrackSequence sequence (project.tracks[0]->clips);
    std::set<ObjectId> runtimeIds;

    for (const auto& n : sequence.notes())
        runtimeIds.insert (n.id);

    CHECK (runtimeIds.size() == 3);
    CHECK (runtimeIds.contains (a.clips()[0]->notes[0]->id));

    CHECK_THROWS_AS (apply (project, "setClipLoop", { { "id", a.clipId }, { "loopLength", 10 } }), CommandError);
}

TEST_CASE ("a clip placed onto another stacks on top of it", "[arrangement][takes]")
{
    // A6 until docs/design/takes-lanes.md: the covered clip stays whole, in a lane below, and plays around the new one.
    SECTION ("covering the end silences it there")
    {
        Arrangement a (0, 4 * beat, { 0, 3 });
        const auto project =
            apply (a.project, "addClip", { { "trackId", a.trackId }, { "start", 2 * beat }, { "length", 4 * beat } });
        const auto& track = *project.tracks[0];
        REQUIRE (track.clips.size() == 2);
        CHECK (track.clips[0]->length == 4 * beat); // not shortened
        CHECK (track.lanes == 2);
        CHECK (track.clips[1]->lane == 0); // the new clip on the top row
        CHECK (track.clips[0]->lane == 1);
        CHECK (track.clips[1]->order > track.clips[0]->order);
        CHECK (played (track).size() == 1); // the note at 3 is covered
    }

    SECTION ("landing in the middle plays the old clip around the new one")
    {
        Arrangement a (0, 8 * beat, { 0, 3, 5, 7 });
        const auto project =
            apply (a.project, "addClip", { { "trackId", a.trackId }, { "start", 2 * beat }, { "length", 2 * beat } });
        REQUIRE (project.tracks[0]->clips.size() == 2);

        // What remains audible: the note at 0, and 5 and 7; 3 is covered.
        const auto notes = played (*project.tracks[0]);
        REQUIRE (notes.size() == 3);
        CHECK_THAT (notes[1].pitch, WithinAbs (65.0, 1e-9));
    }

    SECTION ("covering it completely silences it, and removing the top clip brings it back")
    {
        Arrangement a (2 * beat, 2 * beat, { 0 });
        const auto project =
            apply (a.project, "addClip", { { "trackId", a.trackId }, { "start", 0 }, { "length", 8 * beat } });
        CHECK (project.tracks[0]->clips.size() == 2);
        CHECK (played (*project.tracks[0]).empty());

        const auto top =
            std::ranges::find_if (project.tracks[0]->clips, [&] (const ClipPtr& c) { return c->id != a.clipId; });
        const auto removed = apply (project, "removeClip", { { "id", (*top)->id } });
        CHECK (played (*removed.tracks[0]).size() == 1);
        CHECK (removed.tracks[0]->lanes == 1); // the empty lane went
    }
}

TEST_CASE ("clips move and copy across tracks keeping their relative positions", "[arrangement]")
{
    Arrangement a (0, 2 * beat, { 0 });
    auto project =
        apply (a.project, "addClip", { { "trackId", a.trackId }, { "start", 4 * beat }, { "length", 2 * beat } });
    project = apply (project, "addTrack", {});
    const auto second = project.tracks[0]->clips[1]->id;

    const auto moved =
        apply (project, "moveClips", { { "ids", ids ({ a.clipId, second }) }, { "delta", beat }, { "trackDelta", 1 } });
    CHECK (moved.tracks[0]->clips.empty());
    REQUIRE (moved.tracks[1]->clips.size() == 2);
    CHECK (moved.tracks[1]->clips[0]->start == beat);
    CHECK (moved.tracks[1]->clips[1]->start == 5 * beat);

    const auto copied = apply (
        project, "moveClips", { { "ids", ids ({ a.clipId }) }, { "delta", 0 }, { "trackDelta", 1 }, { "copy", true } });
    CHECK (copied.tracks[0]->clips.size() == 2);
    REQUIRE (copied.tracks[1]->clips.size() == 1);
    CHECK (copied.tracks[1]->clips[0]->id != a.clipId);
    size_t count = 0;
    const auto unique = allNoteIds (copied, count).size();
    CHECK (unique == count);

    CHECK_THROWS_AS (
        apply (project, "moveClips", { { "ids", ids ({ a.clipId }) }, { "delta", 0 }, { "trackDelta", 5 } }),
        CommandError);
    // Nothing moves before the timeline start: the second clip lands on the first, and stacks on it.
    const auto early = apply (project, "moveClips", { { "ids", ids ({ second }) }, { "delta", -100 * beat } });
    CHECK (early.tracks[0]->clips.size() == 2);
    CHECK (early.tracks[0]->clips[0]->start == 0);
    CHECK (early.tracks[0]->clips[1]->start == 0);
}

TEST_CASE ("duplicating places copies right after the selection", "[arrangement]")
{
    Arrangement a (beat, 2 * beat, { 0 });
    const auto project = apply (a.project, "duplicateClips", { { "ids", ids ({ a.clipId }) } });
    const auto& clips = project.tracks[0]->clips;
    REQUIRE (clips.size() == 2);
    CHECK (clips[1]->start == 3 * beat);
    CHECK (clips[1]->notes[0]->id != clips[0]->notes[0]->id);
}

TEST_CASE ("splitting partitions notes by where they start", "[arrangement]")
{
    Arrangement a (0, 4 * beat, { 0, 1, 2, 3 });
    auto project = apply (
        a.project, "updateNotes",
        { { "clipId", a.clipId },
          { "notes", array ({ makeObject ({ { "id", a.clips()[0]->notes[1]->id }, { "length", 2 * beat } }) }) } });
    project = apply (project, "splitClips", { { "ids", ids ({ a.clipId }) }, { "at", 2 * beat } });

    const auto& clips = project.tracks[0]->clips;
    REQUIRE (clips.size() == 2);
    CHECK (clips[0]->notes.size() == 2);
    CHECK (clips[1]->notes.size() == 2);
    CHECK (clips[1]->offset == 2 * beat);

    // The note crossing the split stays in the first clip and is cut there.
    const auto notes = played (*project.tracks[0]);
    REQUIRE (notes.size() == 4);
    CHECK (notes[1].end == 2 * beat);
    CHECK (notes[2].start == 2 * beat);
}

TEST_CASE ("splitting a looped clip keeps both parts sounding the same", "[arrangement]")
{
    Arrangement a (0, beat, { 0 });
    auto project = apply (a.project, "setClipLoop", { { "id", a.clipId }, { "loopLength", beat } });
    project = apply (project, "resizeClip", { { "id", a.clipId }, { "edge", "end" }, { "to", 4 * beat } });
    const auto before = played (*project.tracks[0]);

    project = apply (project, "splitClips", { { "ids", ids ({ a.clipId }) }, { "at", 2 * beat + beat / 2 } });
    const auto after = played (*project.tracks[0]);

    REQUIRE (project.tracks[0]->clips.size() == 2);
    REQUIRE (after.size() == before.size());

    for (size_t i = 0; i < after.size(); ++i)
        CHECK (after[i].start == before[i].start);
}

TEST_CASE ("gluing merges what was audible into one plain clip", "[arrangement]")
{
    Arrangement a (0, beat, { 0 });
    auto project = apply (a.project, "setClipLoop", { { "id", a.clipId }, { "loopLength", beat } });
    project = apply (project, "resizeClip", { { "id", a.clipId }, { "edge", "end" }, { "to", 2 * beat } });
    project = apply (project, "addClip",
                     { { "trackId", a.trackId },
                       { "start", 3 * beat },
                       { "length", beat },
                       { "notes", array ({ noteArgs (0, beat, 70) }) } });
    const auto second = project.tracks[0]->clips[1]->id;

    const auto glued = apply (project, "glueClips", { { "ids", ids ({ a.clipId, second }) } });
    const auto& clips = glued.tracks[0]->clips;
    REQUIRE (clips.size() == 1);
    CHECK (clips[0]->loopLength == 0);
    CHECK (clips[0]->length == 4 * beat);
    CHECK (played (*glued.tracks[0]).size() == 3);
}

TEST_CASE ("markers and the loop range", "[arrangement]")
{
    auto project = apply ({}, "addMarker", { { "position", 8 * beat } });
    project = apply (project, "addMarker", { { "position", 0 }, { "name", "Intro" } });
    REQUIRE (project.markers.size() == 2);
    CHECK (project.markers[0].name == "Intro");
    CHECK (project.markers[1].name == "Marker 1");

    const auto id = project.markers[1].id;
    project = apply (project, "moveMarker", { { "id", id }, { "position", 0 } });
    project = apply (project, "renameMarker", { { "id", id }, { "name", "Verse" } });
    CHECK (project.markers[0].name != project.markers[1].name);
    project = apply (project, "removeMarker", { { "id", id } });
    CHECK (project.markers.size() == 1);

    project = apply (project, "setLoop", { { "enabled", true }, { "start", 4 * beat }, { "end", 8 * beat } });
    CHECK (project.loop.enabled);
    CHECK_THROWS_AS (apply (project, "setLoop", { { "end", 4 * beat } }), CommandError);
}

TEST_CASE ("clip and arrangement data round-trips through project files", "[arrangement][file]")
{
    Arrangement a (0, beat, { 0 });
    auto project = apply (a.project, "setClipLoop", { { "id", a.clipId }, { "loopLength", beat } });
    project = apply (project, "setClipProps", { { "id", a.clipId }, { "name", "Riff" }, { "colour", "#ff0000" } });
    project = apply (project, "setTrackHeight", { { "id", a.trackId }, { "height", 120 } });
    project = apply (project, "addMarker", { { "position", 4 * beat }, { "name", "Chorus" } });
    project = apply (project, "setLoop", { { "enabled", true }, { "start", 0 }, { "end", 2 * beat } });
    project = apply (project, "duplicateClips", { { "ids", ids ({ a.clipId }) }, { "linked", true } });

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("dawg-arr-" + juce::Uuid().toString() + ".dawg");
    REQUIRE (projectfile::write (project, { "id", "v" }, file, false).isEmpty());
    const auto loaded = projectfile::read (file).project;
    std::ignore = file.deleteFile();

    CHECK (juce::JSON::toString (toVar (loaded)) == juce::JSON::toString (toVar (project)));
}

TEST_CASE ("the transport loop wraps sample-accurately and retriggers notes", "[arrangement][engine]")
{
    Arrangement a (0, 4 * beat, { 0 });
    auto project = apply (
        a.project, "updateNotes",
        { { "clipId", a.clipId },
          { "notes", array ({ makeObject ({ { "id", a.clips()[0]->notes[0]->id }, { "length", beat / 2 } }) }) } });
    const auto synth = std::ranges::find_if (project.tracks[0]->graph.nodes,
                                             [] (const GraphNode& n) { return n.type == nodetype::mpeSynth; })
                           ->id;
    project = apply (project, "setNodeParam",
                     { { "trackId", a.trackId }, { "nodeId", synth }, { "param", "release" }, { "value", 0.005 } });
    project = apply (project, "setLoop", { { "enabled", true }, { "start", 0 }, { "end", beat } });

    GraphCompiler compiler;
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    engine.play();

    const auto output = render (engine, 2 * samplesPerBeat + samplesPerBeat / 2);
    const auto& left = output.left();

    // Silent in the second half of each loop pass, sounding again right after each wrap.
    const auto silentFrom = static_cast<size_t> (samplesPerBeat * 3 / 4);
    CHECK (firstAudible ({ left.begin() + static_cast<std::ptrdiff_t> (silentFrom), left.begin() + samplesPerBeat })
           == -1);
    CHECK (firstAudible ({ left.begin() + samplesPerBeat, left.begin() + samplesPerBeat + 10 }) >= 0);
    const auto twoBeats = std::ptrdiff_t { 2 } * samplesPerBeat;
    CHECK (firstAudible ({ left.begin() + twoBeats, left.begin() + twoBeats + 10 }) >= 0);

    // The playhead stays inside the loop.
    CHECK_THAT (engine.getTelemetry().positionBeats, WithinAbs (0.5, 1e-6));
}

// ---------------------------------------------------------------------------------------------------------
// Linked clips

TEST_CASE ("note edits on a linked clip apply to all its links", "[arrangement][linked]")
{
    Arrangement a (0, beat, { 0 });
    auto project = apply (a.project, "duplicateClips", { { "ids", ids ({ a.clipId }) }, { "linked", true } });
    const auto& clips = project.tracks[0]->clips;
    REQUIRE (clips.size() == 2);
    CHECK (clips[0]->linkId != 0);
    CHECK (clips[0]->linkId == clips[1]->linkId);
    CHECK (clips[0]->notes[0]->id == clips[1]->notes[0]->id);

    const auto copyId = clips[1]->id;
    project = apply (project, "addNotes",
                     { { "clipId", copyId }, { "notes", array ({ noteArgs (beat / 2, beat / 2, 72) }) } });
    CHECK (project.tracks[0]->clips[0]->notes.size() == 2);
    CHECK (project.tracks[0]->clips[1]->notes.size() == 2);

    // Both clips play on one track: every placement sounds under its own runtime id.
    const TrackSequence sequence (project.tracks[0]->clips);
    std::set<ObjectId> runtimeIds;
    for (const auto& n : sequence.notes())
        runtimeIds.insert (n.id);
    CHECK (runtimeIds.size() == 4);

    // Making one unique ends the link for both (a link needs two members).
    project = apply (project, "makeUnique", { { "ids", ids ({ copyId }) } });
    CHECK (project.tracks[0]->clips[0]->linkId == 0);
    CHECK (project.tracks[0]->clips[1]->linkId == 0);
    CHECK (project.tracks[0]->clips[1]->notes[0]->id != project.tracks[0]->clips[0]->notes[0]->id);

    project = apply (project, "removeNotes",
                     { { "clipId", copyId }, { "ids", array ({ project.tracks[0]->clips[1]->notes[0]->id }) } });
    CHECK (project.tracks[0]->clips[0]->notes.size() == 2);
}

TEST_CASE ("linked copies by drag, and splitting a linked clip", "[arrangement][linked]")
{
    Arrangement a (0, 2 * beat, { 0, 1 });
    auto project =
        apply (a.project, "moveClips", { { "ids", ids ({ a.clipId }) }, { "delta", 4 * beat }, { "linked", true } });
    REQUIRE (project.tracks[0]->clips.size() == 2);
    CHECK (project.tracks[0]->clips[1]->linkId == project.tracks[0]->clips[0]->linkId);

    // Splitting keeps the left part linked and makes the right part unique; both sound as before.
    const auto before = played (*project.tracks[0]);
    project = apply (project, "splitClips", { { "ids", ids ({ a.clipId }) }, { "at", beat } });
    const auto& clips = project.tracks[0]->clips;
    REQUIRE (clips.size() == 3);
    CHECK (clips[0]->linkId != 0);
    CHECK (clips[1]->linkId == 0);
    CHECK (played (*project.tracks[0]).size() == before.size());
}

TEST_CASE ("gluing one looped clip unrolls it; tracks can be recoloured", "[arrangement]")
{
    Arrangement a (0, beat, { 0 });
    auto project = apply (a.project, "setClipLoop", { { "id", a.clipId }, { "loopLength", beat } });
    project = apply (project, "resizeClip", { { "id", a.clipId }, { "edge", "end" }, { "to", 3 * beat } });
    project = apply (project, "glueClips", { { "ids", ids ({ a.clipId }) } });
    CHECK (project.tracks[0]->clips[0]->loopLength == 0);
    CHECK (project.tracks[0]->clips[0]->notes.size() == 3);

    project = apply (project, "setTrackColour", { { "id", a.trackId }, { "colour", "#ff6b6b" } });
    CHECK (project.tracks[0]->colour == "#ff6b6b");
    CHECK_THROWS_AS (apply (project, "setTrackColour", { { "id", a.trackId }, { "colour", "red" } }), CommandError);
}
