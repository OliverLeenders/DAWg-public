#include "TestUtils.h"
#include "dawg/ProjectFile.h"
#include "dawg/VarUtils.h"

#include <catch2/catch_test_macros.hpp>

using namespace dawg;
using namespace dawg::test;

// Takes lanes (docs/design/takes-lanes.md): stacked clips, the top one playing.

namespace
{
[[nodiscard]] ClipPtr clip (ObjectId id, Ticks start, Ticks length, std::int64_t order, int lane = 0)
{
    NoteClip c;
    c.id = id;
    c.start = start;
    c.length = length;
    c.order = order;
    c.lane = lane;
    return std::make_shared<const NoteClip> (std::move (c));
}

[[nodiscard]] std::vector<std::tuple<ObjectId, Ticks, Ticks>> spans (const std::vector<ClipPtr>& clips)
{
    std::vector<std::tuple<ObjectId, Ticks, Ticks>> result;
    for (const auto& c : clips)
        result.emplace_back (c->id, c->start, c->start + c->length);
    return result;
}
} // namespace

TEST_CASE ("clips that don't overlap play as they are", "[takes]")
{
    const std::vector<ClipPtr> clips { clip (1, 0, 100, 5), clip (2, 100, 50, 1) };
    const auto played = playedClips (clips);
    REQUIRE (played.size() == 2);
    CHECK (played[0] == clips[0]); // the same clips, not copies
    CHECK (played[1] == clips[1]);
}

TEST_CASE ("where clips stack, the one on top plays", "[takes]")
{
    // A take over the middle of another: the lower one plays around it.
    CHECK (spans (playedClips ({ clip (1, 0, 100, 1), clip (2, 40, 20, 2, 1) }))
           == std::vector<std::tuple<ObjectId, Ticks, Ticks>> { { 1, 0, 40 }, { 2, 40, 60 }, { 1, 60, 100 } });

    // The order decides, not the position or the lane: here the longer clip is on top and the short one is silent.
    CHECK (spans (playedClips ({ clip (1, 0, 100, 2), clip (2, 40, 20, 1, 1) }))
           == std::vector<std::tuple<ObjectId, Ticks, Ticks>> { { 1, 0, 100 } });

    // Three takes: the newest plays where it is, the older ones where nothing newer covers them.
    CHECK (spans (playedClips ({ clip (1, 0, 100, 1, 2), clip (2, 20, 100, 2, 1), clip (3, 50, 20, 3) }))
           == std::vector<std::tuple<ObjectId, Ticks, Ticks>> {
               { 1, 0, 20 }, { 2, 20, 50 }, { 3, 50, 70 }, { 2, 70, 120 } });
}

TEST_CASE ("a part keeps its content in place", "[takes]")
{
    NoteClip looped;
    looped.start = 1000;
    looped.length = 400;
    looped.offset = 50;
    looped.loopStart = 0;
    looped.loopLength = 100;
    looped.fadeIn = 10;
    looped.fadeOut = 10;

    const auto part = clipPart (looped, 1080, 1200);
    CHECK (part.start == 1080);
    CHECK (part.length == 120);
    CHECK (part.offset == 30); // 50 + 80 = 130, wrapped into the 100-tick loop
    CHECK (part.fadeIn == 0);  // both edges are cuts
    CHECK (part.fadeOut == 0);

    const auto head = clipPart (looped, 1000, 1100);
    CHECK (head.fadeIn == 10);
}

TEST_CASE ("a track's stacked clips flatten around the one on top", "[takes]")
{
    // Two clips on one track, the second on top over the first's middle: the first plays around it.
    auto project = projectWithClip().project;
    const auto first = project.tracks[0]->clips[0]->id;

    NoteClip top;
    top.id = project.nextId++;
    top.start = ticksPerBeat;
    top.length = ticksPerBeat;
    top.order = 100;
    top.lane = 1;
    auto edited = *project.tracks[0];
    edited.clips.push_back (std::make_shared<const NoteClip> (std::move (top)));
    edited.lanes = 2;
    project.tracks[0] = std::make_shared<const Track> (std::move (edited));

    const auto played = playedClips (project.tracks[0]->clips);
    REQUIRE (played.size() == 3);
    CHECK (played[0]->id == first);
    CHECK (played[0]->length == ticksPerBeat); // the long clip stops where the top one starts
    CHECK (played[2]->id == first);
    CHECK (played[2]->start == 2 * ticksPerBeat);
}

TEST_CASE ("version 14 loop takes become one clip per lane", "[takes][file]")
{
    // A clip of 3 takes playing the last one, as loop recording left it.
    const auto clipJson = makeObject ({ { "id", 10 },
                                        { "start", 0 },
                                        { "length", 400 },
                                        { "offset", 100 + 2 * 400 + 30 },
                                        { "fileId", 5 },
                                        { "takes", 3 },
                                        { "takeStart", 100 },
                                        { "takeLength", 400 },
                                        { "notes", makeObject ({}) } });
    const auto track = makeObject ({ { "id", 2 }, { "clips", makeObject ({ { "10", clipJson } }) } });
    const auto migrated = projectfile::migrate (
        makeObject ({ { "format", projectfile::formatName },
                      { "version", 14 },
                      { "nextId", 20 },
                      { "project", makeObject ({ { "tracks", makeObject ({ { "2", track } }) } }) } }));
    const auto& migratedTrack = migrated["project"]["tracks"]["2"];
    const auto* clips = migratedTrack["clips"].getDynamicObject();
    REQUIRE (clips != nullptr);
    CHECK (static_cast<int> (migratedTrack["lanes"]) == 3);
    REQUIRE (clips->getProperties().size() == 3);

    std::map<int, juce::var> byLane;
    for (const auto& entry : clips->getProperties())
    {
        CHECK_FALSE (entry.value.hasProperty ("takes"));
        byLane[static_cast<int> (entry.value["lane"])] = entry.value;
    }

    // Lane 0 keeps the take that played, on top; below it the earlier passes, newest first.
    CHECK (static_cast<double> (byLane[0]["offset"]) == 100 + 2 * 400 + 30);
    CHECK (static_cast<double> (byLane[1]["offset"]) == 100 + 1 * 400 + 30);
    CHECK (static_cast<double> (byLane[2]["offset"]) == 100 + 30);
    CHECK (static_cast<double> (byLane[0]["order"]) > static_cast<double> (byLane[1]["order"]));
    CHECK (static_cast<double> (byLane[1]["order"]) > static_cast<double> (byLane[2]["order"]));
    CHECK (static_cast<double> (migrated["nextId"]) > 20);
}

TEST_CASE ("picking a take brings it, or a part of it, to the top", "[takes]")
{
    // Two takes over the same four beats: the newer plays.
    auto project = projectWithClip().project;
    const auto trackId = project.tracks[0]->id;
    const auto older = project.tracks[0]->clips[0]->id;
    project = apply (project, "addClip", { { "trackId", trackId }, { "start", 0 }, { "length", 16 * ticksPerBeat } });
    REQUIRE (project.tracks[0]->lanes == 2);
    const auto newer =
        std::ranges::find_if (project.tracks[0]->clips, [&] (const ClipPtr& c) { return c->id != older; })->get()->id;
    const auto ids = [] (const Project& p)
    {
        std::vector<ObjectId> r;
        for (const auto& c : playedClips (p.tracks[0]->clips))
            r.push_back (c->id);
        return r;
    };
    CHECK (ids (project) == std::vector<ObjectId> { newer });

    SECTION ("a whole clip")
    {
        const auto picked = apply (project, "bringToTop", { { "id", older } });
        CHECK (ids (picked) == std::vector<ObjectId> { older });
        CHECK (picked.tracks[0]->clips.size() == 2); // nothing is cut
    }

    SECTION ("a part: split off and put on top, the rest stays underneath")
    {
        const auto picked = apply (project, "bringToTop",
                                   { { "id", older }, { "from", 4 * ticksPerBeat }, { "to", 8 * ticksPerBeat } });
        const auto played = playedClips (picked.tracks[0]->clips);
        REQUIRE (played.size() == 3);
        CHECK (played[0]->id == newer);
        CHECK (played[1]->start == 4 * ticksPerBeat);
        CHECK (played[1]->length == 4 * ticksPerBeat);
        CHECK (played[2]->id == newer);
        CHECK (picked.tracks[0]->clips.size() == 4); // the older take in three pieces, and the newer one
    }

    SECTION ("flattening keeps what plays, in one lane")
    {
        const auto picked = apply (project, "bringToTop",
                                   { { "id", older }, { "from", 4 * ticksPerBeat }, { "to", 8 * ticksPerBeat } });
        const auto flat = apply (picked, "flattenTakes", { { "trackId", trackId } });
        const auto& track = *flat.tracks[0];
        CHECK (track.lanes == 1);
        REQUIRE (track.clips.size() == 3);
        CHECK (track.clips[0]->start == 0);
        CHECK (track.clips[1]->start == 4 * ticksPerBeat);
        CHECK (track.clips[2]->start == 8 * ticksPerBeat);
        CHECK (std::ranges::all_of (track.clips, [] (const ClipPtr& c) { return c->lane == 0; }));
        CHECK (playedClips (track.clips) == track.clips); // nothing overlaps any more
    }

    SECTION ("removing a lane removes its clips")
    {
        const auto removed = apply (project, "removeLane", { { "trackId", trackId }, { "lane", 0 } });
        CHECK (removed.tracks[0]->lanes == 1);
        REQUIRE (removed.tracks[0]->clips.size() == 1);
        CHECK (removed.tracks[0]->clips[0]->id == older);
    }
}
