#include "dawg/Session.h"
#include "dawg/VarUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using Catch::Matchers::WithinAbs;

namespace
{
[[nodiscard]] juce::var tempoArgs (double bpm)
{
    return makeObject ({ { "bpm", bpm } });
}
} // namespace

TEST_CASE ("commands change the project and bump the revision", "[session]")
{
    Session session;
    const auto result = session.apply ("setTempo", tempoArgs (140.0));

    REQUIRE (result.ok);
    CHECK (result.revision == 1);
    CHECK_THAT (session.project()->tempoMap.bpmAt (0.0), WithinAbs (140.0, 1.0e-9));
}

TEST_CASE ("invalid commands are rejected without side effects", "[session]")
{
    Session session;

    CHECK_FALSE (session.apply ("doesNotExist", {}).ok);
    CHECK_FALSE (session.apply ("setTempo", makeObject ({ { "bpm", "fast" } })).ok);
    CHECK_FALSE (session.apply ("setTempo", tempoArgs (std::numeric_limits<double>::quiet_NaN())).ok);
    CHECK_FALSE (session.apply ("removeTrack", makeObject ({ { "id", 42 } })).ok);
    CHECK_FALSE (session.apply ("renameTrack", makeObject ({ { "id", 1 }, { "name", "  " } })).ok);
    CHECK (session.revision() == 0);
    CHECK_FALSE (session.canUndo());
}

TEST_CASE ("values are clamped to limits", "[session]")
{
    Session session;
    REQUIRE (session.apply ("setTempo", tempoArgs (5000.0)).ok);
    CHECK_THAT (session.project()->tempoMap.bpmAt (0.0), WithinAbs (limits::maxTempo, 1.0e-9));
}

TEST_CASE ("undo and redo restore snapshots", "[session]")
{
    Session session;
    REQUIRE (session.apply ("addTrack", makeObject ({ { "name", "Drums" } })).ok);
    REQUIRE (session.apply ("setTempo", tempoArgs (90.0)).ok);
    CHECK (session.undoLabel() == "Set tempo");

    REQUIRE (session.undo().ok);
    CHECK_THAT (session.project()->tempoMap.bpmAt (0.0), WithinAbs (120.0, 1.0e-9));
    CHECK (session.project()->tracks.size() == 1);

    REQUIRE (session.undo().ok);
    CHECK (session.project()->tracks.empty());
    CHECK_FALSE (session.undo().ok);

    REQUIRE (session.redo().ok);
    REQUIRE (session.redo().ok);
    CHECK_THAT (session.project()->tempoMap.bpmAt (0.0), WithinAbs (90.0, 1.0e-9));
    CHECK_FALSE (session.canRedo());
}

TEST_CASE ("a new command clears the redo stack", "[session]")
{
    Session session;
    REQUIRE (session.apply ("setTempo", tempoArgs (100.0)).ok);
    REQUIRE (session.undo().ok);
    REQUIRE (session.apply ("setTempo", tempoArgs (110.0)).ok);
    CHECK_FALSE (session.canRedo());
}

TEST_CASE ("commands within one gesture merge into one undo step", "[session]")
{
    Session session;

    for (const auto bpm : { 121.0, 125.0, 130.0 })
        REQUIRE (session.apply ("setTempo", tempoArgs (bpm), "drag-1").ok);

    REQUIRE (session.apply ("setTempo", tempoArgs (150.0), "drag-2").ok);

    REQUIRE (session.undo().ok);
    CHECK_THAT (session.project()->tempoMap.bpmAt (0.0), WithinAbs (130.0, 1.0e-9));
    REQUIRE (session.undo().ok);
    CHECK_THAT (session.project()->tempoMap.bpmAt (0.0), WithinAbs (120.0, 1.0e-9));
    CHECK_FALSE (session.canUndo());
}

TEST_CASE ("unchanged tracks are shared between snapshots", "[session]")
{
    Session session;
    REQUIRE (session.apply ("addTrack", {}).ok);
    REQUIRE (session.apply ("addTrack", {}).ok);
    const auto before = session.project();

    REQUIRE (session.apply ("renameTrack", makeObject ({ { "id", before->tracks[1]->id }, { "name", "Bass" } })).ok);
    const auto after = session.project();

    CHECK (after->tracks[0] == before->tracks[0]);
    CHECK (after->tracks[1] != before->tracks[1]);
    CHECK (after->tracks[1]->name == "Bass");
}

TEST_CASE ("change listener receives a patch", "[session]")
{
    Session session;
    juce::var lastPatch;
    session.setChangeListener ([&] (const ProjectPtr&, const ProjectPtr&, const juce::var& patch)
                               { lastPatch = patch; });

    REQUIRE (session.apply ("setTempo", tempoArgs (128.0)).ok);

    REQUIRE (lastPatch["ops"].size() == 1);
    CHECK (lastPatch["ops"][0]["path"] == "/tempoMap/0/bpm");
    CHECK_THAT (static_cast<double> (lastPatch["ops"][0]["value"]), WithinAbs (128.0, 1.0e-9));
    CHECK (static_cast<bool> (lastPatch["history"]["canUndo"]));
}
