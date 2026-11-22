#include "dawg/JsonPatch.h"
#include "dawg/Model.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace dawg;

namespace
{
[[nodiscard]] juce::var json (const char* text)
{
    return juce::JSON::parse (text);
}

[[nodiscard]] juce::String str (const juce::Array<juce::var>& ops)
{
    return juce::JSON::toString (juce::var (ops), true);
}

[[nodiscard]] std::multiset<juce::String> opSet (const juce::Array<juce::var>& ops)
{
    std::multiset<juce::String> result;

    for (const auto& op : ops)
        result.insert (juce::JSON::toString (op, true));

    return result;
}

[[nodiscard]] Project smallProject()
{
    Project project;
    ObjectId id = 1;

    for (int t = 0; t < 3; ++t)
    {
        Track track;
        track.id = id++;

        for (int c = 0; c < 2; ++c)
        {
            NoteClip clip;
            clip.id = id++;

            for (int n = 0; n < 4; ++n)
            {
                Note note;
                note.id = id++;
                note.start = static_cast<Ticks> (n) * 240;
                clip.notes.push_back (std::make_shared<const Note> (note));
            }

            track.clips.push_back (std::make_shared<const NoteClip> (std::move (clip)));
        }

        project.tracks.push_back (std::make_shared<const Track> (std::move (track)));
    }

    Track master;
    master.id = id;
    master.kind = TrackKind::master;
    project.master = std::make_shared<const Track> (master);
    return project;
}

// The project diff skips shared subtrees; it must produce what diffing the serialised projects produces.
void checkSameAsJsonDiff (const Project& before, const Project& after)
{
    CHECK (opSet (diff (before, after)) == opSet (diff (toVar (before), toVar (after))));
}
} // namespace

TEST_CASE ("identical values produce no ops", "[patch]")
{
    CHECK (diff (json (R"({"a":1,"b":[1,2],"c":{"d":"x"}})"), json (R"({"a":1,"b":[1,2],"c":{"d":"x"}})")).isEmpty());
}

TEST_CASE ("changed scalar becomes replace", "[patch]")
{
    CHECK (str (diff (json (R"({"tempo":120})"), json (R"({"tempo":140})")))
           == R"([{"op": "replace", "path": "/tempo", "value": 140}])");
}

TEST_CASE ("added and removed keys", "[patch]")
{
    const auto ops = diff (json (R"({"a":1})"), json (R"({"b":2})"));
    REQUIRE (ops.size() == 2);
    CHECK (ops[0]["op"] == "remove");
    CHECK (ops[0]["path"] == "/a");
    CHECK (ops[1]["op"] == "add");
    CHECK (ops[1]["path"] == "/b");
}

TEST_CASE ("equal-length arrays diff per element, others are replaced", "[patch]")
{
    auto ops = diff (json (R"({"t":[{"n":"a"},{"n":"b"}]})"), json (R"({"t":[{"n":"a"},{"n":"c"}]})"));
    REQUIRE (ops.size() == 1);
    CHECK (ops[0]["path"] == "/t/1/n");

    ops = diff (json (R"({"t":[1]})"), json (R"({"t":[1,2]})"));
    REQUIRE (ops.size() == 1);
    CHECK (ops[0]["op"] == "replace");
    CHECK (ops[0]["path"] == "/t");
}

TEST_CASE ("string vs number is a change", "[patch]")
{
    CHECK (diff (json (R"({"a":"1"})"), json (R"({"a":1})")).size() == 1);
}

TEST_CASE ("object vs scalar is a change", "[patch]")
{
    CHECK (diff (json (R"({"a":{"b":1}})"), json (R"({"a":1})")).size() == 1);
}

TEST_CASE ("pointer segments are escaped", "[patch]")
{
    CHECK (escapePointerSegment ("a/b~c") == "a~1b~0c");
}

TEST_CASE ("the project diff matches the JSON diff", "[patch]")
{
    const auto base = smallProject();

    SECTION ("project fields")
    {
        auto next = base;
        next.tempoMap = TempoMap ({ { 0, 133.0 } }, {});
        checkSameAsJsonDiff (base, next);
    }

    SECTION ("a track field")
    {
        auto next = base;
        auto track = std::make_shared<Track> (*next.tracks[1]);
        track->gainDb = -3.0;
        next.tracks[1] = track;
        checkSameAsJsonDiff (base, next);
        CHECK (diff (base, next).size() == 1);
    }

    SECTION ("notes edited, added and removed")
    {
        auto next = base;
        auto track = std::make_shared<Track> (*next.tracks[0]);
        auto clip = std::make_shared<NoteClip> (*track->clips[1]);
        auto note = std::make_shared<Note> (*clip->notes[2]);
        note->pitch = 72;
        clip->notes[2] = note;
        clip->notes.erase (clip->notes.begin());
        Note added;
        added.id = 900;
        clip->notes.push_back (std::make_shared<const Note> (added));
        track->clips[1] = clip;
        next.tracks[0] = track;
        checkSameAsJsonDiff (base, next);
    }

    SECTION ("clips and tracks added, removed and reordered")
    {
        auto next = base;
        auto track = std::make_shared<Track> (*next.tracks[2]);
        track->clips.erase (track->clips.begin());
        NoteClip clip;
        clip.id = 901;
        track->clips.push_back (std::make_shared<const NoteClip> (clip));
        next.tracks[2] = track;
        std::swap (next.tracks[0], next.tracks[2]);
        next.tracks.erase (next.tracks.begin() + 1);
        Track added;
        added.id = 902;
        next.tracks.push_back (std::make_shared<const Track> (added));
        checkSameAsJsonDiff (base, next);
    }

    SECTION ("the master track")
    {
        auto next = base;
        auto master = std::make_shared<Track> (*next.master);
        master->pan = 0.25;
        next.master = master;
        checkSameAsJsonDiff (base, next);

        next.master = nullptr;
        checkSameAsJsonDiff (base, next);
        checkSameAsJsonDiff (next, base);
    }

    SECTION ("a copied but unchanged track")
    {
        auto next = base;
        next.tracks[1] = std::make_shared<const Track> (*next.tracks[1]);
        CHECK (diff (base, next).isEmpty());
    }
}
