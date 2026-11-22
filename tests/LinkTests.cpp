#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/CompiledEngine.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ParamLinks.h"
#include "dawg/ProjectFile.h"
#include "dawg/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

// Parameter links (docs/design/parameter-linking.md).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
[[nodiscard]] juce::var list (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

[[nodiscard]] const ParamInfo& infoOf (const char* type, const char* id)
{
    const auto* descriptor = NodeRegistry::builtin().find (type);
    REQUIRE (descriptor != nullptr);
    const auto index = descriptor->indexOfParam (id);
    REQUIRE (index >= 0);
    return descriptor->params[static_cast<size_t> (index)];
}

[[nodiscard]] juce::var address (ObjectId trackId, ObjectId nodeId, const char* param = "")
{
    return makeObject ({ { "trackId", trackId }, { "nodeId", nodeId }, { "param", param } });
}

[[nodiscard]] std::vector<ObjectId> nodesOfType (const Track& track, const char* type)
{
    std::vector<ObjectId> ids;

    for (const auto& node : track.graph.nodes)
        if (node.type == type)
            ids.push_back (node.id);

    return ids;
}

[[nodiscard]] double modelValue (const Project& project, ObjectId trackId, ObjectId nodeId, const char* param)
{
    const auto* node = project.findTrackOrMaster (trackId)->findNode (nodeId);
    REQUIRE (node != nullptr);
    const auto it = node->params.find (param);
    return it != node->params.end() ? it->second
                                    : static_cast<double> (infoOf (node->type.toRawUTF8(), param).defaultValue);
}

constexpr int frames = 48000;

// An audio track monitoring its input, with the default EQ and a second EQ after the (bypassed) compressor, so the
// signal goes EQ A -> EQ B -> Gain.
struct Rig
{
    Rig()
    {
        project = apply ({}, "addTrack", { { "kind", "audio" } });
        trackId = project.tracks[0]->id;
        project = apply (project, "setTrackSwitches", { { "id", trackId }, { "inputChannel", 0 }, { "monitor", 2 } });
        project = apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::eq } });
        const auto eqs = nodesOfType (*project.tracks[0], nodetype::eq);
        REQUIRE (eqs.size() == 2);
        eqA = eqs[0];
        eqB = eqs[1];

        std::mt19937 random (7);
        std::uniform_real_distribution<float> noise (-0.25f, 0.25f);
        input.resize (frames);

        for (auto& sample : input)
            sample = noise (random);
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);
    }

    void setA (const char* param, double value)
    {
        edit ("setNodeParam", { { "trackId", trackId }, { "nodeId", eqA }, { "param", param }, { "value", value } });
    }

    [[nodiscard]] StereoBuffer render()
    {
        if (auto compiled = compiler.update (project, spec); compiled != nullptr)
            engine.setEngine (compiled);

        engine.play();
        const std::array<const float*, 1> inputs { input.data() };
        StereoBuffer out (frames);
        engine.renderOffline (out.channels(), 2, frames, sampleRate, inputs.data(), 1);
        return out;
    }

    // The largest difference between the output and the input, in dB of the input's peak.
    [[nodiscard]] double residualDb()
    {
        const auto out = render();
        auto worst = 0.0;

        for (size_t s = 0; s < static_cast<size_t> (frames); ++s)
            worst = std::max (worst, static_cast<double> (std::abs (out.left()[s] - input[s])));

        return 20.0 * std::log10 (std::max (worst, 1.0e-12) / 0.25);
    }

    [[nodiscard]] float liveValue (ObjectId nodeId, const char* param) const
    {
        const auto* node = compiler.findInstance (trackId, nodeId);
        REQUIRE (node != nullptr);
        return node->params().get (static_cast<size_t> (node->descriptor().indexOfParam (param)));
    }

    std::vector<float> input;
    Project project;
    ObjectId trackId = 0;
    ObjectId eqA = 0;
    ObjectId eqB = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

// ---------------------------------------------------------------------------------------------------------
// Mappings (L2, L4)

TEST_CASE ("link mappings work in the parameters' units, octaves or normalised terms", "[links]")
{
    const auto& gain = infoOf (nodetype::eq, "mid1Gain");
    const auto& freq = infoOf (nodetype::eq, "mid1Freq");
    const auto& q = infoOf (nodetype::eq, "mid1Q");

    LinkMapping inverted;
    inverted.kind = LinkMappingKind::inverted;
    CHECK_THAT (mapLinkValue (inverted, &gain, &gain, 6.0f), WithinAbs (-6.0, 1e-5));
    CHECK_THAT (mapLinkValue ({}, &freq, &freq, 3000.0f), WithinAbs (3000.0, 1e-2));
    CHECK (linkDomain (&freq, &freq) == LinkDomain::octaves);
    CHECK (linkDomain (&gain, &q) == LinkDomain::normalised);
    CHECK (linkDomain (&gain, nullptr) == LinkDomain::normalised);

    LinkMapping octaveUp;
    octaveUp.kind = LinkMappingKind::offset;
    octaveUp.amount = 1.0;
    CHECK_THAT (mapLinkValue (octaveUp, &freq, &freq, 1000.0f), WithinAbs (2000.0, 1e-2));
    CHECK_THAT (*invertLinkValue (octaveUp, &freq, &freq, 2000.0f), WithinAbs (1000.0, 1e-2));

    LinkMapping half;
    half.kind = LinkMappingKind::scaled;
    half.amount = 0.5;
    CHECK_THAT (mapLinkValue (half, &gain, &gain, 12.0f), WithinAbs (6.0, 1e-5));
    CHECK_THAT (*invertLinkValue (half, &gain, &gain, 6.0f), WithinAbs (12.0, 1e-5));
    CHECK_THAT (mapLinkValue (half, &gain, &gain, 24.0f), WithinAbs (12.0, 1e-5));

    // Clamped to the target's range.
    LinkMapping up;
    up.kind = LinkMappingKind::offset;
    up.amount = 30.0;
    CHECK_THAT (mapLinkValue (up, &gain, &gain, 6.0f), WithinAbs (24.0, 1e-5));

    // A plugin parameter inverts around the middle of its normalised range.
    CHECK_THAT (mapLinkValue (inverted, nullptr, nullptr, 0.8f), WithinAbs (0.2, 1e-6));

    // A range, falling, to a plugin parameter.
    LinkMapping range;
    range.kind = LinkMappingKind::range;
    range.inMin = 0.5;
    range.inMax = 1.0;
    range.outMin = 1.0;
    range.outMax = 0.0;
    CHECK_THAT (mapLinkValue (range, &gain, nullptr, 0.0f), WithinAbs (1.0, 1e-6));
    CHECK_THAT (mapLinkValue (range, &gain, nullptr, 12.0f), WithinAbs (0.5, 1e-6));
    CHECK_THAT (*invertLinkValue (range, &gain, nullptr, 0.5f), WithinAbs (12.0, 1e-4));

    // A curve that turns back can't be undone; one that only rises can.
    range.curve = { { 0, 0.0 }, { modulationShapeTicks / 2, 1.0 }, { modulationShapeTicks, 0.0 } };
    CHECK_FALSE (isInvertible (range));
    range.curve = { { 0, 0.0 }, { modulationShapeTicks / 2, 0.8 }, { modulationShapeTicks, 1.0 } };
    REQUIRE (isInvertible (range));
    const auto there = mapLinkValue (range, &gain, nullptr, 18.0f);
    CHECK_THAT (*invertLinkValue (range, &gain, nullptr, there), WithinAbs (18.0, 1e-3));

    LinkMapping none;
    none.kind = LinkMappingKind::scaled;
    CHECK_FALSE (invertLinkValue (none, &gain, &gain, 1.0f).has_value());
}

// ---------------------------------------------------------------------------------------------------------
// Commands (L3 to L5, L12 to L14)

TEST_CASE ("a link sets its target in the model and goes when its node goes", "[links][commands]")
{
    Rig rig;
    rig.setA ("mid1Gain", 6.0);
    rig.edit ("addLink", { { "source", address (rig.trackId, rig.eqA, "mid1Gain") },
                           { "target", address (rig.trackId, rig.eqB, "mid1Gain") },
                           { "mapping", makeObject ({ { "kind", "inverted" } }) } });
    REQUIRE (rig.project.links.size() == 1);
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqB, "mid1Gain"), WithinAbs (-6.0, 1e-5));

    rig.setA ("mid1Gain", 9.0);
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqB, "mid1Gain"), WithinAbs (-9.0, 1e-5));

    // Switched off, the target keeps what it has and is free.
    rig.edit ("setLink", { { "id", rig.project.links[0].id }, { "active", false } });
    rig.setA ("mid1Gain", 3.0);
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqB, "mid1Gain"), WithinAbs (-9.0, 1e-5));
    rig.edit ("setLink", { { "id", rig.project.links[0].id }, { "active", true } });
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqB, "mid1Gain"), WithinAbs (-3.0, 1e-5));

    rig.edit ("removeNode", { { "trackId", rig.trackId }, { "nodeId", rig.eqB } });
    CHECK (rig.project.links.empty());
}

TEST_CASE ("a parameter has one driver and links never go round in a circle", "[links][commands]")
{
    Rig rig;
    const auto a = address (rig.trackId, rig.eqA, "mid1Gain");
    const auto b = address (rig.trackId, rig.eqB, "mid1Gain");
    const auto c = address (rig.trackId, rig.eqB, "mid2Gain");
    rig.edit ("addLink", { { "source", a }, { "target", b } });

    CHECK_THROWS_AS (apply (rig.project, "addLink", { { "source", c }, { "target", b } }), CommandError);
    CHECK_THROWS_AS (apply (rig.project, "addLink", { { "source", b }, { "target", a } }), CommandError);
    CHECK_THROWS_AS (apply (rig.project, "addLink", { { "source", a }, { "target", a } }), CommandError);

    // A chain is fine, and closing it isn't.
    rig.edit ("addLink", { { "source", b }, { "target", c } });
    CHECK_THROWS_AS (apply (rig.project, "addLink", { { "source", c }, { "target", a } }), CommandError);

    // Choices and switches only follow as they are.
    CHECK_THROWS_AS (apply (rig.project, "addLink",
                            { { "source", address (rig.trackId, rig.eqA, "mid1Type") },
                              { "target", address (rig.trackId, rig.eqB, "mid1Type") },
                              { "mapping", makeObject ({ { "kind", "inverted" } }) } }),
                     CommandError);
}

TEST_CASE ("editing a target edits its source through the inverse, in one undo step", "[links][commands]")
{
    Session session;
    REQUIRE (session.apply ("addTrack", makeObject ({ { "kind", "audio" } })).ok);
    const auto trackId = session.project()->tracks[0]->id;
    REQUIRE (session.apply ("insertNode", makeObject ({ { "trackId", trackId }, { "type", nodetype::eq } })).ok);
    const auto eqs = nodesOfType (*session.project()->tracks[0], nodetype::eq);
    REQUIRE (session
                 .apply ("addLink", makeObject ({ { "source", address (trackId, eqs[0], "mid1Gain") },
                                                  { "target", address (trackId, eqs[1], "mid1Gain") },
                                                  { "mapping", makeObject ({ { "kind", "inverted" } }) } }))
                 .ok);

    REQUIRE (
        session
            .apply ("setNodeParam",
                    makeObject (
                        { { "trackId", trackId }, { "nodeId", eqs[1] }, { "param", "mid1Gain" }, { "value", 4.0 } }))
            .ok);
    CHECK_THAT (modelValue (*session.project(), trackId, eqs[0], "mid1Gain"), WithinAbs (-4.0, 1e-5));
    CHECK_THAT (modelValue (*session.project(), trackId, eqs[1], "mid1Gain"), WithinAbs (4.0, 1e-5));

    REQUIRE (session.undo().ok);
    CHECK_THAT (modelValue (*session.project(), trackId, eqs[0], "mid1Gain"), WithinAbs (0.0, 1e-5));
    CHECK_THAT (modelValue (*session.project(), trackId, eqs[1], "mid1Gain"), WithinAbs (0.0, 1e-5));
}

TEST_CASE ("linking two EQs mirrors every parameter with the gains inverted", "[links][commands]")
{
    Rig rig;
    rig.setA ("mid1Used", 1.0);
    rig.setA ("mid1Gain", 6.0);
    rig.setA ("mid1Freq", 3000.0);
    rig.setA ("mid1Type", 1.0);
    rig.edit ("linkNodes", { { "source", address (rig.trackId, rig.eqA) },
                             { "target", address (rig.trackId, rig.eqB) },
                             { "preset", "mirrorInverted" } });

    CHECK (rig.project.links.size() == NodeRegistry::builtin().find (nodetype::eq)->params.size());
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqB, "mid1Gain"), WithinAbs (-6.0, 1e-5));
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqB, "mid1Freq"), WithinAbs (3000.0, 1e-2));
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqB, "mid1Type"), WithinAbs (1.0, 1e-9));
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqB, "mid1Used"), WithinAbs (1.0, 1e-9));

    // Both make their cuts as the reciprocals of their boosts, so the pair cancels (section 10).
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqA, "cuts"), WithinAbs (1.0, 1e-9));
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqB, "cuts"), WithinAbs (1.0, 1e-9));

    const auto inverted = std::ranges::count_if (rig.project.links, [] (const ParamLink& l)
                                                 { return l.mapping.kind == LinkMappingKind::inverted; });
    CHECK (inverted == 8); // every band has a gain from -24 to +24 dB

    // Linking again replaces the pair's links.
    rig.edit ("linkNodes", { { "source", address (rig.trackId, rig.eqA) },
                             { "target", address (rig.trackId, rig.eqB) },
                             { "preset", "mirror" } });
    CHECK (rig.project.links.size() == NodeRegistry::builtin().find (nodetype::eq)->params.size());
    CHECK_THAT (modelValue (rig.project, rig.trackId, rig.eqB, "mid1Gain"), WithinAbs (6.0, 1e-5));
}

TEST_CASE ("duplicating a track copies the links between its own devices", "[links][commands]")
{
    Rig rig;
    rig.edit ("addTrack", { { "kind", "audio" } });
    const auto other = rig.project.tracks[1]->id;
    const auto otherEq = nodesOfType (*rig.project.tracks[1], nodetype::eq).front();
    rig.edit ("addLink", { { "source", address (rig.trackId, rig.eqA, "mid1Gain") },
                           { "target", address (rig.trackId, rig.eqB, "mid1Gain") } });
    rig.edit ("addLink", { { "source", address (rig.trackId, rig.eqA, "mid2Gain") },
                           { "target", address (other, otherEq, "mid2Gain") } });

    rig.edit ("duplicateTrack", { { "id", rig.trackId } });
    REQUIRE (rig.project.links.size() == 3);
    const auto& copy = rig.project.links.back();
    const auto copyId = rig.project.tracks[1]->id;
    const auto copiedEqs = nodesOfType (*rig.project.tracks[1], nodetype::eq);
    CHECK (copy.source == ParamAddress { copyId, copiedEqs[0], "mid1Gain" });
    CHECK (copy.target == ParamAddress { copyId, copiedEqs[1], "mid1Gain" });
}

TEST_CASE ("links round-trip through a project file", "[links][file]")
{
    Rig rig;
    rig.edit ("addLink", { { "source", address (rig.trackId, rig.eqA, "mid1Gain") },
                           { "target", address (rig.trackId, rig.eqB, "mid1Gain") },
                           { "mapping", makeObject ({ { "kind", "inverted" }, { "centre", 1.5 } }) } });
    rig.edit ("addLink",
              { { "source", address (rig.trackId, rig.eqA, "mid1Freq") },
                { "target", address (rig.trackId, rig.eqB, "mid1Q") },
                { "mapping", makeObject ({ { "kind", "range" },
                                           { "inMin", 0.2 },
                                           { "outMin", 0.9 },
                                           { "outMax", 0.1 },
                                           { "curve", list ({ list ({ 0, 0.0 }), list ({ 960, 1.0 }) }) } }) } });

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("dawg-links-" + juce::Uuid().toString())
                          .withFileExtension (projectfile::extension);
    REQUIRE (projectfile::write (rig.project, { "id", "0.1.0" }, file).isEmpty());
    const auto loaded = projectfile::read (file);
    std::ignore = file.deleteFile();

    REQUIRE (loaded.project.links.size() == 2);
    CHECK (loaded.project.links == rig.project.links);
    CHECK (loaded.project.nextId == rig.project.nextId);
}

// ---------------------------------------------------------------------------------------------------------
// The engine (L6 to L10)

TEST_CASE ("a de-emphasis EQ follows its automated emphasis as if mirrored by hand", "[links][engine]")
{
    // EQ A's bell at 3 kHz, automated from -12 dB to +12 dB across the render.
    const auto emphasise = [] (Rig& rig)
    {
        rig.setA ("mid1Used", 1.0);
        rig.setA ("mid1Freq", 3000.0);
        rig.setA ("mid1Gain", 9.0);
        rig.edit (
            "addAutomationLane",
            { { "trackId", rig.trackId },
              { "target", makeObject ({ { "kind", "node" }, { "nodeId", rig.eqA }, { "param", "mid1Gain" } }) } });
        rig.edit ("setAutomationPoints",
                  { { "trackId", rig.trackId },
                    { "laneId", rig.project.tracks[0]->automation[0].id },
                    { "points", list ({ list ({ 0, 0.25 }), list ({ 2 * ticksPerBeat, 0.75 }) }) } });
    };

    Rig linked;
    emphasise (linked);
    linked.edit ("linkNodes", { { "source", address (linked.trackId, linked.eqA) },
                                { "target", address (linked.trackId, linked.eqB) },
                                { "preset", "mirrorInverted" } });

    // The same by hand: B set up like A, with a lane of its own that mirrors A's.
    Rig byHand;
    emphasise (byHand);

    for (const auto& [param, value] :
         { std::pair { "mid1Used", 1.0 }, { "mid1Freq", 3000.0 }, { "mid1Gain", -9.0 }, { "cuts", 1.0 } })
        byHand.edit (
            "setNodeParam",
            { { "trackId", byHand.trackId }, { "nodeId", byHand.eqB }, { "param", param }, { "value", value } });

    byHand.setA ("cuts", 1.0); // as linking the pair does

    byHand.edit (
        "addAutomationLane",
        { { "trackId", byHand.trackId },
          { "target", makeObject ({ { "kind", "node" }, { "nodeId", byHand.eqB }, { "param", "mid1Gain" } }) } });
    byHand.edit ("setAutomationPoints",
                 { { "trackId", byHand.trackId },
                   { "laneId", byHand.project.tracks[0]->automation[1].id },
                   { "points", list ({ list ({ 0, 0.75 }), list ({ 2 * ticksPerBeat, 0.25 }) }) } });

    const auto a = linked.render();
    const auto b = byHand.render();
    auto worst = 0.0f;

    for (size_t s = 0; s < static_cast<size_t> (frames); ++s)
        worst = std::max (worst, std::abs (a.left()[s] - b.left()[s]));

    CHECK (worst < 1.0e-6f);
    CHECK_THAT (linked.liveValue (linked.eqB, "mid1Gain"),
                WithinAbs (-linked.liveValue (linked.eqA, "mid1Gain"), 1e-4));

    // The pair cancels the emphasis, its cuts mirroring its boosts; without the link, B stays flat and nothing does.
    const auto cancelled = linked.residualDb();
    CHECK (cancelled < -90.0);

    const auto links = linked.project.links; // edits replace the project

    for (const auto& link : links)
        linked.edit ("setLink", { { "id", link.id }, { "active", false } });

    linked.edit (
        "setNodeParam",
        { { "trackId", linked.trackId }, { "nodeId", linked.eqB }, { "param", "mid1Gain" }, { "value", 0.0 } });
    CHECK (cancelled < linked.residualDb() - 20.0);
}

TEST_CASE ("with cuts mirroring boosts, a boost and a cut of the same band cancel", "[links][engine][eq]")
{
    // EQ A boosts, EQ B cuts as much, with bells, a shelf and a tilt, one high and narrow.
    const auto set = [] (Rig& rig, ObjectId eq, double sign, double cuts)
    {
        const std::initializer_list<std::pair<const char*, double>> values {
            { "mid1Used", 1.0 }, { "mid1Freq", 3000.0 },  { "mid1Gain", 9.0 * sign },  { "mid1Q", 1.4 },
            { "mid2Used", 1.0 }, { "mid2Freq", 18000.0 }, { "mid2Gain", 18.0 * sign }, { "mid2Q", 10.0 },
            { "lowUsed", 1.0 },  { "lowFreq", 120.0 },    { "lowGain", 6.0 * sign },   { "mid3Used", 1.0 },
            { "mid3Type", 6.0 }, { "mid3Freq", 900.0 },   { "mid3Gain", 4.0 * sign },  { "cuts", cuts },
        };

        for (const auto& [param, value] : values)
            rig.edit ("setNodeParam",
                      { { "trackId", rig.trackId }, { "nodeId", eq }, { "param", param }, { "value", value } });
    };

    // Each rig holds an engine, too big for the stack four times over.
    const auto mirrored = std::make_unique<Rig>();
    set (*mirrored, mirrored->eqA, 1.0, 1.0);
    set (*mirrored, mirrored->eqB, -1.0, 1.0);
    CHECK (mirrored->residualDb() < -100.0);

    const auto matched = std::make_unique<Rig>();
    set (*matched, matched->eqA, 1.0, 0.0);
    set (*matched, matched->eqB, -1.0, 0.0);
    CHECK (matched->residualDb() > -60.0);

    // Boosts don't change with the setting.
    const auto boostMatched = std::make_unique<Rig>();
    const auto boostMirrored = std::make_unique<Rig>();
    set (*boostMatched, boostMatched->eqA, 1.0, 0.0);
    set (*boostMirrored, boostMirrored->eqA, 1.0, 1.0);
    const auto a = boostMatched->render();
    const auto b = boostMirrored->render();
    CHECK (std::ranges::equal (a.left(), b.left()));
}

TEST_CASE ("a lane on a linked parameter doesn't play", "[links][engine][automation]")
{
    Rig rig;
    rig.edit ("addLink", { { "source", address (rig.trackId, rig.eqA, "mid1Gain") },
                           { "target", address (rig.trackId, rig.eqB, "mid1Gain") },
                           { "mapping", makeObject ({ { "kind", "inverted" } }) } });
    rig.setA ("mid1Gain", 5.0);
    rig.edit ("addAutomationLane",
              { { "trackId", rig.trackId },
                { "target", makeObject ({ { "kind", "node" }, { "nodeId", rig.eqB }, { "param", "mid1Gain" } }) } });
    rig.edit ("setAutomationPoints", { { "trackId", rig.trackId },
                                       { "laneId", rig.project.tracks[0]->automation[0].id },
                                       { "points", list ({ list ({ 0, 1.0 }) }) } });

    std::ignore = rig.render();
    CHECK_THAT (rig.liveValue (rig.eqB, "mid1Gain"), WithinAbs (-5.0, 1e-4));
}

TEST_CASE ("a link across tracks follows a block later", "[links][engine]")
{
    Rig rig;
    rig.edit ("addTrack", { { "kind", "audio" } });
    const auto other = rig.project.tracks[1]->id;
    const auto otherEq = nodesOfType (*rig.project.tracks[1], nodetype::eq).front();
    rig.edit ("addLink", { { "source", address (rig.trackId, rig.eqA, "mid1Gain") },
                           { "target", address (other, otherEq, "mid1Gain") },
                           { "mapping", makeObject ({ { "kind", "inverted" } }) } });

    // Automation moves the source where the model can't see it.
    rig.edit ("addAutomationLane",
              { { "trackId", rig.trackId },
                { "target", makeObject ({ { "kind", "node" }, { "nodeId", rig.eqA }, { "param", "mid1Gain" } }) } });
    rig.edit ("setAutomationPoints", { { "trackId", rig.trackId },
                                       { "laneId", rig.project.tracks[0]->automation[0].id },
                                       { "points", list ({ list ({ 0, 0.75 }) }) } });
    CHECK_THAT (modelValue (rig.project, other, otherEq, "mid1Gain"), WithinAbs (0.0, 1e-6));

    std::ignore = rig.render();
    const auto* target = rig.compiler.findInstance (other, otherEq);
    REQUIRE (target != nullptr);
    CHECK_THAT (target->params().get (static_cast<size_t> (target->descriptor().indexOfParam ("mid1Gain"))),
                WithinAbs (-12.0, 1e-4));
}

TEST_CASE ("a link follows late by the latency between its ends", "[links][engine]")
{
    Rig rig;
    rig.edit ("insertNode", { { "trackId", rig.trackId }, { "type", nodetype::saturator } });
    rig.edit ("reorderNode", { { "trackId", rig.trackId },
                               { "nodeId", nodesOfType (*rig.project.tracks[0], nodetype::saturator).front() },
                               { "index", 1 } });
    rig.edit ("addLink", { { "source", address (rig.trackId, rig.eqA, "mid1Gain") },
                           { "target", address (rig.trackId, rig.eqB, "mid1Gain") } });

    const auto compiled = rig.compiler.compile (rig.project, spec);
    const CompiledLink* found = nullptr;

    for (const auto& step : compiled->tracks[0].steps)
        for (const auto& link : step.links)
            found = &link;

    REQUIRE (found != nullptr);
    const auto* saturator =
        rig.compiler.findInstance (rig.trackId, nodesOfType (*rig.project.tracks[0], nodetype::saturator).front());
    REQUIRE (saturator != nullptr);
    CHECK (saturator->latencySamples() > 0);
    CHECK (found->delaySamples == saturator->latencySamples());
}

TEST_CASE ("following links allocates nothing on the audio thread", "[links][engine][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    Rig rig;
    rig.edit ("linkNodes", { { "source", address (rig.trackId, rig.eqA) },
                             { "target", address (rig.trackId, rig.eqB) },
                             { "preset", "mirrorInverted" } });
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));
    rig.engine.play();
    StereoBuffer buffer (blockSize);
    rig.engine.renderOffline (buffer.channels(), 2, blockSize, sampleRate); // lazy setup outside the guard

    const auto before = allocationViolationCount();

    {
        const ScopedNoAllocation guard;

        for (int block = 0; block < 16; ++block)
            rig.engine.renderOffline (buffer.channels(), 2, blockSize, sampleRate);
    }

    CHECK (allocationViolationCount() == before);
}
