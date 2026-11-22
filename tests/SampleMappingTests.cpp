#include "dawg/SampleMapping.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

// Mapping imported samples (docs/design/sampler-mapping.md).

using namespace dawg;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double rate = 48000.0;

// A decaying sine at a key for `seconds`, then silence to the end of `slot` seconds.
void tone (juce::AudioBuffer<float>& audio, double at, double key, double seconds, double slot)
{
    const auto hz = 440.0 * std::exp2 ((key - 69.0) / 12.0);
    const auto first = static_cast<int> (at * rate);
    const auto sounding = static_cast<int> (seconds * rate);

    for (int i = 0; i < static_cast<int> (slot * rate) && first + i < audio.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        const auto value =
            i < sounding ? 0.5 * std::sin (juce::MathConstants<double>::twoPi * hz * t) * std::exp (-t) : 0.0;

        for (int c = 0; c < audio.getNumChannels(); ++c)
            audio.addSample (c, first + i, static_cast<float> (value));
    }
}

[[nodiscard]] SampleToMap sliced (const juce::String& name, std::vector<SampleSlice> slices)
{
    SampleToMap sample;
    sample.name = name;
    sample.slices = std::move (slices);
    return sample;
}

[[nodiscard]] SampleToMap named (const juce::String& name, double pitch = 0.0)
{
    return sliced (name, { SampleSlice { 0, 0, pitch } });
}
} // namespace

TEST_CASE ("a sample's name gives its note, however it's written", "[sampler][mapping]")
{
    CHECK (parseSampleName ("McGaffin Carillon_C3").key == 48);
    CHECK (parseSampleName ("musicBox_brassBells_Bb3").key == 58);
    CHECK (parseSampleName ("musicBox_brassBells_ G4").key == 67);
    CHECK (parseSampleName ("musicBox_Ab4_L-001").key == 68);
    CHECK (parseSampleName ("Raiden_Lead_01_C-2").key == 36);
    CHECK (parseSampleName ("Raiden_Lead_02_C#2").key == 37);
    CHECK (parseSampleName ("G-String Sustain_fret11_noteF#5-002").key == 78);
    CHECK (parseSampleName ("Perc_TubularBell_C4-[3]").key == 60);
    CHECK (parseSampleName (juce::CharPointer_UTF8 ("Bell C\xe2\x99\xaf"
                                                    "3"))
               .key
           == 49);
    CHECK (parseSampleName ("key01").key == -1);
    CHECK (parseSampleName ("303_Bass").key == -1);
    CHECK (parseSampleName ("AF_Mute_f").key == -1);
    CHECK (parseSampleName ("D50 Pad").key == -1);
}

TEST_CASE ("a sample's name gives its loudness and the octave its notes count from", "[sampler][mapping]")
{
    const auto piano = parseSampleName ("Grand Piano-00-Velocity 20-027--d#0");
    CHECK (piano.key == 15);
    CHECK (piano.octaveHint == 12);
    CHECK (piano.velocity == 20);

    const auto soft = parseSampleName ("ab2_pp");
    CHECK (soft.key == 44);
    CHECK (soft.velocity == 32);
    CHECK (parseSampleName ("AF_Mute_f").velocity == 96);
    CHECK (parseSampleName ("Wurli_A1_FF").velocity == 112);

    const auto snare = parseSampleName ("Snare_Solo_A#1_AB01_buzzpressedcenter001_48_63");
    CHECK (snare.key == 34);
    CHECK (snare.lowVelocity == 48);
    CHECK (snare.highVelocity == 63);
    CHECK (parseSampleName ("rototom_1_01").lowVelocity == -1);
}

TEST_CASE ("a file of notes in equal slots is cut into them, each with its pitch", "[sampler][mapping]")
{
    // Three slots of two seconds, C2, C3 and C4, the first with a second hit inside it that is no slot of its own.
    juce::AudioBuffer<float> audio (2, static_cast<int> (6.0 * rate));
    audio.clear();
    tone (audio, 0.0, 36.0, 0.6, 2.0);
    tone (audio, 0.9, 36.0, 0.3, 1.0);
    tone (audio, 2.0, 48.0, 1.5, 2.0);
    tone (audio, 4.0, 60.0, 1.5, 2.0);

    const auto slices = analyseSample (audio, rate);
    REQUIRE (slices.size() == 3);
    CHECK (slices[0].start == 0);
    CHECK (slices[0].end == slices[1].start);
    CHECK (slices[1].start > static_cast<juce::int64> (1.99 * rate));
    CHECK (slices[1].start < static_cast<juce::int64> (2.0 * rate));
    CHECK (slices[2].end == audio.getNumSamples());
    CHECK_THAT (slices[0].pitch, WithinAbs (36.0, 0.3));
    CHECK_THAT (slices[1].pitch, WithinAbs (48.0, 0.3));
    CHECK_THAT (slices[2].pitch, WithinAbs (60.0, 0.3));
}

TEST_CASE ("a file of one sound stays whole, with its pitch, and noise has none", "[sampler][mapping]")
{
    juce::AudioBuffer<float> audio (1, static_cast<int> (2.0 * rate));
    audio.clear();
    tone (audio, 0.0, 69.0, 1.8, 2.0);
    const auto slices = analyseSample (audio, rate);
    REQUIRE (slices.size() == 1);
    CHECK (slices[0].start == 0);
    CHECK (slices[0].end == 0);
    CHECK_THAT (slices[0].pitch, WithinAbs (69.0, 0.2));

    juce::Random random (7);

    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i, random.nextFloat() - 0.5f);

    CHECK (analyseSample (audio, rate)[0].pitch == 0.0);
}

TEST_CASE ("notes spread to the keys halfway to their neighbours", "[sampler][mapping]")
{
    const auto zones = planSampleZones ({ named ("Bell_C4"), named ("Bell_C2"), named ("Bell_C3") });
    REQUIRE (zones.has_value());
    REQUIRE (zones->size() == 3);
    CHECK ((*zones)[0] == PlannedZone { 1, 0, 0, 36, 0, 42, 0, 127 });
    CHECK ((*zones)[1] == PlannedZone { 2, 0, 0, 48, 43, 54, 0, 127 });
    CHECK ((*zones)[2] == PlannedZone { 0, 0, 0, 60, 55, 127, 0, 127 });
}

TEST_CASE ("loudness layers split the velocities, the same on every key", "[sampler][mapping]")
{
    const auto zones = planSampleZones ({ named ("a3_pp"), named ("a3_ff"), named ("a4_pp") });
    REQUIRE (zones.has_value());
    REQUIRE (zones->size() == 3);
    // pp at 32 and ff at 112 meet at 72. The ff layer has one note, so it covers the keyboard.
    CHECK ((*zones)[0] == PlannedZone { 0, 0, 0, 57, 0, 63, 0, 72 });
    CHECK ((*zones)[1] == PlannedZone { 1, 0, 0, 57, 0, 127, 73, 127 });
    CHECK ((*zones)[2] == PlannedZone { 2, 0, 0, 69, 64, 127, 0, 72 });

    const auto ranged = planSampleZones ({ named ("Snare_C2_0_47"), named ("Snare_C2_48_127") });
    REQUIRE (ranged.has_value());
    CHECK ((*ranged)[0].lowVelocity == 0);
    CHECK ((*ranged)[0].highVelocity == 47);
    CHECK ((*ranged)[1].lowVelocity == 48);
}

TEST_CASE ("names that count middle C as C3 move up an octave", "[sampler][mapping]")
{
    // By the pitches: most of the files sound an octave above their names.
    const auto heard = planSampleZones ({ named ("Pad_C3", 60.1), named ("Pad_C4", 72.0), named ("Pad_C5") });
    REQUIRE (heard.has_value());
    CHECK ((*heard)[0].rootKey == 60);
    CHECK ((*heard)[2].rootKey == 84);

    // By the MIDI numbers beside the names.
    const auto numbered = planSampleZones ({ named ("Piano-027--d#0"), named ("Piano-033--a0") });
    REQUIRE (numbered.has_value());
    CHECK ((*numbered)[0].rootKey == 27);
    CHECK ((*numbered)[1].rootKey == 33);

    // Names that agree with the pitches stay.
    const auto agreed = planSampleZones ({ named ("Pad_C3", 48.0), named ("Pad_C4", 60.0) });
    CHECK ((*agreed)[0].rootKey == 48);
}

TEST_CASE ("the note, title and loop a file stores come before its name", "[sampler][mapping]")
{
    // A stored root key is exact: no octave convention or pitch moves it.
    auto low = named ("Bell_C3", 60.0);
    low.storedKey = 50;
    auto high = named ("Bell_C4", 72.0);
    high.storedKey = 62;
    high.loopStart = 1000;
    high.loopEnd = 5000;
    const auto stored = planSampleZones ({ low, high });
    REQUIRE (stored.has_value());
    CHECK ((*stored)[0].rootKey == 50);
    CHECK ((*stored)[1].rootKey == 62);
    CHECK ((*stored)[0].loopStart == -1);
    CHECK ((*stored)[1].loopStart == 1000);
    CHECK ((*stored)[1].loopEnd == 5000);

    // A stored middle C against another note in the name is a recorder's default: the name wins.
    auto defaulted = named ("Pad_A3");
    defaulted.storedKey = 60;
    const auto byName = planSampleZones ({ defaulted, named ("Pad_A4") });
    REQUIRE (byName.has_value());
    CHECK ((*byName)[0].rootKey == 57);

    // A title names the note when the file name doesn't.
    auto first = named ("Carillon 01");
    first.title = "Carillon G4";
    auto second = named ("Carillon 02");
    second.title = "Carillon A4";
    const auto titled = planSampleZones ({ first, second });
    REQUIRE (titled.has_value());
    CHECK ((*titled)[0].rootKey == 67);
    CHECK ((*titled)[1].rootKey == 69);
}

TEST_CASE ("files without notes or pitches take a key each, and pitches map unnamed files", "[sampler][mapping]")
{
    const auto drums = planSampleZones ({ named ("Kick"), named ("Snare"), named ("HiHat") });
    REQUIRE (drums.has_value());
    REQUIRE (drums->size() == 3);
    CHECK ((*drums)[0] == PlannedZone { 2, 0, 0, 36, 36, 36, 0, 127 });
    CHECK ((*drums)[2].rootKey == 38);
    CHECK_FALSE (planSampleZones ({ named ("Kick") }).has_value());

    const auto zones = planSampleZones ({ named ("key02", 62.0), named ("key01", 60.1), named ("key10", 64.0) });
    REQUIRE (zones.has_value());
    CHECK ((*zones)[0].rootKey == 60);
    CHECK ((*zones)[0].file == 1);
    CHECK ((*zones)[2].rootKey == 64);
}

TEST_CASE ("a sliced file's slots become zones, an octave apart where a pitch was missed", "[sampler][mapping]")
{
    const auto zones =
        planSampleZones ({ sliced ("303_Bass", { { 0, 100, 36.0 }, { 100, 200, 48.1 }, { 200, 300, 59.9 } }) });
    REQUIRE (zones.has_value());
    REQUIRE (zones->size() == 3);
    CHECK ((*zones)[1] == PlannedZone { 0, 100, 200, 48, 43, 54, 0, 127 });
}

TEST_CASE ("slots are found by their period, with a short last slot and a slot without an attack of its own",
           "[sampler][mapping]")
{
    // Two-second slots in a file of 9.5 seconds; the slot at 4 s follows a sound that rings to it.
    juce::AudioBuffer<float> audio (1, static_cast<int> (9.5 * rate));
    audio.clear();
    tone (audio, 0.0, 36.0, 1.5, 2.0);
    tone (audio, 2.0, 48.0, 2.0, 2.0);
    tone (audio, 4.0, 60.0, 1.5, 2.0);
    tone (audio, 6.0, 72.0, 1.5, 2.0);
    tone (audio, 8.0, 84.0, 1.0, 1.5);

    const auto slices = analyseSample (audio, rate);
    REQUIRE (slices.size() == 5);
    CHECK (slices[2].start == static_cast<juce::int64> (3.95 * rate));
    CHECK (slices[1].end == slices[2].start);
    CHECK (slices[4].end == audio.getNumSamples());

    for (size_t i = 0; i < slices.size(); ++i)
        CHECK_THAT (slices[i].pitch, WithinAbs (36.0 + 12.0 * static_cast<double> (i), 0.3));
}

TEST_CASE ("numbered files without notes go up together, one found an octave off put back", "[sampler][mapping]")
{
    const auto zones = planSampleZones (
        { named ("key01", 60.0), named ("key02", 73.1), named ("key03", 62.0), named ("key04", 62.9) });
    REQUIRE (zones.has_value());
    REQUIRE (zones->size() == 4);
    CHECK ((*zones)[1].rootKey == 61);
    CHECK ((*zones)[1].file == 1);
    CHECK ((*zones)[3].rootKey == 63);
}

TEST_CASE ("a sample's name gives its side, microphone and take, and the piece they share", "[sampler][mapping]")
{
    const auto left = parseSampleName ("musicBox_Ab4_L-001");
    CHECK (left.side == -1);
    CHECK (left.take);
    CHECK (left.piece == "musicBox");
    CHECK (parseSampleName ("musicBox_Ab4_R-002").side == 1);

    const auto mic = parseSampleName ("gretschDrumset_china_HiHatMic");
    CHECK (mic.microphone == "HiHatMic");
    CHECK (mic.piece == "gretschDrumset china");
    CHECK (parseSampleName ("Snare_Solo_A#1_AB01_buzzpressedcenter001_48_63").piece
           == "Snare Solo AB01 buzzpressedcenter001");
    CHECK_FALSE (parseSampleName ("rototom_1").take);
}

TEST_CASE ("left and right files layer on one note, and takes share their keys", "[sampler][mapping]")
{
    const auto pair =
        planSampleZones ({ named ("box_C4_L"), named ("box_C4_R"), named ("box_C5_L"), named ("box_C5_R") });
    REQUIRE (pair.has_value());
    REQUIRE (pair->size() == 4);
    CHECK ((*pair)[0] == PlannedZone { 0, 0, 0, 60, 0, 66, 0, 127, 1, -1.0 });
    CHECK ((*pair)[1] == PlannedZone { 1, 0, 0, 60, 0, 66, 0, 127, 2, 1.0 });

    const auto takes = planSampleZones ({ named ("uke_C4-001"), named ("uke_C4-002"), named ("uke_E4-001") });
    REQUIRE (takes.has_value());
    CHECK ((*takes)[0].lowKey == (*takes)[1].lowKey);
    CHECK ((*takes)[0].highKey == (*takes)[1].highKey);
    CHECK ((*takes)[1].layer == 1);
}

TEST_CASE ("a drum kit's microphones layer each piece on its key", "[sampler][mapping]")
{
    const auto zones = planSampleZones ({ named ("kit_snare_OHLMic"), named ("kit_kick_KickMic"),
                                          named ("kit_kick_OHLMic"), named ("kit_snare_KickMic") });
    REQUIRE (zones.has_value());
    REQUIRE (zones->size() == 4);
    // The kick on 36, the snare on 37; the KickMic in layer 1, the OHLMic in layer 2.
    CHECK ((*zones)[0].rootKey == 36);
    CHECK ((*zones)[0].file == 1);
    CHECK ((*zones)[0].layer == 1);
    CHECK ((*zones)[1].file == 2);
    CHECK ((*zones)[1].layer == 2);
    CHECK ((*zones)[2].rootKey == 37);
}

TEST_CASE ("takes of one unnamed piece share the note most of them were found at", "[sampler][mapping]")
{
    const auto zones =
        planSampleZones ({ named ("rototom_3_01", 40.0), named ("rototom_3_02", 40.2), named ("rototom_3_04", 58.0),
                           named ("rototom_1_05", 64.0), named ("rototom_1_01", 64.1) });
    REQUIRE (zones.has_value());
    REQUIRE (zones->size() == 5);
    CHECK ((*zones)[2].rootKey == 40);
    CHECK ((*zones)[2].file == 2);
    CHECK ((*zones)[3].rootKey == 64);
    CHECK ((*zones)[4].lowVelocity == 0); // 1_05 is a take, not a velocity range
    CHECK (parseSampleName ("rototom_1_05").lowVelocity == -1);
}
