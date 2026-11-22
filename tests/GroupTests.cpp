#include "TestUtils.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;

// Group tracks (docs/design/groups-and-sends.md): the model, the commands and the file.

namespace
{
// A project with tracks named "1" … "count", in that order.
[[nodiscard]] Project tracks (int count)
{
    Project project;

    for (int i = 1; i <= count; ++i)
        project = apply (project, "addTrack", { { "name", juce::String (i) } });

    return project;
}

[[nodiscard]] ObjectId idOf (const Project& project, const juce::String& name)
{
    for (const auto& track : project.tracks)
        if (track->name == name)
            return track->id;

    FAIL ("No track named " << name); // [[noreturn]]: the loop above either returns or fails
}

// The tracks as "name/parent-name" in order, e.g. "1/ G/ 2/G".
[[nodiscard]] juce::String layout (const Project& project)
{
    juce::StringArray result;

    for (const auto& track : project.tracks)
    {
        const auto* parent = project.findTrack (track->parent);
        result.add (track->name + "/" + (parent != nullptr ? parent->name : juce::String()));
    }

    return result.joinIntoString (" ");
}

[[nodiscard]] juce::var ids (std::initializer_list<ObjectId> values)
{
    juce::Array<juce::var> result;

    for (const auto id : values)
        result.add (id);

    return result;
}
} // namespace

TEST_CASE ("grouping tracks puts a group where the first one was, holding them", "[groups]")
{
    auto project = tracks (4);
    project = apply (project, "groupTracks",
                     { { "ids", ids ({ idOf (project, "2"), idOf (project, "4") }) }, { "name", "Drums" } });

    CHECK (layout (project) == "1/ Drums/ 2/Drums 4/Drums 3/");

    const auto& group = *project.findTrack (idOf (project, "Drums"));
    CHECK (group.kind == TrackKind::group);
    CHECK (group.clips.empty());
    REQUIRE (group.graph.nodes.size() == 5); // Mix Input -> EQ -> Compressor -> Gain -> Output
    CHECK (group.graph.nodes[0].type == nodetype::mixInput);

    // Groups nest: grouping members of a group makes a group inside it.
    project = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "4") }) }, { "name", "Toms" } });
    CHECK (layout (project) == "1/ Drums/ 2/Drums Toms/Drums 4/Toms 3/");

    // A selected group takes its members along; tracks inside a selected group aren't moved on their own.
    project = apply (
        project, "groupTracks",
        { { "ids", ids ({ idOf (project, "Drums"), idOf (project, "2"), idOf (project, "3") }) }, { "name", "Band" } });
    CHECK (layout (project) == "1/ Band/ Drums/Band 2/Drums Toms/Drums 4/Toms 3/Band");
}

TEST_CASE ("ungrouping moves the members up a level", "[groups]")
{
    auto project = tracks (3);
    project = apply (project, "groupTracks",
                     { { "ids", ids ({ idOf (project, "1"), idOf (project, "2") }) }, { "name", "Outer" } });
    project = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "2") }) }, { "name", "Inner" } });
    project = apply (project, "ungroup", { { "id", idOf (project, "Inner") } });

    CHECK (layout (project) == "Outer/ 1/Outer 2/Outer 3/");
    CHECK_THROWS_AS (apply (project, "ungroup", { { "id", idOf (project, "1") } }), CommandError);
}

TEST_CASE ("tracks move into, out of and within groups", "[groups]")
{
    auto project = tracks (4);
    project = apply (project, "groupTracks",
                     { { "ids", ids ({ idOf (project, "2"), idOf (project, "3") }) }, { "name", "G" } });
    REQUIRE (layout (project) == "1/ G/ 2/G 3/G 4/");

    // Into the group, before one of its members.
    project = apply (project, "moveTracks",
                     { { "ids", ids ({ idOf (project, "4") }) },
                       { "parent", idOf (project, "G") },
                       { "before", idOf (project, "3") } });
    CHECK (layout (project) == "1/ G/ 2/G 4/G 3/G");

    // Out of it, to the end of the top level.
    project = apply (project, "moveTracks", { { "ids", ids ({ idOf (project, "2") }) }, { "parent", 0 } });
    CHECK (layout (project) == "1/ G/ 4/G 3/G 2/");

    // A group moves with its members.
    project = apply (project, "moveTracks",
                     { { "ids", ids ({ idOf (project, "G") }) }, { "parent", 0 }, { "before", idOf (project, "1") } });
    CHECK (layout (project) == "G/ 4/G 3/G 1/ 2/");

    // Not into itself, and only before a track of the destination.
    CHECK_THROWS_AS (
        apply (project, "moveTracks", { { "ids", ids ({ idOf (project, "G") }) }, { "parent", idOf (project, "G") } }),
        CommandError);
    CHECK_THROWS_AS (apply (project, "moveTracks",
                            { { "ids", ids ({ idOf (project, "1") }) },
                              { "parent", idOf (project, "G") },
                              { "before", idOf (project, "2") } }),
                     CommandError);
}

TEST_CASE ("removing a group removes everything inside it", "[groups]")
{
    auto project = tracks (3);
    project = apply (project, "groupTracks",
                     { { "ids", ids ({ idOf (project, "1"), idOf (project, "2") }) }, { "name", "Outer" } });
    project = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "2") }) }, { "name", "Inner" } });
    project = apply (project, "removeTrack", { { "id", idOf (project, "Outer") } });

    CHECK (layout (project) == "3/");
}

TEST_CASE ("duplicating a group copies it with its members", "[groups]")
{
    auto project = tracks (2);
    project = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "1") }) }, { "name", "Outer" } });
    project = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "1") }) }, { "name", "Inner" } });
    project = apply (project, "duplicateTrack", { { "id", idOf (project, "Outer") } });

    REQUIRE (project.tracks.size() == 7);
    CHECK (layout (project) == "Outer/ Inner/Outer 1/Inner Outer/ Inner/Outer 1/Inner 2/");

    // The copies hold the copies.
    CHECK (project.tracks[4]->parent == project.tracks[3]->id);
    CHECK (project.tracks[5]->parent == project.tracks[4]->id);
    CHECK (project.tracks[3]->id != project.tracks[0]->id);
}

TEST_CASE ("tracks and groups can be added inside a group", "[groups]")
{
    auto project = tracks (2);
    project = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "1") }) }, { "name", "G" } });
    project = apply (project, "addTrack", { { "name", "3" }, { "parent", idOf (project, "G") } });
    project =
        apply (project, "addTrack", { { "name", "Sub" }, { "kind", "group" }, { "parent", idOf (project, "G") } });

    CHECK (layout (project) == "G/ 1/G 3/G Sub/G 2/");
    CHECK_THROWS_AS (apply (project, "addTrack", { { "parent", idOf (project, "1") } }), CommandError);
    CHECK_THROWS_AS (
        apply (project, "addClip", { { "trackId", idOf (project, "G") }, { "start", 0 }, { "length", 960 } }),
        CommandError);
}

TEST_CASE ("clips move to the tracks named for them, past groups", "[groups][clips]")
{
    auto project = tracks (2);
    project = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "2") }) }, { "name", "G" } });
    const auto first = idOf (project, "1");
    const auto second = idOf (project, "2");
    project = apply (project, "addClip", { { "trackId", first }, { "start", 0 }, { "length", 960 } });
    const auto clip = project.findTrack (first)->clips[0]->id;

    project = apply (project, "moveClips",
                     { { "ids", ids ({ clip }) },
                       { "delta", 0 },
                       { "trackIds", makeObject ({ { juce::String (clip).toRawUTF8(), second } }) } });

    CHECK (project.findTrack (first)->clips.empty());
    CHECK (project.findTrack (second)->clips.size() == 1);
    CHECK_THROWS_AS (
        apply (project, "moveClips",
               { { "ids", ids ({ clip }) },
                 { "delta", 0 },
                 { "trackIds", makeObject ({ { juce::String (clip).toRawUTF8(), idOf (project, "G") } }) } }),
        CommandError);
}

TEST_CASE ("folding is for groups", "[groups]")
{
    auto project = tracks (1);
    project = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "1") }) }, { "name", "G" } });
    project = apply (project, "setTrackFolded", { { "id", idOf (project, "G") }, { "folded", true } });

    CHECK (project.findTrack (idOf (project, "G"))->folded);
    CHECK_THROWS_AS (apply (project, "setTrackFolded", { { "id", idOf (project, "1") }, { "folded", true } }),
                     CommandError);
}

TEST_CASE ("normalising repairs parents and keeps groups together", "[groups]")
{
    auto project = tracks (4);
    project = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "1") }) }, { "name", "G" } });

    // A member out of place, a parent that isn't a group, and a loop between two groups.
    const auto setParent = [&] (const juce::String& name, ObjectId parent)
    {
        for (auto& track : project.tracks)
        {
            if (track->name == name)
            {
                auto changed = *track;
                changed.parent = parent;
                track = std::make_shared<const Track> (std::move (changed));
            }
        }
    };

    setParent ("4", idOf (project, "G")); // member at the end of the list
    setParent ("3", idOf (project, "2")); // "2" isn't a group
    project = apply (project, "addTrack", { { "name", "H" }, { "kind", "group" } });
    setParent ("H", idOf (project, "G"));
    setParent ("G", idOf (project, "H")); // G and H contain each other

    normaliseGroups (project);
    CHECK (layout (project) == "G/ 1/G 4/G H/G 2/ 3/");
}

TEST_CASE ("groups survive a save and open", "[groups][file]")
{
    auto project = tracks (2);
    project = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "2") }) }, { "name", "G" } });
    project = apply (project, "setTrackFolded", { { "id", idOf (project, "G") }, { "folded", true } });

    const juce::TemporaryFile temporary (juce::String ("Groups") + projectfile::extension);
    REQUIRE (projectfile::write (project, { "id", "v" }, temporary.getFile(), false).isEmpty());
    const auto loaded = projectfile::read (temporary.getFile()).project;

    CHECK (layout (loaded) == "1/ G/ 2/G");
    CHECK (loaded.findTrack (idOf (loaded, "G"))->kind == TrackKind::group);
    CHECK (loaded.findTrack (idOf (loaded, "G"))->folded);
}

// ---------------------------------------------------------------------------------------------------------
// The engine: group buses, mute and solo (R5, R6)

namespace
{
constexpr float level = 0.25f;
constexpr int frames = 2048;
constexpr size_t probe = 100;

// Monitored audio tracks with a constant input, each adding `level` to the mix, and a master track.
struct GroupRig
{
    explicit GroupRig (int count)
        : input (frames, level)
    {
        for (int i = 1; i <= count; ++i)
        {
            project = apply (project, "addTrack", { { "kind", "audio" }, { "name", juce::String (i) } });
            project = apply (project, "setTrackSwitches",
                             { { "id", project.tracks.back()->id }, { "inputChannel", 0 }, { "monitor", 2 } });
        }

        project = withMaster (project);
        engine.setEngine (compiler.compile (project, spec));
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);
        engine.setEngine (compiler.compile (project, spec));
    }

    [[nodiscard]] float render()
    {
        const std::array<const float*, 1> inputs { input.data() };
        StereoBuffer out (frames);
        engine.renderOffline (out.channels(), 2, frames, sampleRate, inputs.data(), 1);
        return out.left()[probe];
    }

    [[nodiscard]] ObjectId id (const juce::String& name) const { return idOf (project, name); }

    std::vector<float> input;
    Project project;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

TEST_CASE ("members are summed on the group's bus, through its chain", "[groups][engine]")
{
    GroupRig rig (3);
    rig.edit ("groupTracks", { { "ids", ids ({ rig.id ("1"), rig.id ("2") }) }, { "name", "G" } });
    CHECK_THAT (rig.render(), Catch::Matchers::WithinAbs (3 * level, 1e-6));

    // The group's fader scales its members only.
    rig.edit ("setTrackGain", { { "id", rig.id ("G") }, { "gainDb", -96 } });
    CHECK_THAT (rig.render(), Catch::Matchers::WithinAbs (level, 1e-4));

    // Nested: an inner group inside G is silenced with it.
    rig.edit ("setTrackGain", { { "id", rig.id ("G") }, { "gainDb", 0 } });
    rig.edit ("groupTracks", { { "ids", ids ({ rig.id ("2") }) }, { "name", "Inner" } });
    CHECK_THAT (rig.render(), Catch::Matchers::WithinAbs (3 * level, 1e-6));

    rig.edit ("setTrackGain", { { "id", rig.id ("Inner") }, { "gainDb", -96 } });
    CHECK_THAT (rig.render(), Catch::Matchers::WithinAbs (2 * level, 1e-4));

    // The group meters its own output.
    rig.edit ("setTrackGain", { { "id", rig.id ("Inner") }, { "gainDb", 0 } });
    std::ignore = rig.render();
    CHECK_THAT (rig.compiler.trackState (rig.id ("G"))->peakLeft.load(), Catch::Matchers::WithinAbs (2 * level, 1e-6));
}

TEST_CASE ("mute and solo follow the groups", "[groups][engine]")
{
    GroupRig rig (3);
    rig.edit ("groupTracks", { { "ids", ids ({ rig.id ("1"), rig.id ("2") }) }, { "name", "G" } });

    // Muting the group silences its members.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("G") }, { "mute", true } });
    CHECK_THAT (rig.render(), Catch::Matchers::WithinAbs (level, 1e-6));
    rig.edit ("setTrackSwitches", { { "id", rig.id ("G") }, { "mute", false } });

    // Soloing a member keeps its group audible, but not the other members or other tracks.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1") }, { "solo", true } });
    CHECK_THAT (rig.render(), Catch::Matchers::WithinAbs (level, 1e-6));
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1") }, { "solo", false } });

    // Soloing the group solos all its members.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("G") }, { "solo", true } });
    CHECK_THAT (rig.render(), Catch::Matchers::WithinAbs (2 * level, 1e-6));
}
