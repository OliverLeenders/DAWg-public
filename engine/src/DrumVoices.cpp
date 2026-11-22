#include "dawg/DrumVoices.h"

#include "dawg/CompiledEngine.h"
#include "dawg/PolyBlep.h"
#include "loopreverb/Simd.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

// The Drum Synth's voices (docs/design/drum-synth.md, DR5-DR7, sections 3.1-3.5). Every model is a few sines, a bank
// of six polyBLEP squares (the 808's metal), xorshift noise, prewarped SVFs at fixed cutoffs (no oversampling: their
// resonance is low and their cutoffs don't sweep), and DrumEnvelopes; the cymbal is a bank of resonators. A voice
// renders mono, then pans; the cymbal alone is stereo.

namespace dawg
{

namespace
{
constexpr double twoPi = 2.0 * std::numbers::pi;
const auto butterworth = std::numbers::sqrt2;

constexpr std::array<ParamInfo, drumCommonCount> commonParams { {
    { "level", "Level", -60.0f, 6.0f, 0.0f, ParamKind::continuous, "dB" },
    { "pan", "Pan", -100.0f, 100.0f, 0.0f, ParamKind::continuous, "%" },
    { "tune", "Tune", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "st" },
    { "decay", "Decay", 10.0f, 400.0f, 100.0f, ParamKind::continuous, "%", 100.0f },
    { "velocity", "Vel. amount", 0.0f, 100.0f, 70.0f, ParamKind::continuous,
      "%" }, // how much velocity changes the level
} };

constexpr std::array<ParamInfo, 7> kickParams { {
    { "pitch", "Pitch", 30.0f, 120.0f, 50.0f, ParamKind::continuous, "Hz", 55.0f },
    { "sweep", "Sweep", 0.0f, 48.0f, 24.0f, ParamKind::continuous, "st" },
    { "sweepTime", "Sweep time", 2.0f, 200.0f, 30.0f, ParamKind::continuous, "ms", 30.0f },
    { "hold", "Hold", 0.0f, 200.0f, 0.0f, ParamKind::continuous, "ms", 40.0f },
    { "click", "Click", 0.0f, 100.0f, 40.0f, ParamKind::continuous, "%" },
    { "clickTone", "Click tone", 1000.0f, 12000.0f, 4000.0f, ParamKind::continuous, "Hz", 4000.0f },
    { "drive", "Drive", 0.0f, 24.0f, 3.0f, ParamKind::continuous, "dB" },
} };

constexpr std::array<ParamInfo, 6> snareParams { {
    { "pitch", "Pitch", 100.0f, 400.0f, 185.0f, ParamKind::continuous, "Hz", 200.0f },
    { "sweep", "Sweep", 0.0f, 12.0f, 3.0f, ParamKind::continuous, "st" },
    { "tone", "Tone", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "snappy", "Snappy", 0.0f, 100.0f, 60.0f, ParamKind::continuous, "%" },
    { "noiseDecay", "Noise decay", 20.0f, 1000.0f, 200.0f, ParamKind::continuous, "ms", 200.0f },
    { "noiseTone", "Noise tone", 1000.0f, 12000.0f, 5000.0f, ParamKind::continuous, "Hz", 4000.0f },
} };

constexpr std::array<ParamInfo, 5> tomParams { {
    { "pitch", "Pitch", 50.0f, 400.0f, 110.0f, ParamKind::continuous, "Hz", 140.0f },
    { "sweep", "Sweep", 0.0f, 12.0f, 4.0f, ParamKind::continuous, "st" },
    { "sweepTime", "Sweep time", 10.0f, 500.0f, 120.0f, ParamKind::continuous, "ms", 120.0f },
    { "overtones", "Overtones", 0.0f, 100.0f, 40.0f, ParamKind::continuous, "%" },
    { "stick", "Stick", 0.0f, 100.0f, 30.0f, ParamKind::continuous, "%" },
} };

constexpr std::array<ParamInfo, 4> hatParams { {
    { "tone", "Tone", 2000.0f, 14000.0f, 7000.0f, ParamKind::continuous, "Hz", 7000.0f },
    { "colour", "Colour", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "noise", "Noise", 0.0f, 100.0f, 25.0f, ParamKind::continuous, "%" },
    { "attack", "Attack", 0.0f, 20.0f, 0.0f, ParamKind::continuous, "ms", 5.0f },
} };

// Tone is the stick's hardness, Colour how much the low modes give way to the high, Wash and Sizzle the noise under
// the modes, Ride the cymbal's kind (0 a crash, 100 a ride), Swell the noise's rise.
constexpr std::array<ParamInfo, 7> cymbalParams { {
    { "tone", "Tone", 2000.0f, 12000.0f, 6000.0f, ParamKind::continuous, "Hz", 6000.0f },
    { "colour", "Colour", 0.0f, 100.0f, 60.0f, ParamKind::continuous, "%" },
    { "wash", "Wash", 0.0f, 100.0f, 35.0f, ParamKind::continuous, "%" },
    { "sizzle", "Sizzle", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "ride", "Ride", 0.0f, 100.0f, 0.0f, ParamKind::continuous, "%" },
    { "swell", "Swell", 0.0f, 200.0f, 60.0f, ParamKind::continuous, "ms", 40.0f },
    { "width", "Width", 0.0f, 100.0f, 60.0f, ParamKind::continuous, "%" },
} };

constexpr std::array<ParamInfo, 4> clapParams { {
    { "tone", "Tone", 500.0f, 4000.0f, 1200.0f, ParamKind::continuous, "Hz", 1200.0f },
    { "spread", "Spread", 5.0f, 30.0f, 10.0f, ParamKind::continuous, "ms" },
    { "bursts", "Bursts", 2.0f, 5.0f, 3.0f, ParamKind::integer },
    { "tail", "Tail", 0.0f, 100.0f, 60.0f, ParamKind::continuous, "%" },
} };

constexpr std::array<ParamInfo, 3> rimParams { {
    { "pitch", "Pitch", 200.0f, 2000.0f, 900.0f, ParamKind::continuous, "Hz", 800.0f },
    { "tone", "Tone", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "click", "Click", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
} };

// The randomise ranges (DR14), in each table's order; { 1, 0 } keeps a parameter.
constexpr DrumRange kept { 1.0f, 0.0f };
constexpr std::array<DrumRange, 7> kickRanges { { { 40.0f, 70.0f },
                                                  { 12.0f, 36.0f },
                                                  { 15.0f, 80.0f },
                                                  { 0.0f, 40.0f },
                                                  { 10.0f, 70.0f },
                                                  { 2000.0f, 8000.0f },
                                                  { 0.0f, 9.0f } } };
constexpr std::array<DrumRange, 6> snareRanges {
    { { 150.0f, 260.0f }, { 0.0f, 6.0f }, { 30.0f, 70.0f }, { 30.0f, 90.0f }, { 100.0f, 350.0f }, { 3000.0f, 9000.0f } }
};
constexpr std::array<DrumRange, 5> tomRanges {
    { kept, { 1.0f, 8.0f }, { 50.0f, 250.0f }, { 10.0f, 70.0f }, { 0.0f, 60.0f } }
};
constexpr std::array<DrumRange, 4> hatRanges {
    { { 5000.0f, 10000.0f }, { 25.0f, 75.0f }, { 10.0f, 50.0f }, { 0.0f, 3.0f } }
};
constexpr std::array<DrumRange, 7> cymbalRanges { { { 4000.0f, 10000.0f },
                                                    { 30.0f, 80.0f },
                                                    { 15.0f, 50.0f },
                                                    { 10.0f, 70.0f },
                                                    kept,
                                                    { 0.0f, 80.0f },
                                                    { 30.0f, 80.0f } } };
constexpr std::array<DrumRange, 4> clapRanges {
    { { 800.0f, 2000.0f }, { 6.0f, 16.0f }, { 2.0f, 4.0f }, { 30.0f, 80.0f } }
};
constexpr std::array<DrumRange, 3> rimRanges { { { 600.0f, 1400.0f }, { 20.0f, 80.0f }, { 20.0f, 80.0f } } };

// The models' decays in seconds at Decay 100 %; a cymbal's is its modes' at 1 kHz, a crash's then a ride's.
constexpr double kickDecay = 0.45;
constexpr double snareDecay = 0.15;
constexpr double tomDecay = 0.4;
constexpr double hatDecay = 0.15;
constexpr double crashDecay = 2.5;
constexpr double rideDecay = 4.0;
constexpr double clapDecay = 0.25;
constexpr double rimDecay = 0.06;

// The cymbal (section 3.5): a crash, and how a ride differs from it, Ride interpolating between them.
struct CymbalShape
{
    double low;     // Hz, the lowest mode
    double high;    // Hz, the highest
    double density; // modes per Hz grow as f^density, as on a thin plate
    double tilt;    // a mode's decay goes as (f / 1 kHz)^tilt
    double strike;  // s, the stick's burst
    double modes;   // how many
};

constexpr CymbalShape crashShape { 300.0, 16000.0, 0.5, -0.15, 0.003, 128.0 };
constexpr CymbalShape rideShape { 400.0, 14000.0, 0.3, -0.25, 0.0015, 96.0 };
constexpr std::uint32_t cymbalSeed = 7U; // the modes are the same on every hit and every pad: Tune tells cymbals apart
// The stick: a click, the same on every hit, and a burst of noise, which differs a little from hit to hit (DR11).
// Noise alone made some hits far more tonal than others, as its spectrum excites some modes and misses others.
constexpr float stickImpulse = 1.3f;
constexpr float stickNoise = 0.7f;
// A crash's coupling (section 3.5, 2026-11-21): a hard hit bends the plate non-linearly, passing energy from the
// modes it rang into others. The modes' sum, soft-clipped and cubed, comes back in as excitation for the first 0.3 s
// after a hit: nothing for soft hits, an explosion for hard ones. Above about 0.6 the loop rings on by itself.
constexpr float crashCoupling = 0.375f;
constexpr float crashDrive = 1.2f;
constexpr double couplingTime = 0.3;  // s, to -60 dB
constexpr double washStart = 10000.0; // Hz, the wash's low-pass at a hit
constexpr double washEnd = 4000.0;    // where it closes to

[[nodiscard]] float dbToGain (float db) noexcept
{
    return std::pow (10.0f, db / 20.0f);
}

// sin (2 pi phase) for a phase in 0..1, folded onto a quarter wave and summed to the 11th power: within 4e-6 of
// std::sin, a fifth of its cost. Every sine a voice plays goes through it.
[[nodiscard]] double sinOfPhase (double phase) noexcept
{
    auto x = phase < 0.5 ? phase : phase - 1.0; // -0.5..0.5

    if (x > 0.25)
        x = 0.5 - x;
    else if (x < -0.25)
        x = -0.5 - x;

    const auto y = twoPi * x;
    const auto y2 = y * y;
    return y
           * (1.0
              + y2
                    * (-1.0 / 6.0
                       + y2 * (1.0 / 120.0 + y2 * (-1.0 / 5040.0 + y2 * (1.0 / 362880.0 + y2 * (-1.0 / 39916800.0))))));
}

// A phase advanced by a step under 1, kept in 0..1.
[[nodiscard]] double advance (double phase, double step) noexcept
{
    phase += step;
    return phase >= 1.0 ? phase - 1.0 : phase;
}

// tanh, as a rational curve that meets 1 at 3 and stays there: the kick's drive.
[[nodiscard]] float softClip (float x) noexcept
{
    const auto c = std::clamp (x, -3.0f, 3.0f);
    return c * (27.0f + c * c) / (27.0f + 9.0f * c * c);
}

// A seed that never leaves xorshift32 stuck at zero.
[[nodiscard]] std::uint32_t scramble (std::uint32_t seed) noexcept
{
    auto x = (seed + 0x9e3779b9U) * 0x85ebca6bU;
    x ^= x >> 13U;
    x *= 0xc2b2ae35U;
    x ^= x >> 16U;
    return x != 0 ? x : 1U;
}

[[nodiscard]] float nextNoise (std::uint32_t& state) noexcept
{
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return static_cast<float> (static_cast<std::int32_t> (state)) / 2147483648.0f;
}
} // namespace

void DrumMetal::start (double ratio, double sampleRate) noexcept
{
    m_phases = {};

    for (size_t i = 0; i < m_steps.size(); ++i)
        m_steps[i] = std::min (0.5, frequencies[i] * ratio / sampleRate);
}

double DrumMetal::next() noexcept
{
    double sum = 0.0;

    for (size_t i = 0; i < m_phases.size(); ++i)
    {
        const auto dt = m_steps[i];
        const auto t = m_phases[i];
        sum += (t < 0.5 ? 1.0 : -1.0) + polyblep::blep (t, dt) - polyblep::blep (t < 0.5 ? t + 0.5 : t - 0.5, dt);
        m_phases[i] = advance (t, dt);
    }

    return sum / static_cast<double> (m_phases.size());
}

std::span<const ParamInfo> drumCommonParams() noexcept
{
    return commonParams;
}

std::span<const ParamInfo> drumModelParams (DrumModel model) noexcept
{
    switch (model)
    {
        case DrumModel::kick:
            return kickParams;
        case DrumModel::snare:
            return snareParams;
        case DrumModel::tom:
            return tomParams;
        case DrumModel::hat:
            return hatParams;
        case DrumModel::cymbal:
            return cymbalParams;
        case DrumModel::clap:
            return clapParams;
        case DrumModel::rim:
            return rimParams;
        case DrumModel::off:
            break;
    }

    return {};
}

std::span<const DrumRange> drumModelRandomRanges (DrumModel model) noexcept
{
    switch (model)
    {
        case DrumModel::kick:
            return kickRanges;
        case DrumModel::snare:
            return snareRanges;
        case DrumModel::tom:
            return tomRanges;
        case DrumModel::hat:
            return hatRanges;
        case DrumModel::cymbal:
            return cymbalRanges;
        case DrumModel::clap:
            return clapRanges;
        case DrumModel::rim:
            return rimRanges;
        case DrumModel::off:
            break;
    }

    return {};
}

const char* drumModelName (DrumModel model) noexcept
{
    static constexpr std::array<const char*, drumModelCount> names { "Off", "Kick",   "Snare", "Tom",
                                                                     "Hat", "Cymbal", "Clap",  "Rim" };
    return names[static_cast<size_t> (model)];
}

const char* drumModelId (DrumModel model) noexcept
{
    static constexpr std::array<const char*, drumModelCount> ids { "off", "kick",   "snare", "tom",
                                                                   "hat", "cymbal", "clap",  "rim" };
    return ids[static_cast<size_t> (model)];
}

DrumHit DrumHit::defaults (DrumModel model) noexcept
{
    DrumHit hit;
    hit.model = model;

    for (size_t i = 0; i < commonParams.size(); ++i)
        hit.common[i] = commonParams[i].defaultValue;

    const auto own = drumModelParams (model);

    for (size_t i = 0; i < own.size(); ++i)
        hit.values[i] = own[i].defaultValue;

    return hit;
}

DrumHit& DrumHit::set (const char* id, float value) noexcept
{
    for (size_t i = 0; i < commonParams.size(); ++i)
        if (std::strcmp (commonParams[i].id, id) == 0)
            common[i] = value;

    const auto own = drumModelParams (model);

    for (size_t i = 0; i < own.size(); ++i)
        if (std::strcmp (own[i].id, id) == 0)
            values[i] = value;

    return *this;
}

// ---------------------------------------------------------------------------------------------------------

void DrumVoice::prepare (double sampleRate) noexcept
{
    m_sampleRate = sampleRate;
    m_model = DrumModel::off;
}

void DrumVoice::trigger (const DrumHit& hit) noexcept
{
    m_envelopes = {};
    m_modePhases = {};
    m_filterStates = {};
    m_driveGain = 1.0f;
    m_driveNorm = 1.0f;
    m_burstsLeft = 0;
    m_fade = 1.0;
    m_fadeStep = 0.0;
    m_sweep.start (m_sampleRate, 0.0, 1.0);

    if (hit.model == DrumModel::cymbal)
    {
        m_cymbal.y1.fill (0.0f);
        m_cymbal.y2.fill (0.0f);
        m_washEarly = {};
        m_washTail = {};
        m_sizzle = {};
        m_feedback = 0.0f;
    }

    setUp (hit);
}

bool DrumVoice::restrike (const DrumHit& hit) noexcept
{
    if (hit.model != DrumModel::cymbal || m_model != DrumModel::cymbal || ! active() || choking())
        return false;

    setUp (hit);
    return true;
}

void DrumVoice::setUp (const DrumHit& hit) noexcept
{
    m_model = hit.model;
    m_values = hit.values;
    m_noise = scramble (hit.seed);
    m_noiseRight = scramble (hit.seed ^ 0x5bd1e995U);
    m_noiseWide = scramble (hit.seed ^ 0x27d4eb2fU);

    const auto sr = m_sampleRate;
    const auto& v = m_values;
    const auto scale = static_cast<double> (std::clamp (hit.common[drumDecay], 10.0f, 400.0f)) / 100.0;
    const auto tune = std::exp2 (static_cast<double> (hit.common[drumTune]) / 12.0);
    const auto velocity = std::clamp (hit.velocity, 1.0f, 127.0f) / 127.0f;
    const auto amount = std::clamp (hit.common[drumVelocity], 0.0f, 100.0f) / 100.0f;

    // DR7: velocity sets the level by the pad's amount, and brightens a little (an accent).
    m_level = dbToGain (hit.common[drumLevel]) * (1.0f - amount + amount * velocity * velocity);
    m_accent = 0.25f * amount * velocity;
    m_pan = panGains (hit.common[drumPan] / 100.0f);
    const auto accent = 1.0 + static_cast<double> (m_accent);

    switch (m_model)
    {
        case DrumModel::kick:
        {
            m_pitch = static_cast<double> (v[0]) * tune;
            m_sweep.start (sr, static_cast<double> (v[1]), static_cast<double> (v[2]) / 1000.0);
            m_envelopes[0].start (sr, 0.0, static_cast<double> (v[3]) / 1000.0, kickDecay * scale);
            m_envelopes[1].start (sr, 0.0, 0.0, 0.006);
            setFilter (0, SvfMode::bandPass, static_cast<double> (v[5]) * accent, 1.0);
            m_noiseGain = v[4] / 100.0f * static_cast<float> (accent);
            m_modeFrequencies[0] = m_pitch;

            if (v[6] > 0.01f)
            {
                m_driveGain = dbToGain (v[6]);
                m_driveNorm = 1.0f / softClip (m_driveGain);
            }

            m_trim = 0.7f;
            break;
        }

        case DrumModel::snare:
        {
            const auto t = v[2] / 100.0f;
            const auto body = std::min (1.0f, 2.0f * (1.0f - t));
            m_pitch = static_cast<double> (v[0]) * tune;
            m_modeFrequencies = { m_pitch, 1.75 * m_pitch, 0.0, 0.0 };
            m_modeGains = { body, 0.6f * body, 0.0f, 0.0f };
            m_sweep.start (sr, static_cast<double> (v[1]), 0.015);
            m_envelopes[0].start (sr, 0.0, 0.0, snareDecay * scale);
            m_envelopes[1].start (sr, 0.0, 0.0, 0.7 * snareDecay * scale);
            m_envelopes[2].start (sr, 0.0, 0.0, static_cast<double> (v[4]) / 1000.0 * scale);
            setFilter (0, SvfMode::highPass, static_cast<double> (v[5]) * accent, butterworth);
            m_noiseGain = std::min (1.0f, 2.0f * t) * v[3] / 100.0f;
            m_trim = 0.36f;
            break;
        }

        case DrumModel::tom:
        {
            const auto overtones = v[3] / 100.0f;
            m_pitch = static_cast<double> (v[0]) * tune;
            m_modeFrequencies = { m_pitch, 1.59 * m_pitch, 2.14 * m_pitch, 0.0 };
            m_modeGains = { 1.0f, 0.6f * overtones, 0.4f * overtones, 0.0f };
            m_sweep.start (sr, static_cast<double> (v[1]), static_cast<double> (v[2]) / 1000.0);
            m_envelopes[0].start (sr, 0.0, 0.0, tomDecay * scale);
            m_envelopes[1].start (sr, 0.0, 0.0, tomDecay / 1.6 * scale);
            m_envelopes[2].start (sr, 0.0, 0.0, tomDecay / 2.3 * scale);
            m_envelopes[3].start (sr, 0.0, 0.0, 0.012);
            setFilter (0, SvfMode::bandPass, 3000.0 * accent, 1.0);
            m_noiseGain = 0.8f * v[4] / 100.0f;
            m_trim = 0.6f;
            break;
        }

        case DrumModel::hat:
        {
            const auto colour = std::exp2 ((static_cast<double> (v[1]) - 50.0) / 50.0);
            const auto tone = static_cast<double> (v[0]) * (1.0 + 0.5 * static_cast<double> (m_accent));
            m_pitch = tune;
            setFilter (0, SvfMode::bandPass, 3440.0 * colour, 1.0 / 3.0);
            setFilter (1, SvfMode::bandPass, 7100.0 * colour, 1.0 / 3.0);
            // 24 dB/oct high-passes, two stages each for the metal (2, 4) and the noise (3, 5).
            setFilter (2, SvfMode::highPass, tone, butterworth);
            setFilter (3, SvfMode::highPass, tone, butterworth);
            setFilter (4, SvfMode::highPass, tone, butterworth);
            setFilter (5, SvfMode::highPass, tone, butterworth);
            m_bandScale = 1.0f / 3.0f;
            m_noiseGain = v[2] / 100.0f;
            m_metalLeft.start (tune, sr);
            m_envelopes[0].start (sr, static_cast<double> (v[3]) / 1000.0, 0.0, hatDecay * scale);
            m_trim = 2.6f;
            break;
        }

        case DrumModel::cymbal:
        {
            // Velocity strikes harder rather than turning the cymbal up, so a new hit adds to the ringing one.
            const auto strength = m_level / dbToGain (hit.common[drumLevel]);
            m_level = dbToGain (hit.common[drumLevel]);
            setUpCymbal (tune, scale, strength);
            break;
        }

        case DrumModel::clap:
        {
            setFilter (0, SvfMode::bandPass, static_cast<double> (v[0]) * accent, 0.7);
            m_bandScale = 0.7f;
            m_burstSpacing = std::max (1, static_cast<int> (std::lround (static_cast<double> (v[1]) / 1000.0 * sr)));
            m_burstsLeft = std::clamp (static_cast<int> (std::lround (v[2])), 2, 5) - 1;
            m_untilBurst = m_burstSpacing;
            m_envelopes[0].start (sr, 0.0, 0.0, std::max (0.004, 0.8 * static_cast<double> (v[1]) / 1000.0));
            m_noiseGain = v[3] / 100.0f;
            m_tailDecay = clapDecay * scale;
            m_trim = 2.4f;
            break;
        }

        case DrumModel::rim:
        {
            const auto t = v[1] / 100.0f;
            m_pitch = static_cast<double> (v[0]) * tune;
            m_modeFrequencies = { m_pitch, 2.3 * m_pitch, 0.0, 0.0 };
            m_modeGains = { 1.0f - 0.6f * t, 0.4f + 0.6f * t, 0.0f, 0.0f };
            m_envelopes[0].start (sr, 0.0, 0.0, rimDecay * scale);
            m_envelopes[1].start (sr, 0.0, 0.0, 0.66 * rimDecay * scale);
            m_envelopes[2].start (sr, 0.0, 0.0, 0.003);
            setFilter (0, SvfMode::highPass, 2500.0, butterworth);
            m_noiseGain = v[2] / 100.0f * static_cast<float> (accent);
            m_trim = 0.5f;
            break;
        }

        case DrumModel::off:
            break;
    }
}

// The cymbal (section 3.5): up to 128 two-pole resonators at inharmonic frequencies, denser towards the top, each
// with its own decay and place in the stereo field, rung by a short burst of low-passed noise (the stick). Close modes
// beat against each other, which is the shimmer. Under them, a wash of noise through a low-pass that closes as the
// cymbal rings, and a sizzle of high noise that swells after the hit.
void DrumVoice::setUpCymbal (double tune, double scale, float strength) noexcept
{
    const auto sr = m_sampleRate;
    const auto& v = m_values;
    const auto r = static_cast<double> (std::clamp (v[4], 0.0f, 100.0f)) / 100.0;
    const auto mix = [r] (double crash, double ride) { return crash + (ride - crash) * r; };
    const auto low = mix (crashShape.low, rideShape.low) * tune;
    const auto high = mix (crashShape.high, rideShape.high) * tune;
    const auto p = mix (crashShape.density, rideShape.density) + 1.0;
    const auto tilt = mix (crashShape.tilt, rideShape.tilt);
    const auto t60 = mix (crashDecay, rideDecay) * scale;
    const auto hardness = static_cast<double> (v[0]) * (1.0 + 0.5 * static_cast<double> (m_accent));
    const auto lift = static_cast<double> (v[1]) / 100.0;
    const auto width = v[6] / 100.0f;
    const auto nyquist = 0.45 * sr;

    m_cymbalModes = std::min (maxCymbalModes,
                              4 * static_cast<size_t> (std::lround (mix (crashShape.modes, rideShape.modes) / 4.0)));

    // The same draws in the same order on every hit, so a restrike rings the same cymbal.
    auto rng = cymbalSeed * 2654435761U | 1U;
    const auto uniform = [&rng] { return 0.5 * (static_cast<double> (nextNoise (rng)) + 1.0); };
    const auto lowP = std::pow (low, p);
    const auto highP = std::pow (high, p);
    auto longest = 0.0;

    for (size_t i = 0; i < m_cymbalModes; ++i)
    {
        const auto f = std::pow (lowP + uniform() * (highP - lowP), 1.0 / p);
        const auto decay = t60 * std::pow (f / 1000.0 / tune, tilt) * (0.7 + 0.6 * uniform());
        const auto gain = (0.4 + 0.6 * uniform()) / (1.0 + std::pow (f / hardness, 2.0)) * std::pow (f / 1000.0, lift);
        const auto pan = static_cast<float> (2.0 * uniform() - 1.0) * width;

        if (f >= nyquist)
        {
            m_cymbal.a1[i] = m_cymbal.a2[i] = m_cymbal.gainLeft[i] = m_cymbal.gainRight[i] = 0.0f;
            continue;
        }

        const auto radius = std::pow (10.0, -3.0 / (decay * sr));
        const auto w = twoPi * f / sr;
        m_cymbal.a1[i] = static_cast<float> (2.0 * radius * std::cos (w));
        m_cymbal.a2[i] = static_cast<float> (-radius * radius);
        const auto g = static_cast<float> (0.05 * gain * std::sin (w));
        m_cymbal.gainLeft[i] = g * std::sqrt (0.5f * (1.0f - pan));
        m_cymbal.gainRight[i] = g * std::sqrt (0.5f * (1.0f + pan));
        longest = std::max (longest, decay);
    }

    const auto swell = static_cast<double> (v[5]) / 1000.0;
    setFilter (0, SvfMode::highPass, 400.0 * tune, butterworth);
    setFilter (2, SvfMode::lowPass, std::min (nyquist, hardness), butterworth);
    setFilter (3, SvfMode::highPass, std::min (nyquist, 7000.0 * tune), butterworth);
    m_washCutoff = std::min (nyquist, washStart * tune);
    m_washEnd = std::min (nyquist, washEnd * tune);
    m_washClose = std::exp (-32.0 / (0.35 * t60 * sr));
    m_washUntil = 0;
    m_noiseGain = v[2] / 100.0f;
    m_sizzleGain = 0.25f * v[3] / 100.0f;
    m_strike = strength;
    m_impulse =
        stickImpulse * static_cast<float> (std::sqrt (mix (crashShape.strike, rideShape.strike) / crashShape.strike));
    m_washEarly.add (0.6f * strength, 0.01 + 0.5 * swell, 0.12 * t60, sr);
    m_washTail.add (0.4f * strength, 0.01 + 0.5 * swell, t60, sr);
    m_sizzle.add (strength, swell, 0.5 * t60, sr);

    // 0: the voice's life, until the longest mode is at -60 dB; 1: the stick; 2: the coupling, a crash's only.
    m_coupling = crashCoupling * static_cast<float> (1.0 - r);
    m_envelopes[2].start (sr, 0.0, 0.0, couplingTime);
    m_envelopes[0].start (sr, 0.0, 0.0, 0.6 * std::max (longest, t60));
    m_envelopes[1].start (sr, 0.0, 0.0, mix (crashShape.strike, rideShape.strike));
    m_trim = dbToGain (static_cast<float> (mix (-7.6, -4.0))); // as loud as the cymbal before it (section 9.2)
}

void DrumVoice::Bloom::add (float amount, double riseSeconds, double fallSeconds, double sampleRate) noexcept
{
    fall = static_cast<float> (std::pow (10.0, -3.0 / std::max (1.0, fallSeconds * sampleRate)));
    const auto rise = static_cast<int> (std::lround (riseSeconds * sampleRate));

    if (rise < 1)
    {
        value += amount;
        rising = 0;
        return;
    }

    step = amount / static_cast<float> (rise);
    rising = rise;
}

float DrumVoice::Bloom::next() noexcept
{
    value *= fall;

    if (rising > 0)
    {
        value += step;
        --rising;
    }

    return value;
}

void DrumVoice::choke (double seconds) noexcept
{
    if (! active())
        return;

    m_fadeStep = std::min (m_fade, 1.0 / std::max (1.0, seconds * m_sampleRate));
}

bool DrumVoice::active() const noexcept
{
    if (m_model == DrumModel::off)
        return false;

    return m_burstsLeft > 0 || std::ranges::any_of (m_envelopes, [] (const auto& e) { return ! e.finished(); });
}

void DrumVoice::render (float* left, float* right, int count) noexcept
{
    if (! active())
    {
        m_model = DrumModel::off;
        return;
    }

    for (int n = 0; n < count; ++n)
    {
        std::array<float, 2> out {};

        switch (m_model)
        {
            case DrumModel::kick:
                out[0] = kick();
                break;
            case DrumModel::snare:
                out[0] = snare();
                break;
            case DrumModel::tom:
                out[0] = tom();
                break;
            case DrumModel::hat:
                out[0] = hat();
                break;
            case DrumModel::cymbal:
                out = cymbal();
                break;
            case DrumModel::clap:
                out[0] = clap();
                break;
            case DrumModel::rim:
                out[0] = rim();
                break;
            case DrumModel::off:
                return;
        }

        if (m_model != DrumModel::cymbal)
            out[1] = out[0];

        const auto gain = m_level * m_trim * static_cast<float> (m_fade);
        left[n] += out[0] * gain * m_pan[0];
        right[n] += out[1] * gain * m_pan[1];

        if (m_fadeStep > 0.0)
        {
            m_fade -= m_fadeStep;

            if (m_fade <= 1.0e-9)
            {
                m_model = DrumModel::off;
                return;
            }
        }
    }

    if (! active())
        m_model = DrumModel::off;
}

// ---------------------------------------------------------------------------------------------------------
// Building blocks

float DrumVoice::noise() noexcept
{
    return nextNoise (m_noise);
}

float DrumVoice::noiseRight() noexcept
{
    return nextNoise (m_noiseRight);
}

float DrumVoice::filter (size_t index, size_t channel, float x) noexcept
{
    return m_filterStates[index][channel].process (m_filters[index], x);
}

void DrumVoice::setFilter (size_t index, SvfMode mode, double cutoff, double k) noexcept
{
    m_filters[index] = SvfCoefficients::bilinear (mode, std::max (20.0, cutoff), k, m_sampleRate);
}

// Modes [from, to): sines at their frequencies times a ratio, each with its gain and the envelope of its index. A mode
// with no gain, or whose envelope has ended, costs nothing.
float DrumVoice::sines (size_t from, size_t to, double sweepRatio) noexcept
{
    float sum = 0.0f;
    const auto scale = sweepRatio / m_sampleRate;

    for (size_t i = from; i < to; ++i)
    {
        if (juce::exactlyEqual (m_modeGains[i], 0.0f) || m_envelopes[i].finished())
            continue;

        m_modePhases[i] = advance (m_modePhases[i], m_modeFrequencies[i] * scale);
        sum += static_cast<float> (sinOfPhase (m_modePhases[i])) * m_modeGains[i] * m_envelopes[i].next();
    }

    return sum;
}

// A soft clip scaled so a full-scale input stays at full scale. The kick's body is a low sine, whose harmonics stay
// far below Nyquist, so it needs neither anti-aliasing nor oversampling.
float DrumVoice::drive (float x) noexcept
{
    return m_driveGain <= 1.0f ? x : softClip (x * m_driveGain) * m_driveNorm;
}

// Band-limited noise through a filter under an envelope, or nothing once the envelope has ended.
float DrumVoice::burst (size_t filterIndex, size_t envelope, float gain) noexcept
{
    if (juce::exactlyEqual (gain, 0.0f) || m_envelopes[envelope].finished())
        return 0.0f;

    return filter (filterIndex, 0, noise()) * m_envelopes[envelope].next() * gain;
}

// ---------------------------------------------------------------------------------------------------------
// Models

float DrumVoice::kick() noexcept
{
    const auto ratio = m_sweep.nextRatio();
    m_modePhases[0] = advance (m_modePhases[0], m_pitch * ratio / m_sampleRate);
    const auto body = drive (static_cast<float> (sinOfPhase (m_modePhases[0])) * m_envelopes[0].next());
    return body + burst (0, 1, m_noiseGain);
}

float DrumVoice::snare() noexcept
{
    const auto body = sines (0, 2, m_sweep.nextRatio());
    return body + burst (0, 2, m_noiseGain);
}

float DrumVoice::tom() noexcept
{
    const auto body = sines (0, 3, m_sweep.nextRatio());
    return body + burst (0, 3, m_noiseGain);
}

float DrumVoice::hat() noexcept
{
    const auto m = static_cast<float> (m_metalLeft.next());
    const auto bands = (filter (0, 0, m) + filter (1, 0, m)) * m_bandScale;
    const auto metalPart = m_noiseGain < 1.0f ? filter (4, 0, filter (2, 0, bands)) * (1.0f - m_noiseGain) : 0.0f;
    const auto noisePart = m_noiseGain > 0.0f ? filter (5, 0, filter (3, 0, noise())) * m_noiseGain : 0.0f;
    return (metalPart + noisePart) * m_envelopes[0].next();
}

std::array<float, 2> DrumVoice::cymbal() noexcept
{
    namespace simd = reverb::simd;
    (void) m_envelopes[0].next(); // the voice's life
    auto stick = 0.0f;

    if (! m_envelopes[1].finished())
    {
        stick = filter (2, 0, m_impulse + stickNoise * noise() * m_envelopes[1].next()) * m_strike;
        m_impulse = 0.0f;
    }

    // The modes, four at a time.
    const auto x = simd::broadcast (stick + m_feedback);
    auto left = simd::broadcast (0.0f);
    auto right = left;
    auto& c = m_cymbal;

    for (size_t i = 0; i < m_cymbalModes; i += 4)
    {
        const auto y1 = simd::load (c.y1.data() + i);
        const auto y =
            simd::load (c.a1.data() + i) * y1 + simd::load (c.a2.data() + i) * simd::load (c.y2.data() + i) + x;
        simd::store (c.y2.data() + i, y1);
        simd::store (c.y1.data() + i, y);
        left = left + y * simd::load (c.gainLeft.data() + i);
        right = right + y * simd::load (c.gainRight.data() + i);
    }

    std::array<float, 4> l {};
    std::array<float, 4> r {};
    simd::store (l.data(), left);
    simd::store (r.data(), right);
    std::array<float, 2> out { (l[0] + l[1]) + (l[2] + l[3]), (r[0] + r[1]) + (r[2] + r[3]) };

    // The coupling, a cubic stiffness: it restores rather than drives, but mixes the modes into sums and differences.
    if (! m_envelopes[2].finished())
    {
        const auto d = softClip ((out[0] + out[1]) * crashDrive);
        m_feedback = -m_coupling * m_envelopes[2].next() * d * d * d;
    }
    else
    {
        m_feedback = 0.0f;
    }

    // The wash, its low-pass stepping every 32 samples.
    const auto level = m_washEarly.next() + m_washTail.next();

    if (m_noiseGain > 0.0f && level > 1.0e-6f)
    {
        if (--m_washUntil <= 0)
        {
            m_washUntil = 32;
            m_filters[1] = SvfCoefficients::bilinear (SvfMode::lowPass, m_washCutoff, 0.9, m_sampleRate);
            m_washCutoff = m_washEnd + (m_washCutoff - m_washEnd) * m_washClose;
        }

        const auto width = m_values[6] / 100.0f;
        // The wash's own noise, so the stick and the sizzle sound the same whatever the wash.
        const auto nl = nextNoise (m_noiseRight);
        const std::array<float, 2> noises { nl, (1.0f - width) * nl + width * nextNoise (m_noiseWide) };

        for (size_t ch = 0; ch < 2; ++ch)
            out[ch] += filter (1, ch, filter (0, ch, noises[ch])) * level * m_noiseGain;
    }

    const auto sizzle = m_sizzle.next();

    if (m_sizzleGain > 0.0f && sizzle > 1.0e-6f)
    {
        const auto s = filter (3, 0, noise()) * sizzle * m_sizzleGain;
        out[0] += s;
        out[1] += s;
    }

    return out;
}

float DrumVoice::clap() noexcept
{
    if (m_burstsLeft > 0 && --m_untilBurst <= 0)
    {
        // Each burst restarts the short envelope; the last one starts the tail too.
        m_envelopes[0].start (m_sampleRate, 0.0, 0.0,
                              std::max (0.004, 0.8 * static_cast<double> (m_values[1]) / 1000.0));
        m_untilBurst = m_burstSpacing;

        if (--m_burstsLeft == 0)
            m_envelopes[1].start (m_sampleRate, 0.0, 0.0, m_tailDecay);
    }

    const auto n = filter (0, 0, noise()) * m_bandScale;
    return n * (m_envelopes[0].next() + m_envelopes[1].next() * m_noiseGain);
}

float DrumVoice::rim() noexcept
{
    return sines (0, 2, 1.0) + burst (0, 2, m_noiseGain);
}

} // namespace dawg
