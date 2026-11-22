#include "TestUtils.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"
#include "dawg/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <map>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
[[nodiscard]] juce::var array (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

// A project with a note carrying expression, an instrument plugin with state and custom MIDI settings.
struct SavedProject
{
    SavedProject()
    {
        auto setup = projectWithClip();
        auto note = noteArgs (ticksPerBeat, ticksPerBeat, 60.25);
        note.getDynamicObject()->setProperty ("expression",
                                              makeObject ({ { "pressure", array ({ array ({ 0, 0.5 }) }) } }));
        project = apply (setup.project, "addNotes", { { "clipId", setup.clipId }, { "notes", array ({ note }) } });
        project = apply (project, "setTrackOffset", { { "id", setup.trackId }, { "offsetMs", -12.5 } });
        project = apply (project, "insertNode",
                         { { "trackId", setup.trackId },
                           { "type", nodetype::pluginInstrument },
                           { "plugin", makeObject ({ { "identifier", "test-S" }, { "name", "S" } }) } });
        trackId = setup.trackId;
        pluginId = project.tracks[0]->graph.nodes.back().id;
        project =
            apply (project, "setPluginMidi", { { "trackId", trackId }, { "nodeId", pluginId }, { "bendRange", 24 } });
        project =
            withPluginState (project, trackId, pluginId, std::make_shared<const juce::MemoryBlock> ("\x01\x02\x03", 3));
    }

    [[nodiscard]] const GraphNode& plugin (const Project& p) const
    {
        const auto& graph = p.tracks[0]->graph;
        return graph.nodes[static_cast<size_t> (graph.indexOfNode (pluginId))];
    }

    Project project;
    ObjectId trackId = 0;
    ObjectId pluginId = 0;
};

// A fresh folder in the system temp directory, removed afterwards.
struct TempFolder
{
    TempFolder()
        : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("dawg-test-" + juce::Uuid().toString()))
    {
        REQUIRE (folder.createDirectory());
    }

    ~TempFolder() { std::ignore = folder.deleteRecursively(); }

    TempFolder (const TempFolder&) = delete;
    TempFolder& operator= (const TempFolder&) = delete;
    TempFolder (TempFolder&&) = delete;
    TempFolder& operator= (TempFolder&&) = delete;

    juce::File folder;
};
} // namespace

TEST_CASE ("project files round-trip notes, graphs and plugin state", "[file]")
{
    const SavedProject saved;
    const TempFolder temp;
    const auto file = temp.folder.getChildFile (juce::String ("Song") + projectfile::extension);

    REQUIRE (projectfile::write (saved.project, { "abc-id", "0.1.0" }, file).isEmpty());
    const auto loaded = projectfile::read (file);

    // Same model (the UI form compares everything except plugin state), plus state, ids and metadata.
    CHECK (juce::JSON::toString (toVar (loaded.project)) == juce::JSON::toString (toVar (saved.project)));
    CHECK (loaded.project.nextId == saved.project.nextId);
    CHECK (loaded.metadata.id == "abc-id");
    CHECK (loaded.metadata.appVersion == "0.1.0");

    const auto& node = saved.plugin (loaded.project);
    REQUIRE (node.pluginState != nullptr);
    CHECK (*node.pluginState == juce::MemoryBlock ("\x01\x02\x03", 3));
    CHECK (node.midi.bendRange == 24);
    CHECK_THAT (loaded.project.tracks[0]->offsetMs, WithinAbs (-12.5, 1e-9));
}

TEST_CASE ("version 5 EQs show the bands that could be heard, on the wider frequency ranges", "[file][eq]")
{
    // A new track's EQ: Mid 1 boosted, Mid 2's gain automated, the low shelf cut but switched off, the HP switched
    // on and its frequency automated, the rest flat or off.
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    const auto& graph = project.tracks[0]->graph;
    const auto eqId =
        std::ranges::find_if (graph.nodes, [] (const GraphNode& n) { return n.type == nodetype::eq; })->id;

    for (const auto* param : { "mid2Gain", "hpFreq" })
        project = apply (project, "addAutomationLane",
                         { { "trackId", trackId },
                           { "target", makeObject ({ { "kind", "node" }, { "nodeId", eqId }, { "param", param } }) } });

    auto json = toVar (project);
    const auto& track = json["tracks"][juce::Identifier (juce::String (trackId))];
    auto* node = track["graph"]["nodes"][juce::Identifier (juce::String (eqId))].getDynamicObject();
    REQUIRE (node != nullptr);
    node->setProperty (
        "params",
        makeObject (
            { { "mid1Gain", 6.0 }, { "lowOn", 0.0 }, { "lowGain", -6.0 }, { "hpOn", 1.0 }, { "hpFreq", 100.0 } }));

    // The HP frequency's lane from the middle of its version 5 range (200 Hz) to its top (2 kHz).
    const auto* lanes = track["automation"].getDynamicObject();
    REQUIRE (lanes != nullptr);

    for (const auto& lane : lanes->getProperties())
        if (lane.value["param"].toString() == "hpFreq")
            lane.value.getDynamicObject()->setProperty (
                "points", juce::Array<juce::var> { juce::Array<juce::var> { 0, 0.5, 0, 0.0 },
                                                   juce::Array<juce::var> { 960, 1.0, 0, 0.0 } });

    const auto migrated = projectfile::migrate (makeObject ({ { "format", projectfile::formatName },
                                                              { "version", 5 },
                                                              { "nextId", project.nextId },
                                                              { "project", json } }));
    CHECK (static_cast<int> (migrated["version"]) == projectfile::version);

    const auto loaded = projectfile::fromVar (migrated, [] (const juce::String&) { return nullptr; });
    const auto& migratedTrack = *loaded.project.tracks[0];
    const auto& eq =
        *std::ranges::find_if (migratedTrack.graph.nodes, [&] (const GraphNode& n) { return n.id == eqId; });

    // Audible bands are in use, the bypassed shelf too; the flat bells and the LP that was off are not.
    const std::map<juce::String, double> expected { { "mid1Gain", 6.0 }, { "mid1Used", 1.0 }, { "mid2Used", 1.0 },
                                                    { "lowOn", 0.0 },    { "lowGain", -6.0 }, { "lowUsed", 1.0 },
                                                    { "hpOn", 1.0 },     { "hpFreq", 100.0 }, { "hpUsed", 1.0 } };
    CHECK (eq.params == expected);

    const auto hpLane = std::ranges::find_if (migratedTrack.automation, [] (const AutomationLane& lane)
                                              { return lane.target.param == "hpFreq"; });
    REQUIRE (hpLane != migratedTrack.automation.end());
    REQUIRE (hpLane->points.size() == 2);
    const auto* info = NodeRegistry::builtin().find (nodetype::eq);
    const auto& hpFreq = info->params[static_cast<size_t> (info->indexOfParam ("hpFreq"))];
    CHECK_THAT (paramFromNormalised (hpFreq, hpLane->points[0].value), Catch::Matchers::WithinRel (200.0, 1e-3));
    CHECK_THAT (paramFromNormalised (hpFreq, hpLane->points[1].value), Catch::Matchers::WithinRel (2000.0, 1e-3));
}

TEST_CASE ("version 6 EQ slopes keep their steepness among 6 to 48 dB per octave", "[file][eq]")
{
    // An EQ with its HP at 24 dB/octave and its LP's slope automated from 12 to 24 dB/octave.
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    const auto& graph = project.tracks[0]->graph;
    const auto eqId =
        std::ranges::find_if (graph.nodes, [] (const GraphNode& n) { return n.type == nodetype::eq; })->id;
    project = apply (project, "addAutomationLane",
                     { { "trackId", trackId },
                       { "target", makeObject ({ { "kind", "node" }, { "nodeId", eqId }, { "param", "lpSlope" } }) } });

    auto json = toVar (project);
    const auto& track = json["tracks"][juce::Identifier (juce::String (trackId))];
    auto* node = track["graph"]["nodes"][juce::Identifier (juce::String (eqId))].getDynamicObject();
    REQUIRE (node != nullptr);
    node->setProperty ("params", makeObject ({ { "hpUsed", 1.0 }, { "hpSlope", 1.0 }, { "lpUsed", 1.0 } }));

    const auto* lanes = track["automation"].getDynamicObject();
    REQUIRE (lanes != nullptr);
    REQUIRE (lanes->getProperties().size() == 1);
    lanes->getProperties().getValueAt (0).getDynamicObject()->setProperty (
        "points", juce::Array<juce::var> { juce::Array<juce::var> { 0, 0.0, 0, 0.0 },
                                           juce::Array<juce::var> { 960, 1.0, 0, 0.0 } });

    const auto migrated = projectfile::migrate (makeObject ({ { "format", projectfile::formatName },
                                                              { "version", 6 },
                                                              { "nextId", project.nextId },
                                                              { "project", json } }));
    CHECK (static_cast<int> (migrated["version"]) == projectfile::version);

    const auto loaded = projectfile::fromVar (migrated, [] (const juce::String&) { return nullptr; });
    const auto& migratedTrack = *loaded.project.tracks[0];
    const auto& eq =
        *std::ranges::find_if (migratedTrack.graph.nodes, [&] (const GraphNode& n) { return n.id == eqId; });

    // 24 dB/octave is index 3 now; the LP's unset slope stays unset (12 dB/octave, the default before and now).
    const std::map<juce::String, double> expected { { "hpUsed", 1.0 }, { "hpSlope", 3.0 }, { "lpUsed", 1.0 } };
    CHECK (eq.params == expected);

    const auto* info = NodeRegistry::builtin().find (nodetype::eq);
    const auto& lpSlope = info->params[static_cast<size_t> (info->indexOfParam ("lpSlope"))];
    REQUIRE (migratedTrack.automation.size() == 1);
    const auto& points = migratedTrack.automation[0].points;
    REQUIRE (points.size() == 2);
    CHECK (paramFromNormalised (lpSlope, points[0].value) == 1.0f);
    CHECK (paramFromNormalised (lpSlope, points[1].value) == 3.0f);
}

TEST_CASE ("an LFO's tempo-synced cycle and rate survive saving and loading", "[file][modulation]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::lfo } });
    const auto lfoId = project.tracks[0]->graph.nodes.back().id;

    // A 1/16 cycle (choice 7; the default is 1/4, choice 13) and a free rate that is not the default either.
    for (const auto& [param, value] :
         { std::pair { "sync", 1.0 }, std::pair { "cycle", 7.0 }, std::pair { "rateMs", 125.0 } })
        project = apply (project, "setNodeParam",
                         { { "trackId", trackId }, { "nodeId", lfoId }, { "param", param }, { "value", value } });

    const TempFolder temp;
    const auto file = temp.folder.getChildFile (juce::String ("Lfo") + projectfile::extension);
    REQUIRE (projectfile::write (project, { "id", "v" }, file).isEmpty());
    const auto loaded = projectfile::read (file).project;

    const auto& graph = loaded.tracks[0]->graph;
    const auto& lfo = graph.nodes[static_cast<size_t> (graph.indexOfNode (lfoId))];
    const std::map<juce::String, double> expected { { "sync", 1.0 }, { "cycle", 7.0 }, { "rateMs", 125.0 } };
    CHECK (lfo.params == expected);
}

TEST_CASE ("version 7 LFOs with a cycle in beats get the nearest note length", "[file][modulation]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;

    for (int i = 0; i < 3; ++i)
        project = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::lfo } });

    const auto& nodes = project.tracks[0]->graph.nodes;
    const std::array ids { nodes[nodes.size() - 3].id, nodes[nodes.size() - 2].id, nodes.back().id };

    // As a version 7 file: one LFO from before the note-length cycle at a quarter beat (1/16), one at three beats
    // (1/2.), and one saved since then with both, whose cycle (1/8) wins.
    auto json = toVar (project);
    const auto& graphNodes = json["tracks"][juce::Identifier (juce::String (trackId))]["graph"]["nodes"];
    const std::array params { makeObject ({ { "sync", 1.0 }, { "syncBeats", 0.25 } }),
                              makeObject ({ { "syncBeats", 3.0 } }),
                              makeObject ({ { "syncBeats", 0.25 }, { "cycle", 10.0 } }) };

    for (size_t i = 0; i < ids.size(); ++i)
        graphNodes[juce::Identifier (juce::String (ids[i]))].getDynamicObject()->setProperty ("params", params[i]);

    const auto migrated = projectfile::migrate (makeObject ({ { "format", projectfile::formatName },
                                                              { "version", 7 },
                                                              { "nextId", project.nextId },
                                                              { "project", json } }));
    CHECK (static_cast<int> (migrated["version"]) == projectfile::version);

    const auto loaded = projectfile::fromVar (migrated, [] (const juce::String&) { return nullptr; });
    const auto& graph = loaded.project.tracks[0]->graph;
    const auto paramsOf = [&] (ObjectId id)
    { return graph.nodes[static_cast<size_t> (graph.indexOfNode (id))].params; };

    CHECK (paramsOf (ids[0]) == std::map<juce::String, double> { { "sync", 1.0 }, { "cycle", 7.0 } });
    CHECK (paramsOf (ids[1]) == std::map<juce::String, double> { { "cycle", 18.0 } });
    CHECK (paramsOf (ids[2]) == std::map<juce::String, double> { { "cycle", 10.0 } });
}

TEST_CASE ("version 8 plain MIDI plugins keep their two-semitone bend range", "[file][plugins]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;

    for (int i = 0; i < 3; ++i)
        project = apply (project, "addNode",
                         { { "trackId", trackId },
                           { "type", nodetype::pluginInstrument },
                           { "plugin", makeObject ({ { "identifier", "test-S" }, { "name", "S" } }) } });

    const auto& nodes = project.tracks[0]->graph.nodes;
    const std::array ids { nodes[nodes.size() - 3].id, nodes[nodes.size() - 2].id, nodes.back().id };

    // As a version 8 file: plain MIDI with the old default range, which it ignored; MPE at 24; no settings (older).
    auto json = toVar (project);
    const auto& graphNodes = json["tracks"][juce::Identifier (juce::String (trackId))]["graph"]["nodes"];
    const auto node = [&] (size_t i)
    { return graphNodes[juce::Identifier (juce::String (ids[i]))].getDynamicObject(); };
    node (0)->setProperty ("midi", makeObject ({ { "mpe", false }, { "bendRange", 48 } }));
    node (1)->setProperty ("midi", makeObject ({ { "mpe", true }, { "bendRange", 24 } }));
    node (2)->removeProperty ("midi");

    const auto migrated = projectfile::migrate (makeObject ({ { "format", projectfile::formatName },
                                                              { "version", 8 },
                                                              { "nextId", project.nextId },
                                                              { "project", json } }));
    CHECK (static_cast<int> (migrated["version"]) == projectfile::version);

    const auto loaded = projectfile::fromVar (migrated, [] (const juce::String&) { return nullptr; });
    const auto& graph = loaded.project.tracks[0]->graph;
    const auto midiOf = [&] (ObjectId id) { return graph.nodes[static_cast<size_t> (graph.indexOfNode (id))].midi; };

    CHECK (midiOf (ids[0]) == MidiOutputSettings { false, 2 });
    CHECK (midiOf (ids[1]) == MidiOutputSettings { true, 24 });
    CHECK (midiOf (ids[2]) == MidiOutputSettings { true, 48 });
}

TEST_CASE ("version 9 gain slots on the gain in dB move to the amplitude", "[file][modulation]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    const auto& nodes = project.tracks[0]->graph.nodes;
    const auto gain = std::ranges::find_if (nodes, [] (const GraphNode& n) { return n.type == nodetype::gain; })->id;

    // As a version 9 file: a slot on the gain's dB, as the command allowed then.
    auto json = toVar (project);
    const auto& graphNodes = json["tracks"][juce::Identifier (juce::String (trackId))]["graph"]["nodes"];
    graphNodes[juce::Identifier (juce::String (gain))].getDynamicObject()->setProperty (
        "modulations",
        makeObject ({ { "1", makeObject ({ { "param", "gainDb" }, { "depth", 0.3 }, { "active", true } }) } }));

    const auto migrated = projectfile::migrate (makeObject ({ { "format", projectfile::formatName },
                                                              { "version", 9 },
                                                              { "nextId", project.nextId },
                                                              { "project", json } }));
    CHECK (static_cast<int> (migrated["version"]) == projectfile::version);

    const auto loaded = projectfile::fromVar (migrated, [] (const juce::String&) { return nullptr; });
    const auto& graph = loaded.project.tracks[0]->graph;
    const auto& slots = graph.nodes[static_cast<size_t> (graph.indexOfNode (gain))].modulations;
    REQUIRE (slots.contains (1));
    CHECK (slots.at (1).param == "amplitude");
    CHECK_THAT (slots.at (1).depth, WithinAbs (0.3, 1e-9));
}

TEST_CASE ("the file is a zip with project.json and one entry per plugin state", "[file]")
{
    const SavedProject saved;
    const TempFolder temp;
    const auto file = temp.folder.getChildFile ("Song.dawg");
    REQUIRE (projectfile::write (saved.project, { "id", "v" }, file).isEmpty());

    juce::ZipFile zip (file);
    CHECK (zip.getIndexOfFileName ("project.json") >= 0);
    CHECK (zip.getIndexOfFileName ("plugins/" + juce::String (saved.trackId) + "-" + juce::String (saved.pluginId)
                                   + ".bin")
           >= 0);
}

TEST_CASE ("files from newer versions, too old versions and other files are refused", "[file]")
{
    CHECK_THROWS_AS (projectfile::migrate (makeObject (
                         { { "format", projectfile::formatName }, { "version", projectfile::version + 1 } })),
                     CommandError);
    CHECK_THROWS_AS (projectfile::migrate (makeObject (
                         { { "format", projectfile::formatName }, { "version", projectfile::oldestVersion - 1 } })),
                     CommandError);
    CHECK_THROWS_AS (projectfile::migrate (makeObject ({ { "format", "dawg-project-json" }, { "version", 1 } })),
                     CommandError);
    CHECK_THROWS_AS (projectfile::migrate (makeObject ({ { "format", "something" } })), CommandError);

    const TempFolder temp;
    const auto file = temp.folder.getChildFile ("not.dawg");
    REQUIRE (file.replaceWithText ("hello"));
    CHECK_THROWS_AS (projectfile::read (file), CommandError);
    CHECK_THROWS_AS (projectfile::read (temp.folder.getChildFile ("missing.dawg")), CommandError);
}

TEST_CASE ("saving keeps the last versions as backups", "[file]")
{
    const SavedProject saved;
    const TempFolder temp;
    const auto file = temp.folder.getChildFile ("Song.dawg");
    const auto backups = temp.folder.getChildFile ("Backups");

    REQUIRE (projectfile::write (saved.project, { "id", "v" }, file).isEmpty());
    CHECK_FALSE (backups.exists()); // nothing to back up the first time

    // Backups are named by time to the second, so several saves in one second would share a name: rename each
    // new backup to a unique, older-sorting name. Pruning then has to remove those first.
    for (int i = 0; i < projectfile::backupsKept + 2; ++i)
    {
        auto project = saved.project;
        project.tempoMap = TempoMap ({ { 0, 100.0 + i } }, {});
        REQUIRE (projectfile::write (project, { "id", "v" }, file).isEmpty());
        auto newest = backups.findChildFiles (juce::File::findFiles, false, "Song *.dawg");
        std::ranges::sort (newest,
                           [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });
        REQUIRE_FALSE (newest.isEmpty());
        std::ignore =
            newest.getLast().moveFileTo (backups.getChildFile ("Song 0000-00-" + juce::String (10 + i) + ".dawg"));
    }

    CHECK (backups.getNumberOfChildFiles (juce::File::findFiles, "Song *.dawg") == projectfile::backupsKept);
    CHECK_THAT (projectfile::read (file).project.tempoMap.bpmAt (0.0),
                WithinAbs (100.0 + projectfile::backupsKept + 1, 1e-9));
}

TEST_CASE ("a project summary has tempo, tracks and length", "[file]")
{
    const SavedProject saved;
    const auto summary = projectfile::summarise (saved.project);
    CHECK_THAT (summary.tempo, WithinAbs (120.0, 1e-9));
    CHECK (summary.tracks == 1);
    CHECK_THAT (summary.lengthSeconds, WithinAbs (8.0, 1e-9)); // a 16-beat clip at 120 BPM
}

TEST_CASE ("dirty means different from the saved snapshot", "[file][session]")
{
    Session session;
    CHECK_FALSE (session.isDirty());

    REQUIRE (session.apply ("addTrack", makeObject ({})).ok);
    CHECK (session.isDirty());

    session.markSaved();
    CHECK_FALSE (session.isDirty());

    REQUIRE (session.apply ("setTempo", makeObject ({ { "bpm", 90 } })).ok);
    CHECK (session.isDirty());
    REQUIRE (session.undo().ok);
    CHECK_FALSE (session.isDirty()); // back at the saved state

    session.reset (projectWithClip().project);
    CHECK_FALSE (session.isDirty()); // a freshly opened project is clean
}

TEST_CASE ("resetting a session keeps counting revisions and clears history", "[file][session]")
{
    Session session;
    REQUIRE (session.apply ("addTrack", makeObject ({})).ok);
    const auto revision = session.revision();

    session.reset (projectWithClip().project);
    CHECK (session.revision() == revision + 1);
    CHECK_FALSE (session.canUndo());
    CHECK (session.project()->tracks.size() == 1);
}
