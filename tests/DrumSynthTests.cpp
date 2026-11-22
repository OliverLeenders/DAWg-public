#include "TestUtils.h"
#include "dawg/DrumSynth.h"
#include "dawg/DrumVoices.h"
#include "dawg/GraphCompiler.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <deque>
#include <vector>

using namespace dawg;
using namespace dawg::test;

// The Drum Synth node (docs/design/drum-synth.md, DR1-DR11, step 3): pads on keys, choke groups, mono, the voice
// pool, outputs, determinism and the file.

namespace
{
struct Hit
{
    int sample = 0;
    int key = 36;
    float velocity = 1.0f;
    NoteEventType type = NoteEventType::noteOn;
};

// The node alone, fed note events block by block, every output port captured.
struct DrumRig
{
    DrumRig()
        : node (NodeRegistry::builtin().create (nodetype::drumSynth))
    {
        REQUIRE (node != nullptr);
        model.type = nodetype::drumSynth;
        model.id = 7;
        model.drumPads = defaultDrumPads();
        node->configure (model);
        node->prepare (spec);
    }

    [[nodiscard]] DrumPad& pad (int key) { return model.drumPads[static_cast<size_t> (key - firstDrumKey)]; }

    void configure() { node->configure (model); }

    void set (const juce::String& id, float value)
    {
        const auto index = node->descriptor().indexOfParam (id);
        REQUIRE (index >= 0);
        node->params().set (static_cast<size_t> (index), value);
    }

    // Renders, returning each port's left channel (and right in `rights`).
    [[nodiscard]] std::vector<std::vector<float>> run (int total, const std::vector<Hit>& hits, juce::uint32 wired = 1,
                                                       int block = blockSize)
    {
        std::vector<std::vector<float>> lefts (maxDrumOutputs, std::vector<float> (static_cast<size_t> (total)));
        rights.assign (maxDrumOutputs, std::vector<float> (static_cast<size_t> (total)));

        for (int offset = 0; offset < total; offset += block)
        {
            const auto length = std::min (block, total - offset);
            EventBuffer events;

            for (const auto& hit : hits)
            {
                if (hit.sample < offset || hit.sample >= offset + length)
                    continue;

                NoteEvent event;
                event.sampleOffset = hit.sample - offset;
                event.type = hit.type;

                if (hit.type == NoteEventType::noteOn)
                {
                    auto& note = notes.emplace_back();
                    note.id = static_cast<ObjectId> (notes.size());
                    note.pitch = hit.key;
                    note.velocity = hit.velocity;
                    event.note = &note;
                    event.noteId = note.id;
                }

                REQUIRE (events.add (event));
            }

            std::vector<AudioOutput> outputs;

            for (size_t port = 0; port < maxDrumOutputs; ++port)
                outputs.push_back ({ lefts[port].data() + offset, rights[port].data() + offset });

            ProcessContext context;
            context.numSamples = length;
            context.eventsIn = &events;
            context.audioOutputs = outputs;
            context.wiredOutputs = wired;
            node->process (context);
        }

        return lefts;
    }

    std::unique_ptr<Node> node;
    GraphNode model;
    std::deque<CompiledNote> notes; // stable addresses for the events
    std::vector<std::vector<float>> rights;
};

[[nodiscard]] int at (double seconds)
{
    return static_cast<int> (seconds * sampleRate);
}

[[nodiscard]] double peak (const std::vector<float>& x, int from = 0, int to = INT_MAX)
{
    double result = 0.0;

    for (auto i = static_cast<size_t> (std::max (0, from)); i < std::min (static_cast<size_t> (to), x.size()); ++i)
        result = std::max (result, static_cast<double> (std::abs (x[i])));

    return result;
}

[[nodiscard]] int voicesOf (const Node& node)
{
    return static_cast<int> (node.meters()["voices"]);
}
} // namespace

TEST_CASE ("Drum Synth: one fixed list of parameters, defaults after the default kit", "[drums][drumsynth]")
{
    const auto& descriptor = drumSynthDescriptor();
    CHECK (juce::String (descriptor.type) == nodetype::drumSynth);
    CHECK (descriptor.category == NodeCategory::instrument);
    CHECK (isInstrument (descriptor));
    CHECK (descriptor.outputs.size() == static_cast<size_t> (maxDrumOutputs));
    CHECK (drumParamsPerPad() == 41);
    CHECK (descriptor.params.size() == drumParamsPerPad() * drumPadCount);

    const auto snappy = descriptor.indexOfParam ("p3.snare.snappy");
    CHECK (snappy == drumParamIndex (2, DrumModel::snare, "snappy"));
    CHECK (juce::String (descriptor.params[static_cast<size_t> (snappy)].name) == "Pad 3 Snare Snappy");
    CHECK (drumParamId (2, DrumModel::snare, "snappy") == "p3.snare.snappy");
    CHECK (drumParamId (0, DrumModel::off, "level") == "p1.level");
    CHECK (descriptor.indexOfParam ("p16.decay") >= 0);
    CHECK (drumParamIndex (0, DrumModel::kick, "nope") == -1);

    // Toms at six tunings; the open hat longer than the closed one.
    const auto value = [&] (const char* id)
    { return descriptor.params[static_cast<size_t> (descriptor.indexOfParam (id))].defaultValue; };
    CHECK (value ("p6.tom.pitch") == 75.0f);
    CHECK (value ("p15.tom.pitch") == 195.0f);
    CHECK (value ("p11.decay") > value ("p7.decay"));

    // The kit's balance: kick and snare at 0 dB, toms 5 dB under, hats and cymbals more than 6 dB under.
    CHECK (value ("p1.level") == 0.0f);
    CHECK (value ("p3.level") == 0.0f);

    for (const auto* tom : { "p6.level", "p8.level", "p10.level", "p12.level", "p13.level", "p15.level" })
        CHECK (value (tom) == -5.0f);

    for (const auto* overhead : { "p7.level", "p9.level", "p11.level", "p14.level", "p16.level" })
        CHECK (value (overhead) < -6.0f);

    const auto pads = defaultDrumPads();
    REQUIRE (pads.size() == static_cast<size_t> (drumPadCount));
    CHECK (pads[0].model == DrumModel::kick);
    CHECK (pads[0].mono);
    CHECK (pads[6].chokeGroup == 1);  // closed hat, key 42
    CHECK (pads[10].chokeGroup == 1); // open hat, key 46
    CHECK (pads[15].model == DrumModel::cymbal);
    CHECK (drumModelFromId ("clap") == DrumModel::clap);
    CHECK_FALSE (drumModelFromId ("cowbell").has_value());

    CHECK_FALSE (nodetype::allowedInVoiceGraph (nodetype::drumSynth));
}

TEST_CASE ("Drum Synth: a key plays its pad; other keys and pads that are off play nothing", "[drums][drumsynth]")
{
    DrumRig rig;
    CHECK (peak (rig.run (at (0.3), { { 0, 38 } })[0]) > 0.1);

    DrumRig outside;
    CHECK (peak (outside.run (at (0.3), { { 0, 35 }, { 10, 52 } })[0]) == 0.0);

    DrumRig off;
    off.pad (38).model = DrumModel::off;
    off.configure();
    CHECK (peak (off.run (at (0.3), { { 0, 38 } })[0]) == 0.0);

    // Note-offs change nothing: a one-shot rings on.
    DrumRig held;
    const auto out = held.run (at (0.3), { { 0, 38 }, { at (0.01), 38, 1.0f, NoteEventType::noteOff } })[0];
    CHECK (peak (out, at (0.05), at (0.1)) > 0.01);
}

TEST_CASE ("Drum Synth: a closed hat chokes the open hat; other groups ring on", "[drums][drumsynth]")
{
    constexpr juce::uint32 wired = 1U | (1U << 1U) | (1U << 2U);
    const auto chokeAt = at (0.1);

    DrumRig rig;
    rig.pad (46).output = 2; // open hat
    rig.pad (42).output = 3; // closed hat
    rig.configure();
    const auto out = rig.run (at (0.3), { { 0, 46 }, { chokeAt, 42 } }, wired);

    CHECK (peak (out[1], chokeAt - at (0.01), chokeAt) > 0.01);
    CHECK (peak (out[1], chokeAt + at (0.005) + 1) == 0.0); // faded out within 5 ms
    CHECK (peak (out[2], chokeAt) > 0.01);

    DrumRig apart;
    apart.pad (46).output = 2;
    apart.pad (42).output = 3;
    apart.pad (46).chokeGroup = 0;
    apart.configure();
    const auto rings = apart.run (at (0.3), { { 0, 46 }, { chokeAt, 42 } }, wired);
    CHECK (peak (rings[1], chokeAt + at (0.01), chokeAt + at (0.05)) > 0.005);
}

TEST_CASE ("Drum Synth: a pad doesn't choke itself unless it is mono", "[drums][drumsynth]")
{
    DrumRig snare; // not mono
    snare.set ("p3.decay", 400.0f);
    std::ignore = snare.run (at (0.06), { { 0, 38 }, { at (0.05), 38 } });
    CHECK (voicesOf (*snare.node) == 2);

    DrumRig kick; // mono by default
    std::ignore = kick.run (at (0.06), { { 0, 36 }, { at (0.05), 36 } });
    CHECK (voicesOf (*kick.node) == 1);
}

TEST_CASE ("Drum Synth: a cymbal's hits strike the one cymbal, which rings on", "[drums][drumsynth]")
{
    DrumRig ride; // key 51
    const auto out = ride.run (at (1.0), { { 0, 51 }, { at (0.25), 51 }, { at (0.5), 51 }, { at (0.75), 51 } })[0];
    CHECK (voicesOf (*ride.node) == 1);
    CHECK (static_cast<int> (ride.node->meters()["hits"][15]) == 4);
    CHECK (peak (out, at (0.24), at (0.25)) > 0.01); // no fade before a restrike

    DrumRig mono;
    mono.pad (51).mono = true;
    mono.configure();
    std::ignore = mono.run (at (0.3), { { 0, 51 }, { at (0.25), 51 } });
    CHECK (voicesOf (*mono.node) == 1); // the first one choked, not restruck
    CHECK (static_cast<int> (mono.node->meters()["hits"][15]) == 2);
}

TEST_CASE ("Drum Synth: at most four voices a pad, 32 in all, and stealing never fails", "[drums][drumsynth]")
{
    DrumRig rig;
    rig.set ("p3.decay", 400.0f);
    std::vector<Hit> hits;

    for (int i = 0; i < 5; ++i)
        hits.push_back ({ at (0.01) * i, 38 });

    std::ignore = rig.run (at (0.06), hits);
    CHECK (voicesOf (*rig.node) == 4);

    // Forty hits on every pad at once: the pool takes them all, and the output stays finite.
    DrumRig busy;
    std::vector<Hit> many;

    for (int i = 0; i < 40; ++i)
        many.push_back ({ i * 50, firstDrumKey + i % drumPadCount });

    const auto out = busy.run (at (0.2), many);
    CHECK (voicesOf (*busy.node) <= 32);
    CHECK (std::ranges::all_of (out[0], [] (float v) { return std::isfinite (v); }));
}

TEST_CASE ("Drum Synth: a pad plays from its output while it's wired, else in Main", "[drums][drumsynth]")
{
    DrumRig rig;
    rig.pad (38).output = 3;
    rig.configure();

    const auto wired = rig.run (at (0.2), { { 0, 38 } }, 1U | (1U << 2U));
    CHECK (peak (wired[0]) == 0.0);
    CHECK (peak (wired[2]) > 0.1);

    DrumRig unwired;
    unwired.pad (38).output = 3;
    unwired.configure();
    const auto main = unwired.run (at (0.2), { { 0, 38 } }, 1U);
    CHECK (peak (main[0]) > 0.1);
}

TEST_CASE ("Drum Synth: the same pattern renders the same at any block size", "[drums][drumsynth]")
{
    const std::vector<Hit> pattern { { 0, 36 },    { 0, 42 },     { 3000, 42 },  { 6000, 38 },  { 6000, 42, 0.5f },
                                     { 9000, 46 }, { 12000, 42 }, { 12000, 36 }, { 15000, 49 }, { 15000, 39 } };

    DrumRig small;
    DrumRig large;
    const auto a = small.run (at (0.6), pattern, 1, 64);
    const auto b = large.run (at (0.6), pattern, 1, 480);
    CHECK (a[0] == b[0]);
    CHECK (small.rights[0] == large.rights[0]);

    // Two hits of a noisy pad differ (each its own seed), and a stop starts the seeds over (DR11).
    DrumRig twice;
    const auto out = twice.run (at (0.4), { { 0, 38 }, { at (0.2), 38 } })[0];
    CHECK (! std::equal (out.begin(), out.begin() + at (0.1), out.begin() + at (0.2)));
}

TEST_CASE ("Drum Synth: a stop fades everything within 5 ms; bypass is silent", "[drums][drumsynth]")
{
    DrumRig rig;
    const auto stop = at (0.1);
    const auto out = rig.run (at (0.3), { { 0, 49 }, { 0, 36 }, { stop, 0, 1.0f, NoteEventType::chokeAll } })[0];
    CHECK (peak (out, stop - at (0.01), stop) > 0.01);
    CHECK (peak (out, stop + at (0.005) + 1) == 0.0);

    DrumRig bypassed;
    bypassed.model.bypassed = true;
    bypassed.configure();
    CHECK (peak (bypassed.run (at (0.2), { { 0, 38 } })[0]) == 0.0);
}

TEST_CASE ("Drum Synth: velocity, parameters and hit counts reach the voices", "[drums][drumsynth]")
{
    DrumRig loud;
    DrumRig soft;
    const auto hard = peak (loud.run (at (0.2), { { 0, 36, 1.0f } })[0]);
    const auto gentle = peak (soft.run (at (0.2), { { 0, 36, 0.25f } })[0]);
    CHECK (gentle < 0.5 * hard);

    DrumRig quiet;
    quiet.set ("p1.level", -20.0f);
    CHECK (peak (quiet.run (at (0.2), { { 0, 36 } })[0]) < 0.2 * hard);

    // Each hit counts on its pad, for the panel's flashes.
    DrumRig rig;
    std::ignore = rig.run (at (0.05), { { 0, 36 }, { 10, 36 }, { 20, 38 } });
    const auto hits = rig.node->meters()["hits"];
    REQUIRE (hits.isArray());
    CHECK (static_cast<int> (hits[0]) == 2);
    CHECK (static_cast<int> (hits[2]) == 1);
    CHECK (static_cast<int> (hits[1]) == 0);
}

TEST_CASE ("Drum Synth: a new node gets the default kit, and pads survive saving", "[drums][drumsynth]")
{
    auto setup = projectWithClip();
    auto project =
        apply (setup.project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::drumSynth } });
    const auto& graph = project.tracks[0]->graph;
    const auto found = std::ranges::find (graph.nodes, juce::String (nodetype::drumSynth), &GraphNode::type);
    REQUIRE (found != graph.nodes.end());
    CHECK (found->drumPads == defaultDrumPads());

    // Change a pad in the model, save and load.
    auto node = *found;
    node.drumPads[2] = { "Fat snare", DrumModel::tom, 5, 3, true, true };
    auto track = *project.tracks[0];
    track.graph.nodes[static_cast<size_t> (track.graph.indexOfNode (node.id))] = node;
    project.tracks[0] = std::make_shared<const Track> (track);

    std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> states;
    const auto saved = projectfile::toVar (project, { "abc-id", "0.1.0" }, states);
    const auto loaded = projectfile::fromVar (saved, [] (const juce::String&) { return nullptr; });
    const auto& loadedGraph = loaded.project.tracks[0]->graph;
    CHECK (loadedGraph.nodes[static_cast<size_t> (loadedGraph.indexOfNode (node.id))].drumPads == node.drumPads);

    // Pads out of range are clamped; a wrong count or an unknown model is refused.
    juce::Array<juce::var> pads;

    for (int i = 0; i < drumPadCount; ++i)
        pads.add (makeObject ({ { "name", "x" }, { "model", "kick" }, { "output", 40 }, { "choke", -3 } }));

    const auto read = projectfile::drumPadsFromVar (pads);
    CHECK (read[0].output == maxDrumOutputs);
    CHECK (read[0].chokeGroup == 0);
    CHECK (projectfile::drumPadsFromVar (juce::var()).empty());

    auto shorter = pads;
    shorter.remove (0);
    CHECK_THROWS_AS (projectfile::drumPadsFromVar (shorter), CommandError);

    auto unknown = pads;
    unknown.set (0, makeObject ({ { "model", "cowbell" } }));
    CHECK_THROWS_AS (projectfile::drumPadsFromVar (unknown), CommandError);
}

TEST_CASE ("Drum Synth: plays in a project, through the compiler", "[drums][drumsynth]")
{
    auto setup = projectWithClip();
    auto project =
        apply (setup.project, "addNotes",
               { { "clipId", setup.clipId }, { "notes", juce::Array<juce::var> { noteArgs (0, 240, 38.0) } } });
    project = apply (project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::drumSynth } });

    GraphCompiler compiler;
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    engine.play();
    const auto out = render (engine, at (0.3)).left();
    CHECK (peak (out) > 0.05);
}

// ---------------------------------------------------------------------------------------------------------
// Commands (step 4: DR4, DR9, DR10, DR14)

namespace
{
// A project whose first track plays a Drum Synth.
struct KitProject
{
    KitProject()
    {
        auto setup = projectWithClip();
        trackId = setup.trackId;
        project = apply (setup.project, "insertNode", { { "trackId", trackId }, { "type", nodetype::drumSynth } });

        for (const auto& node : project.tracks[0]->graph.nodes)
            if (node.type == nodetype::drumSynth)
                nodeId = node.id;

        REQUIRE (nodeId != 0);
    }

    [[nodiscard]] const Track& track() const
    {
        return *project.tracks[static_cast<size_t> (project.indexOfTrack (trackId))];
    }

    [[nodiscard]] static const GraphNode& nodeIn (const Project& p, ObjectId trackId, ObjectId nodeId)
    {
        const auto& graph = p.tracks[static_cast<size_t> (p.indexOfTrack (trackId))]->graph;
        return graph.nodes[static_cast<size_t> (graph.indexOfNode (nodeId))];
    }

    [[nodiscard]] const GraphNode& node() const { return nodeIn (project, trackId, nodeId); }

    [[nodiscard]] Project with (const char* command,
                                std::initializer_list<std::pair<const char*, juce::var>> args) const
    {
        auto object = makeObject (args);
        object.getDynamicObject()->setProperty ("trackId", trackId);
        object.getDynamicObject()->setProperty ("nodeId", nodeId);
        return applyCommand (project, command, object);
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = with (command, args);
    }

    [[nodiscard]] static float defaultOf (const juce::String& id)
    {
        const auto& d = drumSynthDescriptor();
        return d.params[static_cast<size_t> (d.indexOfParam (id))].defaultValue;
    }

    [[nodiscard]] float value (const juce::String& id) const
    {
        if (const auto it = node().params.find (id); it != node().params.end())
            return static_cast<float> (it->second);

        return defaultOf (id);
    }

    Project project;
    ObjectId trackId = 0;
    ObjectId nodeId = 0;
};
} // namespace

TEST_CASE ("Drum Synth commands: setDrumPad edits a pad's fields and keeps every model's settings",
           "[drums][drumsynth]")
{
    KitProject kit;
    kit.edit ("setNodeParam", { { "param", "p3.snare.snappy" }, { "value", 12.0 } });
    kit.edit ("setDrumPad", { { "pad", 2 },
                              { "model", "tom" },
                              { "name", "  Fat tom " },
                              { "output", 4 },
                              { "chokeGroup", 2 },
                              { "mono", true },
                              { "locked", true } });

    CHECK (kit.node().drumPads[2] == DrumPad { "Fat tom", DrumModel::tom, 4, 2, true, true });

    // Back to a snare: its Snappy is as it was.
    kit.edit ("setDrumPad", { { "pad", 2 }, { "model", "snare" } });
    CHECK (kit.node().drumPads[2].model == DrumModel::snare);
    CHECK (kit.value ("p3.snare.snappy") == 12.0f);

    CHECK_THROWS_AS (kit.with ("setDrumPad", { { "pad", 16 }, { "model", "kick" } }), CommandError);
    CHECK_THROWS_AS (kit.with ("setDrumPad", { { "pad", 0 }, { "model", "cowbell" } }), CommandError);
    CHECK_THROWS_AS (kit.with ("setDrumPad", { { "pad", 0 }, { "output", 17 } }), CommandError);
    CHECK_THROWS_AS (kit.with ("setDrumPad", { { "pad", 0 }, { "chokeGroup", 9 } }), CommandError);
    CHECK_THROWS_AS (kit.with ("setDrumPad", { { "model", "kick" } }), CommandError);
}

TEST_CASE ("Drum Synth commands: copyDrumPad copies all but the output and the lock", "[drums][drumsynth]")
{
    KitProject kit;
    kit.edit ("setNodeParam", { { "param", "p1.kick.pitch" }, { "value", 62.0 } });
    kit.edit ("setDrumPad", { { "pad", 12 }, { "output", 5 }, { "locked", true } });
    kit.edit ("copyDrumPad", { { "from", 0 }, { "to", 12 } });

    const auto& to = kit.node().drumPads[12];
    CHECK (to.model == DrumModel::kick);
    CHECK (to.name == "Kick");
    CHECK (to.mono);
    CHECK (to.output == 5);
    CHECK (to.locked);
    CHECK (kit.value ("p13.kick.pitch") == 62.0f);
    // A parameter the source left at its default comes across too, though the two pads' defaults differ.
    CHECK (kit.value ("p13.tom.pitch") == kit.value ("p1.tom.pitch"));
    CHECK (KitProject::defaultOf ("p13.tom.pitch") != KitProject::defaultOf ("p1.tom.pitch"));
}

TEST_CASE ("Drum Synth commands: randomise stays in range, is repeatable and skips locked pads", "[drums][drumsynth]")
{
    KitProject kit;
    kit.edit ("setDrumPad", { { "pad", 2 }, { "locked", true } });
    const auto before = kit.node();

    const auto a = kit.with ("randomiseDrumPads", { { "seed", 42 } });
    const auto b = kit.with ("randomiseDrumPads", { { "seed", 42 } });
    CHECK (KitProject::nodeIn (a, kit.trackId, kit.nodeId).params
           == KitProject::nodeIn (b, kit.trackId, kit.nodeId).params);
    CHECK (KitProject::nodeIn (a, kit.trackId, kit.nodeId).drumPads == before.drumPads);

    kit.project = a;

    for (int pad = 0; pad < drumPadCount; ++pad)
    {
        const auto model = kit.node().drumPads[static_cast<size_t> (pad)].model;
        const auto params = drumModelParams (model);
        const auto ranges = drumModelRandomRanges (model);
        INFO ("pad " << pad);

        for (size_t i = 0; i < params.size(); ++i)
        {
            const auto id = drumParamId (pad, model, params[i].id);
            INFO (id);

            if (pad == 2 || ranges[i].kept())
            {
                CHECK (kit.value (id) == KitProject::defaultOf (id));
            }
            else
            {
                CHECK (kit.value (id) >= ranges[i].low);
                CHECK (kit.value (id) <= ranges[i].high);
            }
        }

        // Level, Pan and Velocity never move.
        for (const auto* id : { "level", "pan", "velocity" })
            CHECK_FALSE (kit.node().params.contains (drumParamId (pad, DrumModel::off, id)));
    }

    // Vary moves a little; a pad named explicitly is varied even when locked.
    KitProject varied;
    varied.edit ("setDrumPad", { { "pad", 0 }, { "locked", true } });
    varied.edit ("randomiseDrumPads", { { "mode", "vary" }, { "pads", juce::Array<juce::var> { 0 } }, { "seed", 3 } });
    const auto pitch = varied.value ("p1.kick.pitch");
    CHECK (pitch != 50.0f);
    CHECK (std::abs (pitch - 50.0f) <= 0.1f * 30.0f + 1.0e-4f);
    CHECK_FALSE (varied.node().params.contains ("p3.snare.pitch"));
    CHECK_THROWS_AS (varied.with ("randomiseDrumPads", { { "mode", "wild" } }), CommandError);
}

TEST_CASE ("Drum Synth commands: each pad to its own track, and back to Main when the track goes", "[drums][drumsynth]")
{
    KitProject kit;
    juce::Array<juce::var> outputs;
    outputs.add (makeObject ({ { "name", "Snare" }, { "pads", juce::Array<juce::var> { 2 } } }));
    outputs.add (makeObject ({ { "name", "Hats" }, { "pads", juce::Array<juce::var> { 6, 8, 10 } } }));
    kit.edit ("splitInstrumentOutputs", { { "outputs", outputs } });

    const auto& pads = kit.node().drumPads;
    CHECK (pads[2].output == 2);
    CHECK (pads[6].output == 3);
    CHECK (pads[10].output == 3);
    CHECK (pads[0].output == 1);

    // Snare, Hats, and a Main output track for the rest (MO12).
    std::vector<const Track*> outs;

    for (const auto& t : kit.project.tracks)
        if (t->kind == TrackKind::output && t->source == kit.trackId)
            outs.push_back (t.get());

    REQUIRE (outs.size() == 3);
    CHECK (std::ranges::count_if (outs, [] (const Track* t) { return t->mainOutput; }) == 1);
    const auto snare = std::ranges::find_if (outs, [] (const Track* t) { return t->name == "Snare"; });
    REQUIRE (snare != outs.end());

    kit.edit ("removeTrack", { { "id", (*snare)->id } });
    CHECK (kit.node().drumPads[2].output == 1);
    CHECK (kit.node().drumPads[6].output == 3);

    juce::Array<juce::var> wrong;
    wrong.add (makeObject ({ { "name", "x" }, { "pads", juce::Array<juce::var> { 16 } } }));
    CHECK_THROWS_AS (kit.with ("splitInstrumentOutputs", { { "outputs", wrong } }), CommandError);
}

TEST_CASE ("Drum Synth commands: a new Step Sequencer takes the pads that play, by name", "[drums][drumsynth]")
{
    KitProject kit;
    kit.edit ("setDrumPad", { { "pad", 1 }, { "model", "off" } });
    kit.edit ("setDrumPad", { { "pad", 2 }, { "name", "Crack" } });
    kit.project =
        apply (kit.project, "insertNode", { { "trackId", kit.trackId }, { "type", nodetype::stepSequencer } });

    const auto& graph = kit.track().graph;
    const auto sequencer = std::ranges::find (graph.nodes, juce::String (nodetype::stepSequencer), &GraphNode::type);
    REQUIRE (sequencer != graph.nodes.end());
    REQUIRE (sequencer->steps != nullptr);
    const auto& rows = sequencer->steps->rows;

    CHECK (rows.size() == 15);
    CHECK (rows.front().pitch == 51); // highest first
    CHECK (rows.front().name == "Ride");
    CHECK (rows.back().pitch == 36);
    CHECK (std::ranges::none_of (rows, [] (const StepRow& r) { return r.pitch == 37; }));
    CHECK (std::ranges::any_of (rows, [] (const StepRow& r) { return r.pitch == 38 && r.name == "Crack"; }));
}
