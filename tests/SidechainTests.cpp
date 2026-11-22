#include "TestUtils.h"
#include "dawg/PluginNode.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Sidechain inputs (docs/design/sidechain.md): key sends, the order, key buses and the compressor's detector.

namespace
{
[[nodiscard]] ObjectId idOf (const Project& project, const juce::String& name)
{
    for (const auto& track : project.tracks)
        if (track->name == name)
            return track->id;

    FAIL ("No track named " << name); // [[noreturn]]: the loop above either returns or fails
}

[[nodiscard]] const Track& trackNamed (const Project& project, const juce::String& name)
{
    return *project.findTrack (idOf (project, name));
}

[[nodiscard]] ObjectId compressorOf (const Project& project, const juce::String& name)
{
    for (const auto& node : trackNamed (project, name).graph.nodes)
        if (node.type == nodetype::compressor)
            return node.id;

    FAIL ("No compressor on " << name); // [[noreturn]]: the loop above either returns or fails
}

// Keys `to`'s compressor (or another node) from track `from`.
[[nodiscard]] Project key (const Project& project, const juce::String& from, const juce::String& to, ObjectId node = 0)
{
    return applyCommand (project, "addSend",
                         makeObject ({ { "trackId", idOf (project, from) },
                                       { "target", idOf (project, to) },
                                       { "targetNode", node != 0 ? node : compressorOf (project, to) } }));
}

// Tracks "Kick" and "Bass".
[[nodiscard]] Project kickAndBass()
{
    auto project = apply ({}, "addTrack", { { "name", "Kick" } });
    return apply (project, "addTrack", { { "name", "Bass" } });
}
} // namespace

TEST_CASE ("a key send keys a compressor on another track, pre-fader", "[sidechain][commands]")
{
    const auto project = key (kickAndBass(), "Kick", "Bass");
    const auto& sends = trackNamed (project, "Kick").sends;

    REQUIRE (sends.size() == 1);
    CHECK (sends[0].target == idOf (project, "Bass"));
    CHECK (sends[0].targetNode == compressorOf (project, "Bass"));
    CHECK (sends[0].preFader);
    CHECK_THROWS_AS (key (project, "Kick", "Bass"), CommandError); // once per node
}

TEST_CASE ("keys go only to keyable nodes on other tracks, without loops", "[sidechain][commands]")
{
    auto project = kickAndBass();
    const auto bassGain = [&project]
    {
        for (const auto& node : trackNamed (project, "Bass").graph.nodes)
            if (node.type == nodetype::gain)
                return node.id;
        return ObjectId { 0 };
    }();

    CHECK_THROWS_AS (key (project, "Kick", "Bass", bassGain), CommandError);             // not keyable
    CHECK_THROWS_AS (key (project, "Bass", "Bass"), CommandError);                       // its own node
    CHECK_THROWS_AS (key (key (project, "Kick", "Bass"), "Bass", "Kick"), CommandError); // each other

    // A group keying a member's compressor would need the member both before and after the group.
    project = apply (project, "groupTracks",
                     { { "ids", juce::Array<juce::var> { idOf (project, "Bass") } }, { "name", "G" } });
    CHECK_THROWS_AS (key (project, "G", "Bass"), CommandError);
    CHECK_NOTHROW (key (project, "Bass", "G")); // a member keying its group is fine
}

TEST_CASE ("removing a keyed node removes its key sends", "[sidechain][commands]")
{
    auto project = key (kickAndBass(), "Kick", "Bass");
    project = apply (project, "removeNode",
                     { { "trackId", idOf (project, "Bass") }, { "nodeId", compressorOf (project, "Bass") } });
    CHECK (trackNamed (project, "Kick").sends.empty());

    // And repairing a project drops keys to nodes that aren't there.
    project = key (kickAndBass(), "Kick", "Bass");
    auto broken = project;
    auto kick = *broken.tracks[0];
    kick.sends[0].targetNode = 999999;
    broken.tracks[0] = std::make_shared<const Track> (std::move (kick));
    normaliseGroups (broken);
    CHECK (broken.tracks[0]->sends.empty());
}

// ---------------------------------------------------------------------------------------------------------
// The engine

namespace
{
constexpr float level = 0.25f; // -12 dBFS
constexpr int frames = 2048;

// Two monitored audio tracks on the same constant input: "Bass" first, then a muted "Kick" (so only the bass is
// heard), with the bass's compressor on, keyed, fast and hard: threshold -60 dB, 20:1.
struct KeyRig
{
    KeyRig()
        : input (frames, level)
    {
        for (const auto* name : { "Bass", "Kick" })
        {
            project = apply (project, "addTrack", { { "kind", "audio" }, { "name", name } });
            project = apply (project, "setTrackSwitches",
                             { { "id", project.tracks.back()->id }, { "inputChannel", 0 }, { "monitor", 2 } });
        }

        project = apply (project, "setTrackSwitches", { { "id", idOf (project, "Kick") }, { "mute", true } });
        const auto bass = idOf (project, "Bass");
        const auto compressor = compressorOf (project, "Bass");
        project =
            apply (project, "setNodeBypass", { { "trackId", bass }, { "nodeId", compressor }, { "bypassed", false } });

        for (const auto& [param, value] :
             std::initializer_list<std::pair<const char*, double>> { { "threshold", -60.0 },
                                                                     { "ratio", 20.0 },
                                                                     { "attack", 0.1 },
                                                                     { "release", 10.0 },
                                                                     { "detector", 1.0 } })
            project = apply (project, "setNodeParam",
                             { { "trackId", bass }, { "nodeId", compressor }, { "param", param }, { "value", value } });

        project = withMaster (project);
        engine.setEngine (compiler.compile (project, spec));
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);

        if (auto compiled = compiler.update (project, spec))
            engine.setEngine (std::move (compiled));
    }

    void param (const char* name, double value)
    {
        edit ("setNodeParam", { { "trackId", idOf (project, "Bass") },
                                { "nodeId", compressorOf (project, "Bass") },
                                { "param", name },
                                { "value", value } });
    }

    // The level at the end of a block, after a block to settle.
    [[nodiscard]] float render()
    {
        const std::array<const float*, 1> inputs { input.data() };
        StereoBuffer out (frames);

        for (int i = 0; i < 2; ++i)
            engine.renderOffline (out.channels(), 2, frames, sampleRate, inputs.data(), 1);

        return out.left()[frames - 1];
    }

    std::vector<float> input;
    Project project;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

TEST_CASE ("a key from another track ducks the compressor, even from a muted track", "[sidechain][engine]")
{
    KeyRig rig;

    // Keyed but with no key: the detector hears silence.
    CHECK_THAT (rig.render(), WithinAbs (level, 1e-4));

    // The kick (after the bass in the list, so the order must change) keys the bass: 48 dB over at 20:1.
    rig.project = key (rig.project, "Kick", "Bass");
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));
    const auto ducked = rig.render();
    CHECK (ducked < level * 0.01f);

    // Pre-fader: the kick's fader doesn't matter; post-fader it does.
    rig.edit ("setTrackGain", { { "id", idOf (rig.project, "Kick") }, { "gainDb", -96.0 } });
    CHECK_THAT (rig.render(), WithinAbs (ducked, 1e-5));
    const auto sendId = trackNamed (rig.project, "Kick").sends[0].id;
    rig.edit ("setSend", { { "trackId", idOf (rig.project, "Kick") }, { "sendId", sendId }, { "preFader", false } });
    CHECK_THAT (rig.render(), WithinAbs (level, 1e-3));
}

TEST_CASE ("the detector's high-pass and Listen", "[sidechain][engine]")
{
    KeyRig rig;
    rig.project = key (rig.project, "Kick", "Bass");
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));

    // Listen plays the detector signal: the kick's constant level.
    rig.param ("listen", 1.0);
    CHECK_THAT (rig.render(), WithinAbs (level, 1e-4));

    // The high-pass takes the constant away, so there's nothing to hear or react to.
    rig.param ("keyHp", 1.0);
    CHECK_THAT (rig.render(), WithinAbs (0.0, 1e-3));
    rig.param ("listen", 0.0);
    CHECK_THAT (rig.render(), WithinAbs (level, 1e-3));

    // Internal detection reacts to the bass itself.
    rig.param ("keyHp", 0.0);
    rig.param ("detector", 0.0);
    CHECK (rig.render() < level * 0.01f);
}

// ---------------------------------------------------------------------------------------------------------
// Plugins with a sidechain bus (K7)

namespace
{
// An effect with an aux input bus that plays its sidechain instead of its input.
class KeyedProcessor final : public juce::AudioProcessor
{
public:
    KeyedProcessor()
        : juce::AudioProcessor (BusesProperties()
                                    .withInput ("In", juce::AudioChannelSet::stereo())
                                    .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)
                                    .withOutput ("Out", juce::AudioChannelSet::stereo()))
    {
    }

    const juce::String getName() const override { return "Keyed"; }
    void prepareToPlay (double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        const auto key = getBusBuffer (buffer, true, 1);

        for (int ch = 0; ch < 2; ++ch)
            buffer.copyFrom (ch, 0, key, ch, 0, buffer.getNumSamples());
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int /*index*/) override {}
    const juce::String getProgramName (int /*index*/) override { return {}; }
    void changeProgramName (int /*index*/, const juce::String& /*newName*/) override {}
    void getStateInformation (juce::MemoryBlock& /*destData*/) override {}
    void setStateInformation (const void* /*data*/, int /*sizeInBytes*/) override {}
};
} // namespace

TEST_CASE ("an effect plugin's sidechain bus gets the key input", "[sidechain][plugins]")
{
    PluginNode node (false);
    node.prepare (spec);
    node.attach (std::make_unique<KeyedProcessor>());
    REQUIRE (node.hasSidechain());

    constexpr int n = 64;
    std::vector<float> main (n, 0.1f);
    std::vector<float> keyed (n, 0.6f);
    std::vector<float> left (n);
    std::vector<float> right (n);
    const std::array<AudioInput, 2> inputs { AudioInput { main.data(), main.data() },
                                             AudioInput { keyed.data(), keyed.data() } };
    const std::array<AudioOutput, 1> outputs { AudioOutput { left.data(), right.data() } };
    const Transport transport;

    ProcessContext context;
    context.numSamples = n;
    context.audioInputs = inputs;
    context.audioOutputs = outputs;
    context.transport = &transport;
    node.process (context);

    CHECK_THAT (left[n - 1], WithinAbs (0.6, 1e-6));
    CHECK_THAT (right[n - 1], WithinAbs (0.6, 1e-6));
}
