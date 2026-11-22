#include "TestUtils.h"
#include "dawg/DrumEnvelopes.h"
#include "dawg/DrumVoices.h"
#include "dawg/Svf.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <tuple>
#include <vector>

// The prototypes behind the Drum Synth's modal cymbal (docs/design/drum-synth.md, sections 3.5 and 9.2, 2026-11-20),
// kept to try changes by ear. Hidden, each writing WAVs at the same peak level into DAWG_DRUM_WAVS and printing how
// tonal each is: [.cymbal-proto] a noise-and-shimmer cymbal, [.cymbal-modal] modal ones (the owner chose the hybrid),
// [.cymbal-engine] the engine's crash and a ride pattern on one restruck voice, [.cymbal-level] the engine's loudness.
//
// The prototype: a wash of noise through a low-pass that closes as the cymbal rings (its body), shimmer from two banks
// of six squares multiplied together (dozens of unrelated partials, not six), and a ping of a stick click and eight
// short inharmonic partials at 2-7 kHz (in place of the bell's four sines). Its level falls fast, then slowly.

using namespace dawg;
using namespace dawg::test;

namespace
{
constexpr double rate = 48000.0;

struct Cymbal
{
    double toneStart = 12000.0; // the wash's low-pass at the hit, Hz
    double toneEnd = 4000.0;    // where it closes to
    float wash = 0.75f;         // wash against shimmer
    float ping = 0.15f;
    double swell = 0.015; // s
    double decay = 3.5;   // s to -60 dB
    float width = 0.7f;
    double pingDecay = 0.35;   // s, the partials' longest
    float splash = 0.0f;       // a loud, short burst of the wash at the hit, over its usual level
    double splashDecay = 0.15; // s, the splash's fall to -60 dB
};

constexpr std::array<double, 8> pingRatios { 1.0, 1.32, 1.57, 1.83, 2.11, 2.38, 2.71, 3.07 };
constexpr std::array<float, 8> pingGains { 1.0f, 0.8f, 0.7f, 0.6f, 0.5f, 0.45f, 0.4f, 0.3f };
constexpr double pingBase = 2150.0;

[[nodiscard]] float white (std::uint32_t& state)
{
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return static_cast<float> (static_cast<std::int32_t> (state)) / 2147483648.0f;
}

// One hit of the prototype, added into two channels from a sample on.
void addPrototype (const Cymbal& c, std::uint32_t seed, std::vector<float>& left, std::vector<float>& right, size_t at,
                   float velocity = 1.0f)
{
    std::uint32_t noiseL = seed * 2654435761U | 1U;
    std::uint32_t noiseR = (seed + 77U) * 2246822519U | 1U;
    DrumMetal a;
    DrumMetal b;
    a.start (1.0, rate);
    b.start (1.4142, rate);

    // Two envelopes, summed: a fast early drop and a long tail.
    DrumEnvelope early;
    DrumEnvelope tail;
    early.start (rate, c.swell, 0.0, 0.12 * c.decay);
    tail.start (rate, c.swell, 0.0, c.decay);
    DrumEnvelope splash;
    splash.start (rate, 0.002, 0.0, c.splashDecay);
    DrumEnvelope click;
    click.start (rate, 0.0, 0.0, 0.004);
    std::array<DrumEnvelope, 8> partials {};
    std::array<double, 8> phases {};

    for (size_t i = 0; i < partials.size(); ++i)
        partials[i].start (rate, 0.0, 0.0, c.pingDecay * (1.0 - 0.07 * static_cast<double> (i)));

    const auto highPass = SvfCoefficients::bilinear (SvfMode::highPass, 400.0, std::numbers::sqrt2, rate);
    const auto shimmerPass = SvfCoefficients::bilinear (SvfMode::highPass, 3000.0, std::numbers::sqrt2, rate);
    const auto clickPass = SvfCoefficients::bilinear (SvfMode::bandPass, 4500.0, 1.0, rate);
    std::array<SvfState, 2> washHigh {};
    std::array<SvfState, 2> washLow {};
    std::array<SvfState, 2> shimmerHigh {};
    std::array<SvfState, 2> shimmerHigh2 {};
    SvfState clickState;
    SvfCoefficients lowPass;
    const auto tone = std::exp (-1.0 / (0.35 * c.decay * rate));
    auto cutoff = c.toneStart;

    for (size_t n = at; n < left.size(); ++n)
    {
        // The wash closes as it rings; its coefficients follow every 32 samples.
        if ((n - at) % 32 == 0)
            lowPass = SvfCoefficients::bilinear (SvfMode::lowPass, cutoff, 0.9, rate);

        cutoff = c.toneEnd + (cutoff - c.toneEnd) * tone;
        const auto level = 0.6f * early.next() + 0.4f * tail.next() + c.splash * splash.next();

        const auto metal = static_cast<float> (a.next() * b.next());
        const auto nl = white (noiseL);
        const auto nr = (1.0f - c.width) * nl + c.width * white (noiseR);

        float ping = 0.0f;

        for (size_t i = 0; i < partials.size(); ++i)
        {
            phases[i] += pingBase * pingRatios[i] / rate;
            phases[i] -= std::floor (phases[i]);
            ping +=
                static_cast<float> (std::sin (2.0 * std::numbers::pi * phases[i])) * pingGains[i] * partials[i].next();
        }

        ping = 0.12f * ping + clickState.process (clickPass, white (noiseL)) * click.next();

        const std::array<float, 2> noises { nl, nr };
        std::array<float, 2> out {};

        for (size_t ch = 0; ch < 2; ++ch)
        {
            const auto wash = washLow[ch].process (lowPass, washHigh[ch].process (highPass, noises[ch]));
            const auto shimmer = shimmerHigh2[ch].process (shimmerPass, shimmerHigh[ch].process (shimmerPass, metal));
            out[ch] = (c.wash * wash + (1.0f - c.wash) * 1.5f * shimmer) * level + c.ping * ping;
        }

        left[n] += out[0] * velocity;
        right[n] += out[1] * velocity;
    }
}

// One hit of the current model.
void addCurrent (const DrumHit& hit, std::vector<float>& left, std::vector<float>& right, size_t at)
{
    DrumVoice voice;
    voice.prepare (rate);
    voice.trigger (hit);
    voice.render (left.data() + at, right.data() + at, static_cast<int> (left.size() - at));
}

void normalise (std::vector<float>& left, std::vector<float>& right)
{
    float peak = 1.0e-9f;

    for (size_t i = 0; i < left.size(); ++i)
        peak = std::max ({ peak, std::abs (left[i]), std::abs (right[i]) });

    const auto gain = 0.5f / peak; // -6 dBFS

    for (size_t i = 0; i < left.size(); ++i)
    {
        left[i] *= gain;
        right[i] *= gain;
    }
}

// How tonal a sound is 0.3 s in, from 500 Hz to 12 kHz: its loudest bin over the median bin, in dB, and its spectral
// flatness (1 for white noise, near 0 for a few tones). A bell-like cymbal has a high first and a low second.
void printTonality (const char* name, const std::vector<float>& left)
{
    constexpr int order = 14;
    constexpr int size = 1 << order;
    const auto start = static_cast<size_t> (0.3 * rate);
    std::vector<float> data (2 * size, 0.0f);

    for (size_t i = 0; i < size && start + i < left.size(); ++i)
        data[i] = left[start + i]
                  * static_cast<float> (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * static_cast<double> (i) / size));

    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (data.data());
    std::vector<double> band;

    for (int b = 1; b <= size / 2; ++b)
        if (const auto f = b * rate / size; f > 500.0 && f < 12000.0)
            band.push_back (static_cast<double> (data[static_cast<size_t> (b)]));

    auto sorted = band;
    std::ranges::sort (sorted);
    double logSum = 0.0;
    double sum = 0.0;

    for (const auto m : band)
    {
        logSum += std::log (m * m + 1.0e-20);
        sum += m * m;
    }

    const auto flatness =
        std::exp (logSum / static_cast<double> (band.size())) / (sum / static_cast<double> (band.size()));
    std::printf ("  %-24s peak/median %5.1f dB, flatness %.3f%s", name,
                 20.0 * std::log10 (sorted.back() / sorted[sorted.size() / 2]), flatness, "\n");
}

void write (const juce::File& directory, const char* name, std::vector<float> left, std::vector<float> right)
{
    printTonality (name, left);
    normalise (left, right);
    const auto file = directory.getChildFile (name);
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    const std::array<const float*, 2> channels { left.data(), right.data() };
    REQUIRE (writer->writeFromFloatArrays (channels.data(), 2, static_cast<int> (left.size())));
}

[[nodiscard]] DrumHit currentCrash()
{
    return DrumHit::defaults (DrumModel::cymbal);
}

[[nodiscard]] DrumHit currentRide()
{
    return DrumHit::defaults (DrumModel::cymbal)
        .set ("ride", 100.0f)
        .set ("tone", 9000.0f)
        .set ("colour", 50.0f)
        .set ("wash", 25.0f)
        .set ("sizzle", 12.0f)
        .set ("swell", 0.0f)
        .set ("width", 40.0f);
}

const Cymbal crash {};
const Cymbal ride { 9000.0, 5000.0, 0.55f, 0.6f, 0.0, 4.0, 0.4f, 0.6 };
} // namespace

TEST_CASE ("Cymbal prototype: write crash and ride WAVs, current and new", "[.cymbal-proto]")
{
    const auto folder = juce::SystemStats::getEnvironmentVariable ("DAWG_DRUM_WAVS", {});
    REQUIRE (folder.isNotEmpty());
    const juce::File directory (folder);
    REQUIRE (directory.createDirectory());

    const auto seconds = [] (double s) { return static_cast<size_t> (s * rate); };
    const auto eighth = seconds (0.25); // at 120 BPM

    // A crash hit, 4 s.
    {
        std::vector<float> l (seconds (4.0)), r (l.size());
        addCurrent (currentCrash(), l, r, 0);
        write (directory, "crash-current.wav", l, r);
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        addPrototype (crash, 1, l, r, 0);
        write (directory, "crash-new.wav", l, r);
    }

    // Eighth rides for two bars, then a second to ring out; accents on the beats.
    {
        std::vector<float> l (seconds (5.0)), r (l.size());

        for (size_t k = 0; k < 16; ++k)
        {
            auto hit = currentRide();
            hit.velocity = k % 2 == 0 ? 120.0f : 85.0f;
            hit.seed = static_cast<std::uint32_t> (k + 1);
            addCurrent (hit, l, r, k * eighth);
        }

        write (directory, "ride-current.wav", l, r);
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);

        for (size_t k = 0; k < 16; ++k)
            addPrototype (ride, static_cast<std::uint32_t> (k + 1), l, r, k * eighth, k % 2 == 0 ? 1.0f : 0.7f);

        write (directory, "ride-new.wav", l, r);
    }

    // The ride pattern with a crash on the first beat.
    {
        std::vector<float> l (seconds (5.0)), r (l.size());

        for (size_t k = 0; k < 16; ++k)
            addPrototype (ride, static_cast<std::uint32_t> (k + 1), l, r, k * eighth, k % 2 == 0 ? 0.5f : 0.35f);

        addPrototype (crash, 99, l, r, 0, 1.0f);
        write (directory, "crash-and-ride-new.wav", l, r);
    }
}

// ---------------------------------------------------------------------------------------------------------
// A modal cymbal (2026-11-20, after "too noisy"): a bank of two-pole resonators at many inharmonic frequencies,
// denser towards the top as on a thin plate, each with its own decay and place in the stereo field, rung by a short
// band-limited stick burst. Close modes beat against each other, which is the shimmer; a little high noise that
// swells after the hit is the sizzle. One bank per cymbal: a new hit rings the same bank again, as on a real ride.

namespace
{
struct Modal
{
    int modes = 160;
    double low = 300.0;       // Hz, the lowest mode
    double high = 16000.0;    // Hz, the highest
    double density = 0.5;     // modes per Hz grow as f^density
    double t60 = 2.5;         // s, a mode's decay to -60 dB at 1 kHz
    double tilt = -0.15;      // how decay changes with frequency: t60 * (f / 1 kHz)^tilt
    double hardness = 6000.0; // Hz, the stick's brightness (the burst's low-pass)
    double strike = 0.003;    // s, the burst's decay
    float sizzle = 0.06f;     // high noise swelling after the hit
    double bloom = 0.06;      // s, the sizzle's rise
    float width = 0.6f;
    std::uint32_t seed = 7;
    double lift = 0.0; // gain grows as (f / 1 kHz)^lift: above 0 the low modes are quieter
    float wash = 0.0f; // some of the first prototype's wash under the modes
    // Idea 1 (2026-11-21): energy spreading upwards. A loud hit bends the plate non-linearly, which passes energy from
    // the low modes to the high ones. Here the modes' sum, soft-clipped, comes back in as excitation: what the clip
    // takes off (cubic for small signals, so nothing for soft hits) rings the other modes.
    float coupling = 0.0f;      // how much of the clipped-off part comes back
    float drive = 1.0f;         // how early the clip bites
    double couplingDecay = 0.3; // s: the coupling fades out after a hit (to -60 dB), so no setting rings forever
    // Idea 2: a splash, the wash louder and brighter for the first fraction of a second.
    float splash = 0.0f;
    double splashStart = 10000.0; // Hz, the wash's low-pass at the hit
};

[[nodiscard]] float softClip (float x)
{
    const auto c = std::clamp (x, -3.0f, 3.0f);
    return c * (27.0f + c * c) / (27.0f + 9.0f * c * c);
}

struct ModalBank
{
    explicit ModalBank (const Modal& m)
        : spec (m)
    {
        std::uint32_t rng = m.seed * 2654435761U | 1U;
        const auto uniform = [&rng] { return 0.5f * (white (rng) + 1.0f); };
        const auto p = m.density + 1.0;
        const auto lowP = std::pow (m.low, p);
        const auto highP = std::pow (m.high, p);

        for (int i = 0; i < m.modes; ++i)
        {
            // A frequency from a density growing as f^density, its decay, its gain from the stick's spectrum.
            const auto f = std::pow (lowP + static_cast<double> (uniform()) * (highP - lowP), 1.0 / p);
            const auto t60 = m.t60 * std::pow (f / 1000.0, m.tilt) * (0.7 + 0.6 * static_cast<double> (uniform()));
            const auto r = std::pow (10.0, -3.0 / (t60 * rate));
            const auto w = 2.0 * std::numbers::pi * f / rate;
            a1.push_back (static_cast<float> (2.0 * r * std::cos (w)));
            a2.push_back (static_cast<float> (-r * r));
            const auto stick = 1.0 / (1.0 + std::pow (f / m.hardness, 2.0));
            const auto g = static_cast<float> ((0.4 + 0.6 * static_cast<double> (uniform())) * stick * std::sin (w)
                                               * std::pow (f / 1000.0, m.lift));
            const auto pan = (2.0f * uniform() - 1.0f) * m.width;
            gainL.push_back (g * std::sqrt (0.5f * (1.0f - pan)));
            gainR.push_back (g * std::sqrt (0.5f * (1.0f + pan)));
        }

        y1.assign (a1.size(), 0.0f);
        y2.assign (a1.size(), 0.0f);
        stickPass = SvfCoefficients::bilinear (SvfMode::lowPass, m.hardness, std::numbers::sqrt2, rate);
        sizzlePass = SvfCoefficients::bilinear (SvfMode::highPass, 7000.0, std::numbers::sqrt2, rate);
    }

    void hit (float velocity)
    {
        burst.start (rate, 0.0, 0.0, spec.strike);
        sizzleEnv.start (rate, spec.bloom, 0.0, 0.5 * spec.t60);
        couplingEnv.start (rate, 0.0, 0.0, spec.couplingDecay);
        strength = velocity;
    }

    // Renders [from, to) into two channels, adding.
    void render (std::vector<float>& left, std::vector<float>& right, size_t from, size_t to)
    {
        const auto count = a1.size();

        for (size_t n = from; n < to; ++n)
        {
            const auto x = stickState.process (stickPass, white (noise)) * burst.next() * strength + feedback;
            float l = 0.0f;
            float r = 0.0f;

            for (size_t i = 0; i < count; ++i)
            {
                const auto y = a1[i] * y1[i] + a2[i] * y2[i] + x;
                y2[i] = y1[i];
                y1[i] = y;
                l += y * gainL[i];
                r += y * gainR[i];
            }

            if (spec.coupling > 0.0f)
            {
                // A cubic stiffness, as on a plate bent hard (a Duffing term): it restores, so it stores no energy of
                // its own, but it mixes the modes' frequencies into sums and differences that ring other modes.
                const auto d = softClip (0.05f * (l + r) * spec.drive);
                feedback = -spec.coupling * couplingEnv.next() * d * d * d;
                loudest = std::max (loudest, std::abs (d));
            }

            const auto s = spec.sizzle * sizzleState.process (sizzlePass, white (noise)) * sizzleEnv.next() * strength;
            left[n] += 0.05f * l + s;
            right[n] += 0.05f * r + s;
        }
    }

    // The first prototype's wash under the modes, at the bank's level.
    void addWash (std::vector<float>& left, std::vector<float>& right,
                  const std::vector<std::pair<size_t, float>>& hits)
    {
        if (spec.wash <= 0.0f)
            return;

        Cymbal washOnly { spec.splashStart, 4000.0, 1.0f, 0.0f, 0.01, spec.t60, spec.width, 0.3, spec.splash };
        std::vector<float> l (left.size()), r (left.size());

        for (const auto& [at, velocity] : hits)
            addPrototype (washOnly, static_cast<std::uint32_t> (at + 3), l, r, at, velocity);

        for (size_t n = 0; n < left.size(); ++n)
        {
            left[n] += spec.wash * l[n];
            right[n] += spec.wash * r[n];
        }
    }

    Modal spec;
    std::vector<float> a1, a2, gainL, gainR, y1, y2;
    SvfCoefficients stickPass, sizzlePass;
    SvfState stickState, sizzleState;
    DrumEnvelope burst, sizzleEnv, couplingEnv;
    std::uint32_t noise = 12345U;
    float strength = 0.0f;
    float feedback = 0.0f;
    float loudest = 0.0f; // the clip's input at its largest
};

// Hits at samples with velocities, on one bank, `seconds` long.
void renderModal (const Modal& m, const std::vector<std::pair<size_t, float>>& hits, std::vector<float>& left,
                  std::vector<float>& right)
{
    ModalBank bank (m);
    size_t at = 0;

    for (size_t h = 0; h <= hits.size(); ++h)
    {
        const auto until = h < hits.size() ? hits[h].first : left.size();
        bank.render (left, right, at, until);
        at = until;

        if (h < hits.size())
            bank.hit (hits[h].second);
    }

    bank.addWash (left, right, hits);

    if (m.coupling > 0.0f)
        std::printf ("    the clip's input peaked at %.2f%s", bank.loudest, "\n");
}

const Modal modalCrash {};
const Modal modalRide { 120, 400.0, 14000.0, 0.3, 4.0, -0.25, 9000.0, 0.0015, 0.03f, 0.0, 0.4f, 11 };
} // namespace

TEST_CASE ("Cymbal prototype: write modal crash and ride WAVs", "[.cymbal-modal]")
{
    const auto folder = juce::SystemStats::getEnvironmentVariable ("DAWG_DRUM_WAVS", {});
    REQUIRE (folder.isNotEmpty());
    const juce::File directory (folder);
    REQUIRE (directory.createDirectory());

    const auto seconds = [] (double s) { return static_cast<size_t> (s * rate); };
    const auto eighth = seconds (0.25);
    std::vector<std::pair<size_t, float>> ridePattern;

    for (size_t k = 0; k < 16; ++k)
        ridePattern.emplace_back (k * eighth, k % 2 == 0 ? 1.0f : 0.7f);

    const auto one =
        [&] (const Modal& m, const char* name, double length, const std::vector<std::pair<size_t, float>>& hits)
    {
        std::vector<float> l (seconds (length)), r (l.size());
        const auto start = std::chrono::steady_clock::now();
        renderModal (m, hits, l, r);
        const auto ns = std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - start).count();
        std::printf ("  %-24s %d modes, %.0f ns per sample\n", name, m.modes, ns / static_cast<double> (l.size()));
        write (directory, name, l, r);
    };

    // Variants for the ear, from the most tonal to the most noisy.
    one (modalCrash, "crash-modal-a-plain.wav", 4.0, { { 0, 1.0f } });

    auto lifted = modalCrash;
    lifted.modes = 240;
    lifted.lift = 0.6;
    lifted.sizzle = 0.12f;
    one (lifted, "crash-modal-b-bright.wav", 4.0, { { 0, 1.0f } });

    auto hybrid = lifted;
    hybrid.modes = 128;
    hybrid.wash = 0.35f;
    one (hybrid, "crash-modal-c-hybrid.wav", 4.0, { { 0, 1.0f } });

    auto rideBright = modalRide;
    rideBright.lift = 0.5;
    rideBright.modes = 160;
    one (rideBright, "ride-modal-b-bright.wav", 5.0, ridePattern);

    auto rideHybrid = rideBright;
    rideHybrid.modes = 96;
    rideHybrid.wash = 0.25f;
    one (rideHybrid, "ride-modal-c-hybrid.wav", 5.0, ridePattern);
}

TEST_CASE ("Cymbal prototype: the kit's cymbals' loudness", "[.cymbal-level]")
{
    const auto rms = [] (const DrumHit& hit)
    {
        std::vector<float> l (static_cast<size_t> (2.0 * rate)), r (l.size());
        addCurrent (hit, l, r, 0);
        double sum = 0.0;
        float peak = 0.0f;

        for (size_t i = 0; i < l.size(); ++i)
        {
            sum += 0.5 * (static_cast<double> (l[i]) * l[i] + static_cast<double> (r[i]) * r[i]);
            peak = std::max ({ peak, std::abs (l[i]), std::abs (r[i]) });
        }

        return std::pair { 10.0 * std::log10 (sum / static_cast<double> (l.size())), 20.0 * std::log10 (peak) };
    };

    const auto [crashRms, crashPeak] = rms (currentCrash());
    const auto [rideRms, ridePeak] = rms (currentRide());
    std::printf ("  crash rms %.1f dB peak %.1f dB, ride rms %.1f dB peak %.1f dB%s", crashRms, crashPeak, rideRms,
                 ridePeak, "\n");
}

TEST_CASE ("Cymbal prototype: write the engine's crash and ride", "[.cymbal-engine]")
{
    const auto folder = juce::SystemStats::getEnvironmentVariable ("DAWG_DRUM_WAVS", {});
    REQUIRE (folder.isNotEmpty());
    const juce::File directory (folder);
    REQUIRE (directory.createDirectory());
    const auto seconds = [] (double s) { return static_cast<size_t> (s * rate); };

    const auto timed = [] (const char* name, size_t samples, auto&& body)
    {
        const juce::ScopedNoDenormals noDenormals;
        const auto start = std::chrono::steady_clock::now();
        body();
        const auto ns = std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - start).count();
        std::printf ("  %-24s %.0f ns per sample%s", name, ns / static_cast<double> (samples), "\n");
    };

    {
        std::vector<float> l (seconds (4.0)), r (l.size());
        timed ("crash-engine.wav", l.size(), [&] { addCurrent (currentCrash(), l, r, 0); });
        write (directory, "crash-engine.wav", l, r);
    }

    // Eighth rides on one voice, each hit a restrike.
    {
        std::vector<float> l (seconds (5.0)), r (l.size());
        DrumVoice voice;
        voice.prepare (rate);
        const auto eighth = seconds (0.25);
        size_t at = 0;

        timed ("ride-engine.wav", l.size(),
               [&]
               {
                   for (size_t k = 0; k < 16; ++k)
                   {
                       auto hit = currentRide();
                       hit.velocity = k % 2 == 0 ? 120.0f : 85.0f;
                       hit.seed = static_cast<std::uint32_t> (k + 1);

                       if (! voice.restrike (hit))
                           voice.trigger (hit);

                       voice.render (l.data() + at, r.data() + at, static_cast<int> (eighth));
                       at += eighth;
                   }

                   voice.render (l.data() + at, r.data() + at, static_cast<int> (l.size() - at));
               });
        write (directory, "ride-engine.wav", l, r);
    }
}

TEST_CASE ("Cymbal prototype: a crashier crash", "[.cymbal-crash]")
{
    const auto folder = juce::SystemStats::getEnvironmentVariable ("DAWG_DRUM_WAVS", {});
    REQUIRE (folder.isNotEmpty());
    const juce::File directory (folder);
    REQUIRE (directory.createDirectory());

    auto base = modalCrash;
    base.modes = 128;
    base.lift = 0.6;
    base.sizzle = 0.12f;
    base.wash = 0.35f;

    const auto one = [&] (const Modal& m, const char* name, float velocity = 1.0f)
    {
        std::vector<float> l (static_cast<size_t> (4.0 * rate)), r (l.size());
        std::printf ("  %s%s", name, "\n");
        renderModal (m, { { 0, velocity } }, l, r);
        REQUIRE (std::ranges::all_of (l, [] (float x) { return std::isfinite (x) && std::abs (x) < 100.0f; }));
        const auto power = [&] (double from, double to, bool high)
        {
            const auto hp = SvfCoefficients::bilinear (SvfMode::highPass, 6000.0, std::numbers::sqrt2, rate);
            SvfState a, b;
            double sum = 0.0;
            for (size_t i = 0; i < static_cast<size_t> (to * rate); ++i)
            {
                const auto x = high ? b.process (hp, a.process (hp, l[i])) : l[i];
                if (i >= static_cast<size_t> (from * rate))
                    sum += static_cast<double> (x) * x;
            }
            return 10.0 * std::log10 (sum / ((to - from) * rate) + 1e-30);
        };
        std::printf ("    0-0.2 s %.1f dB (above 6 kHz %.1f dB), 3-4 s %.1f dB%s", power (0.0, 0.2, false),
                     power (0.0, 0.2, true), power (3.0, 4.0, false), "\n");
        write (directory, name, l, r);
    };

    one (base, "crash2-0-now.wav");

    // Past about 0.25 at drive 2 the loop rings on by itself for as long as the coupling lasts: louder and tonal.
    const std::array<std::tuple<const char*, float, float>, 2> couplings { { { "a", 0.2f, 2.0f },
                                                                             { "c", 0.15f, 3.0f } } };

    for (const auto& [label, coupling, drive] : couplings)
    {
        auto coupled = base;
        coupled.coupling = coupling;
        coupled.drive = drive;
        const auto name = std::string ("crash2-1") + label + "-coupled.wav";
        one (coupled, name.c_str());
        const auto soft = std::string ("crash2-1") + label + "-coupled-soft.wav";
        one (coupled, soft.c_str(), 0.4f);
    }

    auto splashed = base;
    splashed.splash = 1.5f;
    splashed.splashStart = 16000.0;
    one (splashed, "crash2-2-splash.wav");

    for (const auto& [label, coupling, drive] : couplings)
    {
        auto both = splashed;
        both.coupling = coupling;
        both.drive = drive;
        one (both, (std::string ("crash2-3") + label + "-coupled-and-splash.wav").c_str());
    }
}
