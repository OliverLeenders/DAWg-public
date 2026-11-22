#include "dawg/Sfz.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Reading SFZ patches (docs/design/sampler-mapping.md, section SF).

using namespace dawg;
using Catch::Matchers::WithinAbs;

namespace
{
const juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("patch");

[[nodiscard]] SfzPatch read (const juce::String& text, const std::map<juce::String, juce::String>& includes = {})
{
    return readSfz (
        text, folder,
        [&] (const juce::File& file)
        {
            const auto found = includes.find (file.getFileName());
            return found == includes.end() ? juce::String() : found->second;
        },
        [] (const juce::File& sample) { return ! sample.getFileName().startsWith ("missing"); });
}

[[nodiscard]] double field (const SfzRegion& region, const char* name)
{
    return region.fields[name];
}
} // namespace

TEST_CASE ("an SFZ patch's headers pass their opcodes down to its regions", "[sampler][sfz]")
{
    const auto patch = read (R"(
        <control> default_path=Samples\
        #define $VOL -3
        <global> volume=$VOL ampeg_release=0.5
        <group> lovel=1 hivel=64
        <region> sample=Piano C4 soft.wav key=c4
        <region> sample=Piano E4 soft.wav lokey=62 hikey=66 pitch_keycenter=64 tune=-20 /* a comment */
        <group> lovel=65 hivel=127 loop_mode=one_shot
        <region> sample=Piano C4 loud.wav key=60 // the loud one
    )");

    REQUIRE (patch.regions.size() == 3);
    const auto& soft = patch.regions[0];
    CHECK (soft.sample == folder.getChildFile ("Samples/Piano C4 soft.wav"));
    CHECK (field (soft, "rootKey") == 60);
    CHECK (field (soft, "lowKey") == 60);
    CHECK (field (soft, "highKey") == 60);
    CHECK (field (soft, "lowVelocity") == 0);
    CHECK (field (soft, "highVelocity") == 64);
    CHECK_THAT (field (soft, "gainDb"), WithinAbs (-3.0, 1e-9));
    CHECK_THAT (field (soft, "releaseMs"), WithinAbs (500.0, 1e-9));
    CHECK (soft.fields["mode"] == "gate");
    CHECK_FALSE (soft.fields.hasProperty ("attackMs"));

    const auto& middle = patch.regions[1];
    CHECK (field (middle, "rootKey") == 64);
    CHECK (field (middle, "lowKey") == 62);
    CHECK (field (middle, "highKey") == 66);
    CHECK_THAT (field (middle, "tune"), WithinAbs (-0.2, 1e-9));

    const auto& loud = patch.regions[2];
    CHECK (field (loud, "lowVelocity") == 65);
    CHECK (loud.fields["mode"] == "oneShot");
    CHECK_THAT (field (loud, "gainDb"), WithinAbs (-3.0, 1e-9)); // the global level still holds

    // None of them overlap another, so they share a layer.
    for (const auto& region : patch.regions)
        CHECK (field (region, "layer") == 1);
}

TEST_CASE ("an SFZ patch's loops, frames and pan become a zone's", "[sampler][sfz]")
{
    const auto patch = read ("<region> sample=pad.wav offset=100 end=9999 loop_mode=loop_continuous loop_start=2000 "
                             "loop_end=7999 pan=-50 ampeg_attack=0.01 ampeg_sustain=50");
    REQUIRE (patch.regions.size() == 1);
    const auto& pad = patch.regions[0];
    CHECK (field (pad, "start") == 100);
    CHECK (field (pad, "end") == 10000);
    CHECK (pad.fields["mode"] == "loop");
    CHECK (field (pad, "loopStart") == 2000);
    CHECK (field (pad, "loopEnd") == 8000);
    CHECK_THAT (field (pad, "pan"), WithinAbs (-0.5, 1e-9));
    CHECK_THAT (field (pad, "attackMs"), WithinAbs (10.0, 1e-9));
    CHECK_THAT (field (pad, "sustain"), WithinAbs (0.5, 1e-9));
}

TEST_CASE ("release triggers and missing samples are left out, unknown opcodes named", "[sampler][sfz]")
{
    const auto patch = read (R"(
        <region> sample=kick.wav key=36 fil_type=lpf_2p cutoff=800
        <region> sample=kick release.wav key=36 trigger=release
        <region> sample=missing.wav key=37
    )");
    CHECK (patch.regions.size() == 1);
    CHECK (patch.skipped == 2);
    CHECK (patch.ignored.contains ("fil_type"));
    CHECK (patch.ignored.contains ("cutoff"));
}

TEST_CASE ("overlapping regions are layers, alternating ones takes in order", "[sampler][sfz]")
{
    const auto patch = read (R"(
        <group> key=38 seq_length=2
        <region> sample=snare b.wav seq_position=2
        <region> sample=snare a.wav seq_position=1
        <group> key=38
        <region> sample=snare room.wav
        <region> sample=snare bottom.wav
    )");
    REQUIRE (patch.regions.size() == 4);
    CHECK_FALSE (patch.randomTakes);

    // The takes in their order, in layer 1; the other two each a layer over them.
    CHECK (patch.regions[0].sample.getFileName() == "snare room.wav");
    std::map<juce::String, int> layers;

    for (const auto& region : patch.regions)
        layers[region.sample.getFileName()] = static_cast<int> (region.fields["layer"]);

    CHECK (layers["snare a.wav"] == 1);
    CHECK (layers["snare b.wav"] == 1);
    CHECK (layers["snare room.wav"] == 2);
    CHECK (layers["snare bottom.wav"] == 3);

    const auto a = std::ranges::find_if (patch.regions,
                                         [] (const SfzRegion& r) { return r.sample.getFileName() == "snare a.wav"; });
    const auto b = std::ranges::find_if (patch.regions,
                                         [] (const SfzRegion& r) { return r.sample.getFileName() == "snare b.wav"; });
    CHECK (a < b);

    CHECK (read ("<region> sample=x.wav key=40 lorand=0 hirand=0.5 <region> sample=y.wav key=40 lorand=0.5 hirand=1")
               .randomTakes);
}

TEST_CASE ("velocity crossfades and velocity tracking come in, as ConvertWithMoss writes them", "[sampler][sfz]")
{
    const auto patch = read (R"(
        <global> amp_veltrack=39.37 global_label=Piano amp_velcurve_1=0 amp_velcurve_127=1
        <region> sample=soft.wav hivel=63 xfout_lovel=37 xfout_hivel=63
        <region> sample=loud.wav lovel=38 xfin_lovel=38 xfin_hivel=59
    )");

    REQUIRE (patch.regions.size() == 2);
    CHECK (field (patch.regions[0], "fadeOutLowVelocity") == 37);
    CHECK (field (patch.regions[0], "fadeOutHighVelocity") == 63);
    CHECK_FALSE (patch.regions[0].fields.hasProperty ("fadeInLowVelocity"));
    CHECK (field (patch.regions[1], "fadeInLowVelocity") == 38);
    CHECK (field (patch.regions[1], "fadeInHighVelocity") == 59);
    // They overlap in velocity, so they play together, as layers.
    CHECK (field (patch.regions[1], "layer") == 2);
    CHECK_THAT (patch.velocityTracking, WithinAbs (39.37, 1e-9));
    CHECK (patch.ignored == juce::StringArray { "amp_velcurve_N" });
    CHECK (read ("<region> sample=a.wav").velocityTracking == -1.0);
}

TEST_CASE ("a group another region is cut off by becomes a choke group, and includes are read", "[sampler][sfz]")
{
    const auto patch = read (R"(
        #include "drums.sfz"
    )",
                             { { "drums.sfz", "<region> sample=closed.wav key=42 group=1 off_by=1\n"
                                              "<region> sample=open.wav key=46 group=1 off_by=1\n"
                                              "<region> sample=crash.wav key=49 group=7\n" } });
    REQUIRE (patch.regions.size() == 3);
    CHECK (field (patch.regions[0], "chokeGroup") == 1);
    CHECK (field (patch.regions[1], "chokeGroup") == 1);
    CHECK_FALSE (patch.regions[2].fields.hasProperty ("chokeGroup"));
}

TEST_CASE ("regions playing at the same random value are layers, at different ones takes", "[sampler][sfz]")
{
    const auto patch = read (R"(
        <group> key=68 pan=-100
        <region> sample=left 1.wav lorand=0 hirand=0.5
        <region> sample=left 2.wav lorand=0.5 hirand=1
        <group> key=68 pan=100
        <region> sample=right 1.wav lorand=0 hirand=0.5
        <region> sample=right 2.wav lorand=0.5 hirand=1
    )");
    REQUIRE (patch.regions.size() == 4);
    std::map<juce::String, int> layers;

    for (const auto& region : patch.regions)
        layers[region.sample.getFileNameWithoutExtension()] = static_cast<int> (region.fields["layer"]);

    CHECK (layers["left 1"] == 1);
    CHECK (layers["left 2"] == 1);
    CHECK (layers["right 1"] == 2);
    CHECK (layers["right 2"] == 2);
}
