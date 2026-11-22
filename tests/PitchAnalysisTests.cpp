// Pitch detection for pitch correction (docs/design/pitch-correction.md, section 3).

#include "dawg/PitchAnalysis.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <iostream>

using namespace dawg;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double rate = 48000.0;
constexpr double hop = 0.01;

[[nodiscard]] double hzOf (double pitch)
{
    return 440.0 * std::pow (2.0, (pitch - 69.0) / 12.0);
}

// A track at 10 ms from a pitch function (NaN for unvoiced) over `seconds`, at -20 dBFS where voiced.
template <typename Pitch>
[[nodiscard]] PitchTrack trackOf (double seconds, Pitch&& pitchAt)
{
    PitchTrack track;
    track.sampleRate = rate;
    track.hopSeconds = hop;

    for (auto i = 0; i * hop < seconds; ++i)
    {
        const auto p = pitchAt (i * hop);
        track.pitch.push_back (static_cast<float> (p));
        track.voicedProb.push_back (std::isnan (p) ? 0.0f : 0.9f);
        track.levelDb.push_back (std::isnan (p) ? -80.0f : -20.0f);
    }

    return track;
}

// A salience in CREPE's bins with a Gaussian bump (40 cents wide) at a pitch per frame; NaN gives a flat, low frame.
[[nodiscard]] PitchSalience salienceOf (const std::vector<double>& pitches)
{
    PitchSalience salience;
    salience.hopSeconds = hop;

    for (int k = 0; k < 360; ++k)
        salience.binPitches.push_back (static_cast<float> (69.0 + (1997.3794084376191 + 20.0 * k - 6551.3179) / 100.0));

    for (const auto pitch : pitches)
        for (const auto bin : salience.binPitches)
            salience.values.push_back (
                std::isnan (pitch) ? 0.02f
                                   : static_cast<float> (0.9 * std::exp (-0.5 * std::pow ((bin - pitch) / 0.4, 2.0))));

    return salience;
}

// The shape's largest distance from a function of seconds from the note start.
template <typename Expected>
[[nodiscard]] double shapeError (const DetectedPitchNote& note, double from, double to, Expected&& expected)
{
    auto largest = 0.0;

    for (auto t = from; t <= to; t += 0.005)
    {
        const auto value = evaluateCurve (note.shape, t * rate, 0.0);
        largest = std::max (largest, std::abs (value - expected (t)));
    }

    return largest;
}

// A tone at 48 kHz: a pitch per time, with harmonics (gain per harmonic), 5 ms ramps, 0 for silence.
template <typename Pitch>
[[nodiscard]] juce::AudioBuffer<float> toneOf (double seconds, Pitch&& pitchAt, std::initializer_list<double> harmonics)
{
    juce::AudioBuffer<float> audio (1, static_cast<int> (seconds * rate));
    audio.clear();
    auto phase = 0.0;

    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto pitch = pitchAt (i / rate);

        if (pitch <= 0.0)
            continue;

        phase += juce::MathConstants<double>::twoPi * hzOf (pitch) / rate;
        auto sample = 0.0;
        auto n = 1;

        for (const auto gain : harmonics)
            sample += gain * std::sin (n++ * phase);

        audio.setSample (0, i, static_cast<float> (0.3 * sample));
    }

    return audio;
}

[[nodiscard]] std::unique_ptr<PitchModel> modelNamed (const juce::String& name, int threads = 1)
{
    juce::String error;
    auto model = loadPitchModel (pitchModelsFolder().getChildFile (name + ".onnx"), error, threads);
    INFO (error);
    REQUIRE (model != nullptr);
    return model;
}
} // namespace

TEST_CASE ("pitch decoding follows the salience and removes an octave jump", "[pitch]")
{
    std::vector<double> pitches;

    for (int i = 0; i < 100; ++i)
        pitches.push_back (i < 10 || i >= 90 ? std::nan ("") : 60.0 + 0.003 * i);

    pitches[50] = 72.0; // one frame an octave up

    const auto track = decodePitch (salienceOf (pitches), 0.5f, {}, rate);
    REQUIRE (track.size() == 100);
    CHECK (! track.voiced (5));
    CHECK (! track.voiced (95));

    for (int i = 12; i < 88; ++i)
    {
        INFO ("frame " << i);
        REQUIRE (track.voiced (static_cast<size_t> (i)));
        CHECK_THAT (track.pitch[static_cast<size_t> (i)], WithinAbs (60.0 + 0.003 * i, i == 50 ? 0.5 : 0.05));
    }
}

TEST_CASE ("pitch decoding: quiet frames are unvoiced", "[pitch]")
{
    const std::vector<double> pitches (50, 64.0);
    std::vector<float> levels (50, -20.0f);
    std::fill (levels.begin() + 20, levels.begin() + 30, -80.0f);

    const auto track = decodePitch (salienceOf (pitches), 0.5f, levels, rate);
    CHECK (track.voiced (10));
    CHECK (! track.voiced (25));
    CHECK (track.voiced (40));
}

TEST_CASE ("pitch notes: a melody with a glide", "[pitch]")
{
    // 60 for 0.4 s, a 40 ms glide to 64, 64 for 0.4 s, silence, 62 for 0.3 s.
    const auto track = trackOf (1.3,
                                [] (double t)
                                {
                                    if (t < 0.4)
                                        return 60.0;
                                    if (t < 0.44)
                                        return 60.0 + 4.0 * (t - 0.4) / 0.04;
                                    if (t < 0.84)
                                        return 64.0;
                                    if (t < 0.95)
                                        return std::nan ("");
                                    if (t < 1.25)
                                        return 62.0;
                                    return std::nan ("");
                                });

    const auto notes = findPitchNotes (track, {});
    REQUIRE (notes.size() == 3);
    CHECK_THAT (notes[0].centre, WithinAbs (60.0, 0.05));
    CHECK_THAT (notes[1].centre, WithinAbs (64.0, 0.05));
    CHECK_THAT (notes[2].centre, WithinAbs (62.0, 0.05));
    CHECK (notes[0].end == notes[1].start); // split inside the glide, not apart
    CHECK_THAT (static_cast<double> (notes[1].start) / rate, WithinAbs (0.42, 0.025));
    CHECK_THAT (static_cast<double> (notes[2].start) / rate, WithinAbs (0.95, 0.011));
}

TEST_CASE ("pitch analysis as the bridge sends it", "[pitch]")
{
    const auto track = trackOf (1.0, [] (double t) { return t < 0.5 ? 60.0 : std::nan (""); });
    PitchAnalysis analysis { "1/test", track, findPitchNotes (track, {}) };
    const auto result = pitchAnalysisToVar (analysis);

    CHECK (result["analyser"].toString() == "1/test");
    CHECK_THAT (static_cast<double> (result["hop"]), WithinAbs (track.hopSeconds * 2.0, 1e-9));
    REQUIRE (result["pitch"].size() == static_cast<int> ((track.size() + 1) / 2));
    CHECK_THAT (static_cast<double> (result["pitch"][5]), WithinAbs (60.0, 0.01));
    CHECK (result["pitch"][result["pitch"].size() - 1].isVoid());

    REQUIRE (result["notes"].size() == 1);
    const auto& note = result["notes"][0];
    CHECK_THAT (static_cast<double> (note["centre"]), WithinAbs (60.0, 0.01));
    CHECK (static_cast<juce::int64> (note["end"]) == analysis.notes[0].end);
    REQUIRE (note["shape"].size() == static_cast<int> (analysis.notes[0].shape.size()));
    CHECK (static_cast<int> (note["shape"][0][2]) == static_cast<int> (CurveShape::smooth));
}

TEST_CASE ("pitch notes: vibrato stays one note, with its rate and depth, and out of the shape", "[pitch]")
{
    const auto track =
        trackOf (1.0, [] (double t) { return 67.0 + 0.5 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t); });

    const auto notes = findPitchNotes (track, {});
    REQUIRE (notes.size() == 1);
    CHECK_THAT (notes[0].centre, WithinAbs (67.0, 0.05));
    CHECK_THAT (notes[0].vibratoRate, WithinAbs (5.5, 0.4));
    CHECK_THAT (notes[0].vibratoDepth, WithinAbs (50.0, 10.0));
    CHECK (shapeError (notes[0], 0.15, 0.85, [] (double) { return 0.0; }) < 0.15);
}

TEST_CASE ("pitch notes: a repeated note splits at an onset with a dip before it", "[pitch]")
{
    auto track = trackOf (1.0, [] (double) { return 60.0; });

    for (int i = 45; i < 50; ++i)
        track.levelDb[static_cast<size_t> (i)] = -35.0f;

    const auto notes = findPitchNotes (track, { Onset { static_cast<juce::int64> (0.5 * rate), 1.0f } });
    REQUIRE (notes.size() == 2);
    CHECK_THAT (static_cast<double> (notes[1].start) / rate, WithinAbs (0.5, 0.011));

    // Without the dip the onset doesn't split it.
    const auto steady = trackOf (1.0, [] (double) { return 60.0; });
    CHECK (findPitchNotes (steady, { Onset { static_cast<juce::int64> (0.5 * rate), 1.0f } }).size() == 1);
}

TEST_CASE ("pitch notes: an unvoiced lead-in belongs to the next note", "[pitch]")
{
    auto track = trackOf (1.0, [] (double t) { return t < 0.4 || t >= 0.9 ? std::nan ("") : 62.0; });

    for (int i = 30; i < 40; ++i)
        track.levelDb[static_cast<size_t> (i)] = -30.0f; // a consonant: loud, unvoiced

    const auto notes = findPitchNotes (track, {});
    REQUIRE (notes.size() == 1);
    CHECK_THAT (static_cast<double> (notes[0].leadStart) / rate, WithinAbs (0.3, 0.011));
    CHECK_THAT (static_cast<double> (notes[0].start) / rate, WithinAbs (0.4, 0.011));
}

TEST_CASE ("pitch notes: the shape fits a scoop, a drift and a fall with few nodes", "[pitch]")
{
    // A 1-semitone scoop into the note over 60 ms, a slow 30-cent drift, a 2-semitone fall over the last 100 ms.
    auto deviation = [] (double t)
    {
        auto d = 0.3 * t / 0.8;
        if (t < 0.06)
            d -= 1.0 * std::pow (1.0 - t / 0.06, 2.0);
        if (t > 0.7)
            d -= 2.0 * std::pow ((t - 0.7) / 0.1, 2.0);
        return d;
    };

    const auto track = trackOf (0.8, [&] (double t) { return 65.0 + deviation (t); });
    const auto notes = findPitchNotes (track, {});
    REQUIRE (notes.size() == 1);
    const auto& note = notes[0];

    CHECK (note.shape.size() <= 5);
    CHECK (shapeError (note, 0.0, 0.79, [&] (double t) { return 65.0 + deviation (t) - note.centre; }) < 0.2);

    for (size_t k = 0; k + 1 < note.shape.size(); ++k)
        CHECK (note.shape[k].shape == CurveShape::smooth);

    // measureCentre and fitShape give the same as the note's own.
    CHECK_THAT (measureCentre (track, note.start, note.end), WithinAbs (note.centre, 1.0e-9));
    CHECK (fitShape (track, note.start, note.end, note.centre).size() == note.shape.size());
}

TEST_CASE ("pitch notes: a short note's shape keeps a dip before the next note", "[pitch]")
{
    // A 0.25 s note that scoops down from +0.8, sags to -0.8 just before its end and rises into a note 1.5 semitones
    // up, as in the owner's 17-20-19 take. A free segment can't pass beyond its nodes, so the dip needs its own node.
    auto first = [] (double t)
    {
        if (t < 0.03)
            return 0.8 - 0.8 * t / 0.03;
        if (t < 0.2)
            return -0.8 * (t - 0.03) / 0.17;
        return -0.8 + 1.0 * (t - 0.2) / 0.05;
    };

    const auto track = trackOf (0.6,
                                [&] (double t)
                                {
                                    if (t < 0.25)
                                        return 56.0 + first (t);
                                    if (t < 0.29)
                                        return 56.2 + 1.3 * (t - 0.25) / 0.04;
                                    return 57.5;
                                });

    const auto notes = findPitchNotes (track, {});
    REQUIRE (notes.size() == 2);
    const auto& note = notes[0];
    CHECK (note.shape.size() >= 3);
    CHECK (shapeError (note, 0.05, static_cast<double> (note.end - note.start) / rate - 0.02,
                       [&] (double t) { return 56.0 + first (t) - note.centre; })
           < 0.3);
}

TEST_CASE ("pitch preparation resamples to the model's rate", "[pitch]")
{
    const auto tone = toneOf (1.0, [] (double) { return 69.0; }, { 1.0 });
    const auto prepared = preparePitchAudio (tone, rate, 16000.0);
    CHECK (prepared.size() == 16000);

    // Still 440 Hz: count the rising zero crossings over the middle half second.
    auto crossings = 0;
    for (size_t i = 4001; i < 12000; ++i)
        if (prepared[i - 1] < 0.0f && prepared[i] >= 0.0f)
            ++crossings;

    CHECK (std::abs (crossings - 220) <= 1);
}

TEST_CASE ("pitch analysis with the neural models", "[pitch]")
{
    // 57, 60 with a strong second harmonic, 64, 62 sliding to 64, each 0.4 s with 0.1 s of silence between.
    auto melody = [] (double t)
    {
        if (t < 0.4)
            return 57.0;
        if (t >= 0.5 && t < 0.9)
            return 60.0;
        if (t >= 1.0 && t < 1.4)
            return 64.0;
        if (t >= 1.5 && t < 1.9)
            return 62.0 + 2.0 * std::clamp ((t - 1.7) / 0.1, 0.0, 1.0);
        return 0.0;
    };

    const auto audio = toneOf (2.0, melody, { 1.0, 0.5, 0.3, 0.2 });

    for (const auto* name : { "fcpe", "crepe-tiny" })
    {
        DYNAMIC_SECTION (name)
        {
            const auto model = modelNamed (name);
            const auto analysis = analysePitch (audio, rate, *model);
            REQUIRE (analysis.track.size() > 150);
            CHECK (analysis.analyser.startsWith (juce::String (pitchAnalysisVersion) + "/" + name + ":"));

            // Frames in the middle of each note within 5 cents (the slide: before it).
            for (const auto t : { 0.2, 0.7, 1.2, 1.6 })
            {
                const auto index =
                    static_cast<size_t> (std::lround (analysis.track.indexOf (static_cast<juce::int64> (t * rate))));
                INFO ("at " << t << " s");
                REQUIRE (analysis.track.voiced (index));
                CHECK_THAT (analysis.track.pitch[index], WithinAbs (melody (t), 0.05));
            }

            CHECK (! analysis.track.voiced (
                static_cast<size_t> (std::lround (analysis.track.indexOf (static_cast<juce::int64> (0.95 * rate))))));

            REQUIRE (analysis.notes.size() >= 4);
            CHECK_THAT (analysis.notes[0].centre, WithinAbs (57.0, 0.05));
            CHECK_THAT (analysis.notes[1].centre, WithinAbs (60.0, 0.05));
            CHECK_THAT (analysis.notes[2].centre, WithinAbs (64.0, 0.05));

            // The same every run, so caches and tests can rely on it.
            const auto again = analysePitch (audio, rate, *model);
            CHECK (again.track.pitch.size() == analysis.track.pitch.size());
            CHECK (std::equal (again.track.voicedProb.begin(), again.track.voicedProb.end(),
                               analysis.track.voicedProb.begin()));
        }
    }
}

// The throwaway look-at-it tool (section 7, step 1): analyses DAWG_PITCH_INPUT (a file or a folder of them) with
// the model DAWG_PITCH_MODEL (default fcpe; several separated by commas) on DAWG_PITCH_THREADS threads (default 1),
// and writes <file>.<model>.pitch.json, with the time taken, into DAWG_PITCH_OUTPUT (default: next to each file).
// Run with: dawg_tests "[pitch-tool]".
TEST_CASE ("pitch analysis tool", "[.][pitch-tool]")
{
    const auto input = juce::SystemStats::getEnvironmentVariable ("DAWG_PITCH_INPUT", {});
    REQUIRE (input.isNotEmpty());
    const auto models =
        juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("DAWG_PITCH_MODEL", "fcpe"), ",", "");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File root (input);
    juce::Array<juce::File> files;

    if (root.isDirectory())
        files = root.findChildFiles (juce::File::findFiles, false, formats.getWildcardForAllFormats());
    else
        files.add (root);

    for (const auto& modelName : models)
    {
        const auto model = modelNamed (
            modelName.trim(), juce::SystemStats::getEnvironmentVariable ("DAWG_PITCH_THREADS", "1").getIntValue());

        for (const auto& file : files)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

            if (reader == nullptr)
                continue;

            juce::AudioBuffer<float> audio (static_cast<int> (reader->numChannels),
                                            static_cast<int> (reader->lengthInSamples));
            reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);

            const auto began = std::chrono::steady_clock::now();
            const auto analysis = analysePitch (audio, reader->sampleRate, *model);
            const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - began).count();

            // The model's share of that time.
            const auto modelBegan = std::chrono::steady_clock::now();
            [[maybe_unused]] const auto salience =
                model->salience (preparePitchAudio (audio, reader->sampleRate, model->sampleRate()), {});
            const auto modelSeconds =
                std::chrono::duration<double> (std::chrono::steady_clock::now() - modelBegan).count();
            const auto decodeBegan = std::chrono::steady_clock::now();
            const auto decoded = decodePitch (salience, model->voicingThreshold(), {}, reader->sampleRate);
            const auto decodeSeconds =
                std::chrono::duration<double> (std::chrono::steady_clock::now() - decodeBegan).count();
            const auto onsetsBegan = std::chrono::steady_clock::now();
            const auto onsets = detectOnsets (audio, reader->sampleRate);
            const auto onsetSeconds =
                std::chrono::duration<double> (std::chrono::steady_clock::now() - onsetsBegan).count();
            const auto notesBegan = std::chrono::steady_clock::now();
            [[maybe_unused]] const auto found = findPitchNotes (analysis.track, onsets);
            const auto notesSeconds =
                std::chrono::duration<double> (std::chrono::steady_clock::now() - notesBegan).count();
            std::cout << "decode " << decodeSeconds << " onsets " << onsetSeconds << " notes " << notesSeconds
                      << std::endl;
            const auto length = audio.getNumSamples() / reader->sampleRate;

            std::cout << file.getFileName() << " (" << length << " s), " << modelName << ": " << seconds
                      << " s (the model " << modelSeconds << " s), " << analysis.notes.size() << " notes\n";

            auto* json = new juce::DynamicObject();
            json->setProperty ("file", file.getFullPathName());
            json->setProperty ("analyser", analysis.analyser);
            json->setProperty ("seconds", seconds);
            json->setProperty ("sampleRate", reader->sampleRate);
            json->setProperty ("hopSeconds", analysis.track.hopSeconds);
            json->setProperty ("startSeconds", analysis.track.startSeconds);

            juce::Array<juce::var> pitch, voiced, level;
            for (size_t i = 0; i < analysis.track.size(); ++i)
            {
                pitch.add (analysis.track.voiced (i) ? juce::var (analysis.track.pitch[i]) : juce::var());
                voiced.add (analysis.track.voicedProb[i]);
                level.add (analysis.track.levelDb[i]);
            }
            json->setProperty ("pitch", pitch);
            json->setProperty ("voicedProb", voiced);
            json->setProperty ("levelDb", level);

            juce::Array<juce::var> notes;
            for (const auto& note : analysis.notes)
            {
                auto* n = new juce::DynamicObject();
                n->setProperty ("leadStart", note.leadStart);
                n->setProperty ("start", note.start);
                n->setProperty ("end", note.end);
                n->setProperty ("centre", note.centre);
                n->setProperty ("vibratoRate", note.vibratoRate);
                n->setProperty ("vibratoDepth", note.vibratoDepth);

                juce::Array<juce::var> shape;
                for (const auto& point : note.shape)
                {
                    auto* p = new juce::DynamicObject();
                    p->setProperty ("time", point.time);
                    p->setProperty ("value", point.value);
                    p->setProperty ("shape", point.shape == CurveShape::smooth ? "smooth"
                                             : point.shape == CurveShape::free ? "free"
                                                                               : "linear");
                    p->setProperty ("handleTime", point.handleTime);
                    p->setProperty ("handleValue", point.handleValue);
                    shape.add (juce::var (p));
                }
                n->setProperty ("shape", shape);
                notes.add (juce::var (n));
            }
            json->setProperty ("notes", notes);

            const auto outputFolder = juce::SystemStats::getEnvironmentVariable ("DAWG_PITCH_OUTPUT", {});
            const auto name = file.getFileNameWithoutExtension() + "." + modelName.trim() + ".pitch.json";
            const auto out =
                outputFolder.isEmpty() ? file.getSiblingFile (name) : juce::File (outputFolder).getChildFile (name);
            out.getParentDirectory().createDirectory();
            out.replaceWithText (juce::JSON::toString (juce::var (json)));
        }
    }
}
