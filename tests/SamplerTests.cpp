#include "TestUtils.h"
#include "dawg/AudioPlayer.h"
#include "dawg/AudioSource.h"
#include "dawg/EnvelopeHandle.h"
#include "dawg/GraphCompiler.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"
#include "dawg/StretchRenderer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>

// The Sampler (docs/design/sampler-midi-delay-limiter.md, section 4).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr auto sampleFrames = 24000; // half a second, at 48 kHz
constexpr auto samplesPerBeat = 24000;

// A sample that rises from 0 to 1, so its output tells where and how fast it was read.
[[nodiscard]] std::shared_ptr<AudioSource> ramp()
{
    juce::AudioBuffer<float> buffer (1, sampleFrames);

    for (int i = 0; i < sampleFrames; ++i)
        buffer.setSample (0, i, static_cast<float> (i) / static_cast<float> (sampleFrames));

    return AudioSource::fromBuffer (std::move (buffer), 48000.0);
}

[[nodiscard]] juce::var fileArgs (const juce::String& name)
{
    return makeObject ({ { "path", "Audio/" + name + ".wav" },
                         { "name", name },
                         { "sampleRate", 48000.0 },
                         { "channels", 1 },
                         { "frames", sampleFrames } });
}

// A track playing a Sampler instrument, with velocity not affecting the level.
struct SamplerRig
{
    explicit SamplerRig (std::initializer_list<juce::var> notes)
    {
        auto setup = projectWithClip();
        trackId = setup.trackId;
        project = setup.project;
        juce::Array<juce::var> noteList;

        for (const auto& note : notes)
            noteList.add (note);

        project = apply (project, "addNotes", { { "clipId", setup.clipId }, { "notes", noteList } });
        project = apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::sampler } });

        for (const auto& node : project.tracks[0]->graph.nodes)
            if (node.type == nodetype::sampler)
                samplerId = node.id;

        REQUIRE (samplerId != 0);
        edit ("setNodeParam", { { "param", "velocity" }, { "value", 0.0 } });
        compiler.setAudioSourceResolver ([this] (const AudioFile&) { return source; });
    }

    // The same sound through a voice graph.
    void convert()
    {
        const auto zones = sampler().sampleZones;
        project = apply (project, "convertToVoiceGraph", { { "trackId", trackId }, { "nodeId", samplerId } });
        const auto& node =
            project.tracks[0]->graph.nodes[static_cast<size_t> (project.tracks[0]->graph.indexOfNode (samplerId))];
        REQUIRE (node.type == nodetype::voiceInstrument);
        REQUIRE (node.voiceGraph != nullptr);
        REQUIRE (node.sampleZones.empty());

        for (const auto& inner : node.voiceGraph->nodes)
            if (inner.type == nodetype::sampleVoice)
                voiceNode = &inner;

        REQUIRE (voiceNode != nullptr);
        REQUIRE (voiceNode->sampleZones == zones);
    }

    [[nodiscard]] const GraphNode& sampler() const
    {
        const auto& graph = project.tracks[0]->graph;
        return graph.nodes[static_cast<size_t> (graph.indexOfNode (samplerId))];
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        auto object = makeObject (args);
        object.getDynamicObject()->setProperty ("trackId", trackId);
        object.getDynamicObject()->setProperty ("nodeId", samplerId);
        project = applyCommand (project, command, object);
    }

    void addFile (const juce::String& name, const juce::var& key = {})
    {
        juce::Array<juce::var> files;
        files.add (fileArgs (name));

        if (key.isVoid())
            edit ("addSampleZones", { { "files", files } });
        else
            edit ("addSampleZones", { { "files", files }, { "key", key } });
    }

    [[nodiscard]] std::vector<float> play (int numSamples)
    {
        engine.setEngine (compiler.compile (project, spec));
        engine.play();
        return render (engine, numSamples).left();
    }

    std::shared_ptr<AudioSource> source = ramp();
    Project project;
    ObjectId trackId = 0;
    ObjectId samplerId = 0;
    const GraphNode* voiceNode = nullptr;
    GraphCompiler compiler;
    AudioEngine engine;
};

[[nodiscard]] float peak (const std::vector<float>& samples, size_t from, size_t to)
{
    float result = 0.0f;

    for (auto i = from; i < std::min (to, samples.size()); ++i)
        result = std::max (result, std::abs (samples[i]));

    return result;
}

[[nodiscard]] double slopeAt (const std::vector<float>& samples, size_t at)
{
    return static_cast<double> (samples[at + 1000] - samples[at]) / 1000.0;
}

constexpr double ownSlope = 1.0 / sampleFrames;
} // namespace

TEST_CASE ("a Sampler plays its sample at its own pitch on the root key, an octave up at twice the speed", "[sampler]")
{
    SamplerRig root ({ noteArgs (0, 4 * ticksPerBeat, 60) });
    root.addFile ("ramp");
    const auto atRoot = root.play (samplesPerBeat / 2);
    // The ramp's gain at the centre pan is 1 in each channel.
    CHECK_THAT (slopeAt (atRoot, 2000), WithinAbs (ownSlope, ownSlope * 0.01));

    SamplerRig octave ({ noteArgs (0, 4 * ticksPerBeat, 72) });
    octave.addFile ("ramp");
    CHECK_THAT (slopeAt (octave.play (samplesPerBeat / 2), 2000), WithinAbs (2.0 * ownSlope, ownSlope * 0.02));
}

TEST_CASE ("a Sampler follows a note's pitch curve", "[sampler]")
{
    juce::Array<juce::var> point;
    point.add (0);
    point.add (12.0); // an octave up
    juce::Array<juce::var> curve;
    curve.add (point);
    auto note = noteArgs (0, 4 * ticksPerBeat, 60);
    note.getDynamicObject()->setProperty ("expression", makeObject ({ { "pitch", curve } }));

    SamplerRig rig ({ note });
    rig.addFile ("ramp");
    CHECK_THAT (slopeAt (rig.play (samplesPerBeat / 2), 2000), WithinAbs (2.0 * ownSlope, ownSlope * 0.02));
}

TEST_CASE ("a one-shot plays to its end whatever the note's length, then stops", "[sampler]")
{
    SamplerRig rig ({ noteArgs (0, ticksPerBeat / 8, 60) });
    rig.addFile ("ramp");
    const auto output = rig.play (2 * samplesPerBeat);

    CHECK (peak (output, sampleFrames - 100, sampleFrames - 1) > 0.9f);
    CHECK (peak (output, sampleFrames + 100, output.size()) == 0.0f);
}

TEST_CASE ("a zone plays only the keys and velocities in its range", "[sampler]")
{
    SamplerRig rig ({ noteArgs (0, ticksPerBeat, 37), noteArgs (samplesPerBeat * 0, ticksPerBeat, 50) });
    rig.addFile ("ramp", 36);
    CHECK (peak (rig.play (samplesPerBeat / 2), 0, samplesPerBeat / 2) == 0.0f);

    SamplerRig hit ({ noteArgs (0, ticksPerBeat, 36) });
    hit.addFile ("ramp", 36);
    hit.edit ("setSampleZone", { { "zoneId", hit.sampler().sampleZones[0].id }, { "lowVelocity", 127 } });
    CHECK (peak (hit.play (samplesPerBeat / 2), 0, samplesPerBeat / 2) == 0.0f);
}

TEST_CASE ("a looping zone keeps playing while the note is held and fades after it", "[sampler]")
{
    SamplerRig rig ({ noteArgs (0, 2 * ticksPerBeat, 60) });
    rig.addFile ("ramp");
    rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id },
                                 { "mode", "loop" },
                                 { "loopStart", sampleFrames / 2 },
                                 { "crossfade", 1000 } });
    rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id }, { "releaseMs", 10.0 } });
    const auto output = rig.play (3 * samplesPerBeat);

    // Past the file's end, within the loop: the ramp's upper half.
    CHECK (peak (output, samplesPerBeat + 4000, 2 * samplesPerBeat - 100) > 0.9f);
    CHECK (peak (output, 2 * samplesPerBeat + 1500, output.size()) == 0.0f);
}

TEST_CASE ("a zone on a key goes before a zone over the whole keyboard, and a sample can replace another", "[sampler]")
{
    SamplerRig rig ({});
    rig.addFile ("whole");
    rig.addFile ("kick", 36);
    REQUIRE (rig.sampler().sampleZones.size() == 2);
    CHECK (rig.sampler().sampleZones[0].lowKey == 36);
    CHECK (rig.sampler().sampleZones[1].lowKey == 0);

    const auto kick = rig.sampler().sampleZones[0];
    rig.edit ("setSampleZone", { { "zoneId", kick.id }, { "start", 100 }, { "tune", 2.0 } });
    juce::Array<juce::var> files;
    files.add (fileArgs ("snare"));
    rig.edit ("addSampleZones", { { "files", files }, { "key", 36 }, { "replaceZone", kick.id } });
    REQUIRE (rig.sampler().sampleZones.size() == 2);
    const auto& replaced = rig.sampler().sampleZones[0];
    CHECK (replaced.id == kick.id);
    CHECK (replaced.fileId != kick.fileId);
    CHECK (replaced.start == 0);                       // the old sample's range doesn't fit the new one
    CHECK_THAT (replaced.tune, WithinAbs (2.0, 1e-9)); // the sound's settings stay
}

TEST_CASE ("a loop's crossfade leaves no step at the jump", "[sampler]")
{
    // The ramp's jump from 1 back to 0.5 would step by 0.5; the crossfade moves through it.
    SamplerRig rig ({ noteArgs (0, 4 * ticksPerBeat, 60) });
    rig.addFile ("ramp");
    rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id },
                                 { "mode", "loop" },
                                 { "loopStart", sampleFrames / 2 },
                                 { "crossfade", 2000 } });
    const auto output = rig.play (samplesPerBeat * 3);
    float largest = 0.0f;

    for (size_t i = 1; i < output.size(); ++i)
        largest = std::max (largest, std::abs (output[i] - output[i - 1]));

    CHECK (largest < 0.01f);
}

TEST_CASE ("a note in a choke group stops the group's other notes", "[sampler]")
{
    const auto second = samplesPerBeat / 4;
    const auto beatQuarter = ticksPerBeat / 4;
    const auto output = [&] (int group)
    {
        SamplerRig rig ({ noteArgs (0, ticksPerBeat / 8, 36), noteArgs (beatQuarter, ticksPerBeat / 8, 38) });
        rig.addFile ("ramp", 36);
        rig.addFile ("ramp", 38);

        // Each edit replaces the project, so the ids are taken first.
        std::vector<ObjectId> zoneIds;

        for (const auto& zone : rig.sampler().sampleZones)
            zoneIds.push_back (zone.id);

        for (const auto id : zoneIds)
            rig.edit ("setSampleZone", { { "zoneId", id }, { "chokeGroup", group } });

        return rig.play (samplesPerBeat / 2);
    };

    // The ramp starts over with the second note: with no group the first plays on above it.
    const auto at = static_cast<size_t> (second) + 2000;
    CHECK (output (0)[at] > 0.3f);
    CHECK (output (1)[at] < 0.1f);
}

TEST_CASE ("a zone's envelope shapes it, and a one-shot never releases", "[sampler]")
{
    const auto play = [] (const char* mode, double sustain)
    {
        SamplerRig rig ({ noteArgs (0, ticksPerBeat / 8, 60) }); // ends at 3000 samples
        rig.addFile ("ramp");
        rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id },
                                     { "mode", mode },
                                     { "attackMs", 10.0 },
                                     { "decayMs", 10.0 },
                                     { "sustain", sustain },
                                     { "releaseMs", 10.0 } });
        return rig.play (samplesPerBeat);
    };

    const auto oneShot = play ("oneShot", 0.5);
    // Half way up the attack the level is half: at 240 samples the ramp is 0.01, times 0.5.
    CHECK_THAT (oneShot[240], WithinAbs (0.005, 0.001));
    // Sustained at half, long after the note.
    CHECK_THAT (oneShot[12000], WithinAbs (0.25, 0.01));

    const auto gate = play ("gate", 0.5);
    CHECK_THAT (gate[2000], WithinAbs (0.5 * 2000.0 / sampleFrames, 0.01));
    CHECK (peak (gate, 3600, gate.size()) == 0.0f);

    // No sustain: over once the decay is.
    CHECK (peak (play ("oneShot", 0.0), 1000, samplesPerBeat) == 0.0f);
}

TEST_CASE ("a zone's envelope bends at its tension points", "[sampler]")
{
    const auto play = [] (double attackTime, double attackLevel, double releaseTime, double releaseLevel)
    {
        SamplerRig rig ({ noteArgs (0, ticksPerBeat / 8, 60) }); // ends at 3000 samples
        rig.addFile ("ramp");
        rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id },
                                     { "mode", "gate" },
                                     { "attackMs", 10.0 },
                                     { "decayMs", 0.0 },
                                     { "sustain", 1.0 },
                                     { "releaseMs", 10.0 },
                                     { "attackHandleTime", attackTime },
                                     { "attackHandleLevel", attackLevel },
                                     { "releaseHandleTime", releaseTime },
                                     { "releaseHandleLevel", releaseLevel } });
        return rig.play (samplesPerBeat);
    };

    // Half way up the attack (240 of 480 samples) the ramp is 0.01, times the envelope there.
    const auto straight = play (0.5, 0.5, 0.5, 0.5);
    CHECK_THAT (straight[240], WithinAbs (0.005, 0.0005));

    const auto bent = play (0.2, 0.8, 0.8, 0.2);
    const EnvelopeHandle fast { 0.2f, 0.8f };
    CHECK (fast.shape (0.5f) > 0.6f);
    CHECK_THAT (bent[240], WithinAbs (0.01 * fast.shape (0.5f), 0.0005));

    // The release starts after the note (3000 samples) and lasts 480; half way it has fallen by its curve.
    const EnvelopeHandle slow { 0.8f, 0.2f };
    const auto at = static_cast<size_t> (3000 + 240);
    const auto ramp = static_cast<double> (at) / sampleFrames;
    CHECK_THAT (straight[at], WithinAbs (ramp * 0.5, 0.002));
    CHECK_THAT (bent[at], WithinAbs (ramp * (1.0 - slow.shape (0.5f)), 0.002));
}

TEST_CASE ("editing a zone needs no recompile, and a sounding note follows the edit", "[sampler][compiler]")
{
    const auto run = [] (bool editHalfWay)
    {
        SamplerRig rig ({ noteArgs (0, ticksPerBeat, 60) });
        rig.addFile ("ramp");
        rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id }, { "mode", "gate" } });
        auto first = rig.play (4000);

        if (editHalfWay)
        {
            rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id }, { "gainDb", -6.0 } });
            // Only the zones changed: the compiler hands the Sampler its new zones instead of recompiling.
            CHECK (rig.compiler.update (rig.project, spec) == nullptr);
        }

        const auto second = render (rig.engine, 4000).left();
        first.insert (first.end(), second.begin(), second.end());
        return first;
    };

    const auto plain = run (false);
    const auto edited = run (true);
    CHECK_THAT (edited[2000], WithinAbs (plain[2000], 1.0e-6));
    // A block or so for the new zones to arrive, then -6 dB.
    CHECK_THAT (edited[7000], WithinAbs (plain[7000] * std::pow (10.0f, -6.0f / 20.0f), 1.0e-4));
}

TEST_CASE ("a Sampler's voices play at once", "[sampler]")
{
    SamplerRig rig ({ noteArgs (0, ticksPerBeat, 60), noteArgs (0, ticksPerBeat, 60 + 12) });
    rig.addFile ("ramp");
    // The sum of the ramp at its own speed and at twice that.
    CHECK_THAT (slopeAt (rig.play (samplesPerBeat / 2), 2000), WithinAbs (3.0 * ownSlope, ownSlope * 0.03));
}

TEST_CASE ("a Sampler converts to a voice graph and sounds the same", "[sampler]")
{
    const auto notes = { noteArgs (0, ticksPerBeat / 8, 60), noteArgs (ticksPerBeat / 4, ticksPerBeat, 72) };
    const auto makeRig = [&] (SamplerRig& rig)
    {
        rig.addFile ("ramp");
        rig.edit ("setSampleZone",
                  { { "zoneId", rig.sampler().sampleZones[0].id }, { "attackMs", 5.0 }, { "tune", 0.5 } });
    };

    SamplerRig plain (notes);
    makeRig (plain);
    const auto instrument = plain.play (samplesPerBeat);

    SamplerRig converted (notes);
    makeRig (converted);
    converted.convert();
    const auto voice = converted.play (samplesPerBeat);
    float difference = 0.0f;

    for (size_t i = 0; i < instrument.size(); ++i)
        difference = std::max (difference, std::abs (instrument[i] - voice[i]));

    CHECK (difference < 0.01f);
}

TEST_CASE ("a Sample Voice only goes in a voice graph, and zones survive saving", "[sampler]")
{
    SamplerRig rig ({});
    CHECK_THROWS_AS (applyCommand (rig.project, "addNode",
                                   makeObject ({ { "trackId", rig.trackId }, { "type", nodetype::sampleVoice } })),
                     CommandError);

    rig.addFile ("kick", 36);
    rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id },
                                 { "tune", 1.5 },
                                 { "chokeGroup", 3 },
                                 { "mode", "gate" },
                                 { "decayMs", 20.0 } });
    CHECK (rig.project.audioFiles.size() == 1);

    std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> states;
    const auto saved = projectfile::toVar (rig.project, { "abc-id", "0.1.0" }, states);
    const auto loaded = projectfile::fromVar (saved, [] (const juce::String&) { return nullptr; });
    const auto& graph = loaded.project.tracks[0]->graph;
    const auto& zones = graph.nodes[static_cast<size_t> (graph.indexOfNode (rig.samplerId))].sampleZones;
    REQUIRE (zones.size() == 1);
    CHECK (zones[0] == rig.sampler().sampleZones[0]);

    rig.edit ("removeSampleZone", { { "zoneId", zones[0].id } });
    CHECK (rig.sampler().sampleZones.empty());
}

TEST_CASE ("planned zones come in with their frames, keys and velocities, each file pooled once", "[sampler][mapping]")
{
    SamplerRig rig ({});
    juce::Array<juce::var> files;
    files.add (fileArgs ("bass"));
    juce::Array<juce::var> zones;
    zones.add (makeObject (
        { { "file", 0 }, { "start", 0 }, { "end", 1000 }, { "rootKey", 36 }, { "lowKey", 0 }, { "highKey", 42 } }));
    zones.add (makeObject ({ { "file", 0 },
                             { "start", 1000 },
                             { "end", 2000 },
                             { "rootKey", 48 },
                             { "lowKey", 43 },
                             { "highKey", 127 },
                             { "lowVelocity", 64 } }));
    rig.edit ("addSampleZones", { { "files", files }, { "zones", zones } });

    const auto& added = rig.sampler().sampleZones;
    REQUIRE (added.size() == 2);
    CHECK (added[0].fileId == added[1].fileId);
    CHECK (rig.project.audioFiles.size() == 1);
    CHECK (added[1].start == 1000);
    CHECK (added[1].end == 2000);
    CHECK (added[1].rootKey == 48);
    CHECK (added[1].lowKey == 43);
    CHECK (added[1].highVelocity == 127);
    CHECK (added[1].lowVelocity == 64);
    CHECK (added[0].highKey == 42);

    zones.add (makeObject ({ { "file", 3 } }));
    CHECK_THROWS (applyCommand (
        rig.project, "addSampleZones",
        makeObject (
            { { "trackId", rig.trackId }, { "nodeId", rig.samplerId }, { "files", files }, { "zones", zones } })));
}

TEST_CASE ("in Keep length a note up an octave lasts as long as the sample, and the root key sounds as on tape",
           "[sampler]")
{
    // On tape an octave up plays the half-second ramp in a quarter of a second.
    SamplerRig tape ({ noteArgs (0, 4 * ticksPerBeat, 72) });
    tape.addFile ("ramp");
    const auto taped = tape.play (samplesPerBeat * 2);
    CHECK (peak (taped, sampleFrames / 2 + 500, taped.size()) == 0.0f);

    SamplerRig kept ({ noteArgs (0, 4 * ticksPerBeat, 72) });
    kept.addFile ("ramp");
    kept.edit ("setNodeParam", { { "param", "pitchMode" }, { "value", 1.0 } });
    const auto output = kept.play (samplesPerBeat * 2);
    CHECK (peak (output, sampleFrames - 3000, sampleFrames - 1000) > 0.5f);
    CHECK (peak (output, sampleFrames + 500, output.size()) == 0.0f);

    SamplerRig root ({ noteArgs (0, 4 * ticksPerBeat, 60) });
    root.addFile ("ramp");
    root.edit ("setNodeParam", { { "param", "pitchMode" }, { "value", 1.0 } });
    CHECK_THAT (slopeAt (root.play (samplesPerBeat / 2), 2000), WithinAbs (ownSlope, ownSlope * 0.01));
}

TEST_CASE ("a note plays a zone from each layer, and its layers don't choke each other", "[sampler]")
{
    SamplerRig rig ({ noteArgs (0, 4 * ticksPerBeat, 60) });
    rig.addFile ("one");
    rig.addFile ("two");
    // Both cover the keyboard; the second in layer 2, both in choke group 1.
    rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[1].id }, { "layer", 2 }, { "chokeGroup", 1 } });
    rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id }, { "chokeGroup", 1 } });
    CHECK_THAT (slopeAt (rig.play (samplesPerBeat / 2), 2000), WithinAbs (2.0 * ownSlope, ownSlope * 0.02));
}

TEST_CASE ("a zone's velocity crossfades set its level, equal power", "[sampler]")
{
    // A note at velocity 0.5 (63.5): halfway through a fade in from 32 to 95, and a fade out from 0 to 127.
    const auto levelAt = [] (std::initializer_list<std::pair<const char*, juce::var>> fade)
    {
        auto note = noteArgs (0, ticksPerBeat, 60);
        note.getDynamicObject()->setProperty ("velocity", 0.5);
        SamplerRig rig ({ note });
        rig.addFile ("ramp");
        auto args = makeObject (fade);
        args.getDynamicObject()->setProperty ("zoneId", rig.sampler().sampleZones[0].id);
        args.getDynamicObject()->setProperty ("trackId", rig.trackId);
        args.getDynamicObject()->setProperty ("nodeId", rig.samplerId);
        rig.project = applyCommand (rig.project, "setSampleZone", args);
        return slopeAt (rig.play (samplesPerBeat / 2), 2000) / ownSlope;
    };

    CHECK_THAT (levelAt ({}), WithinAbs (1.0, 0.02));
    CHECK_THAT (levelAt ({ { "fadeInLowVelocity", 32 }, { "fadeInHighVelocity", 95 } }),
                WithinAbs (std::sqrt (0.5), 0.02));
    CHECK_THAT (levelAt ({ { "fadeOutLowVelocity", 0 }, { "fadeOutHighVelocity", 127 } }),
                WithinAbs (std::sqrt (0.5), 0.02));
    CHECK_THAT (levelAt ({ { "fadeInLowVelocity", 64 }, { "fadeInHighVelocity", 100 } }), WithinAbs (0.0, 0.001));
}

TEST_CASE ("takes play in turn, or at random never twice in a row", "[sampler]")
{
    for (const auto random : { false, true })
    {
        SamplerRig rig ({ noteArgs (0, ticksPerBeat / 4, 60), noteArgs (ticksPerBeat, ticksPerBeat / 4, 60),
                          noteArgs (2 * ticksPerBeat, ticksPerBeat / 4, 60),
                          noteArgs (3 * ticksPerBeat, ticksPerBeat / 4, 60) });
        rig.addFile ("first");
        rig.addFile ("second");
        // The same keys and velocities make them takes; the second starts halfway through the ramp.
        rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[1].id }, { "start", sampleFrames / 2 } });
        rig.edit ("setNodeParam", { { "param", "takes" }, { "value", random ? 1.0 : 0.0 } });
        const auto output = rig.play (4 * samplesPerBeat);
        std::vector<bool> second;

        for (int note = 0; note < 4; ++note)
            second.push_back (output[static_cast<size_t> (note * samplesPerBeat + 100)] > 0.25f);

        // Two takes: never the same twice in a row either way, and in turn the first comes first.
        for (size_t i = 1; i < second.size(); ++i)
            CHECK (second[i] != second[i - 1]);

        if (! random)
            CHECK_FALSE (second[0]);
    }
}

TEST_CASE ("in Keep length a zone plays renders at its keys' pitches, as long as the sample", "[sampler][stretch]")
{
    // A 220 Hz sine of half a second on a zone from 60 to 72, rendered with Rubber Band into a cache folder of its own.
    struct Folder
    {
        juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("dawg-sampler-" + juce::Uuid().toString());
        Folder() = default;
        Folder (const Folder&) = delete;
        Folder& operator= (const Folder&) = delete;
        Folder (Folder&&) = delete;
        Folder& operator= (Folder&&) = delete;
        ~Folder() { std::ignore = folder.deleteRecursively(); }
    } cache;

    juce::AudioBuffer<float> sine (1, sampleFrames);

    for (int i = 0; i < sampleFrames; ++i)
        sine.setSample (
            0, i, 0.5f * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / 48000.0)));

    StretchRenderer renderer (
        cache.folder, makeRubberBandStretcher(),
        [&] (const AudioFile&, juce::int64 from, juce::AudioBuffer<float>& into)
        {
            into.clear();

            for (int i = 0; i < into.getNumSamples(); ++i)
                if (const auto frame = from + i; frame >= 0 && frame < sampleFrames)
                    into.setSample (0, i, sine.getSample (0, static_cast<int> (frame)));

            return true;
        },
        [] (AudioFile& file) { return file.path; }, 2);

    SamplerRig rig ({ noteArgs (0, 4 * ticksPerBeat, 72) });
    rig.source = AudioSource::fromBuffer (sine, 48000.0);
    rig.compiler.setStretchRenderer (&renderer);
    rig.addFile ("sine");
    rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id }, { "lowKey", 60 }, { "highKey", 72 } });
    rig.edit ("setNodeParam", { { "param", "pitchMode" }, { "value", 1.0 } });

    // The first compilation asks for the renders; once they're ready, the next plays them.
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));
    REQUIRE (renderer.waitUntilIdle (60000));
    const auto output = rig.play (samplesPerBeat * 2);

    // An octave up: 440 Hz, and still half a second long.
    const auto crossings = zeroCrossings (output, 2000, 22000);
    CHECK_THAT (crossings * 48000.0 / (2.0 * 20000.0), WithinAbs (440.0, 8.0));
    CHECK (peak (output, sampleFrames - 3000, sampleFrames - 1000) > 0.2f);
    CHECK (peak (output, sampleFrames + 2000, output.size()) < 0.01f);

    // Its level steady, without the grains' beating.
    auto quietest = 1.0f;
    auto loudest = 0.0f;

    for (size_t at = 2000; at < 20000; at += 1000)
    {
        quietest = std::min (quietest, peak (output, at, at + 1000));
        loudest = std::max (loudest, peak (output, at, at + 1000));
    }

    CHECK (loudest / quietest < 1.1f);
}

TEST_CASE ("an export renders a Keep length Sampler's pitches first, and a Tape one's not at all", "[sampler][stretch]")
{
    SamplerRig rig ({});
    rig.addFile ("ramp");
    rig.edit ("setSampleZone", { { "zoneId", rig.sampler().sampleZones[0].id }, { "lowKey", 58 }, { "highKey", 63 } });
    CHECK (stretchJobs (rig.project).empty());

    rig.edit ("setNodeParam", { { "param", "pitchMode" }, { "value", 1.0 } });
    const auto jobs = stretchJobs (rig.project);
    // Keys 58 to 63 around the root, 60: five renders, the root itself needing none.
    REQUIRE (jobs.size() == 5);
    CHECK_FALSE (jobs[0].spec.formants);
    CHECK (jobs[0].spec.outputFrames == jobs[0].inputFrames);
    CHECK_THAT (jobs[0].spec.pitchScale, WithinAbs (std::exp2 (-2.0 / 12.0), 1e-9));
}

TEST_CASE (
    "a step sequencer's first step sounds every time playback starts at its clip's start, on a track with latency",
    "[sampler][steps]")
{
    SamplerRig rig ({});
    rig.addFile ("ramp");
    // A loud, flat sample, so any of it shows.
    juce::AudioBuffer<float> flat (1, sampleFrames);
    juce::FloatVectorOperations::fill (flat.getWritePointer (0), 0.5f, sampleFrames);
    rig.source = AudioSource::fromBuffer (std::move (flat), 48000.0);

    rig.project = apply (rig.project, "addClip",
                         { { "trackId", rig.trackId },
                           { "start", 0 },
                           { "length", 4 * ticksPerBeat },
                           { "sequencer", makeObject ({ { "pattern", 0 } }) } });
    rig.project =
        apply (rig.project, "insertNode", { { "trackId", rig.trackId }, { "type", nodetype::stepSequencer } });
    // A Limiter looks ahead, so the track is processed ahead of the playhead by its latency.
    rig.project = apply (rig.project, "insertNode", { { "trackId", rig.trackId }, { "type", nodetype::limiter } });

    // The first step on, on the Sampler's root key.
    auto track = std::make_shared<Track> (*rig.project.tracks[0]);

    for (auto& node : track->graph.nodes)
        if (node.type == nodetype::stepSequencer)
        {
            StepSequence steps;
            steps.rows.push_back ({ 60, "Note" });
            steps = projectfile::normalisedSteps (std::move (steps));
            steps.patterns[0].rows[0].cells[0].on = true;
            node.steps = std::make_shared<const StepSequence> (steps);
        }

    rig.project.tracks[0] = track;

    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));

    for (int round = 0; round < 12; ++round)
    {
        rig.engine.seek (0.0);
        rig.engine.play();
        const auto started = render (rig.engine, 4800).left();
        INFO ("round " << round);
        CHECK (peak (started, 0, 2400) > 0.2f);

        rig.engine.stop();
        std::ignore = render (rig.engine, 512 + 37 * round); // stop at a different sample each time
    }
}
