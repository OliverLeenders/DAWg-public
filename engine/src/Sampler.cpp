#include "dawg/Sampler.h"

#include "dawg/BuiltinNodes.h"
#include "dawg/EnvelopeHandle.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Realtime.h"
#include "dawg/Sequence.h"
#include "dawg/VoiceAllocator.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

// The Sampler (docs/design/sampler-midi-delay-limiter.md, section 4): a sample per note, chosen by the note's key and
// velocity from the node's zones and read with cubic interpolation, through the zone's own envelope. The Sampler
// instrument plays its notes in voices of its own and follows their pitch curves itself. The Sample Voice is the same
// sound for a voice graph, one note per instance, its pitch a parameter that Note Expression's pitch signal drives at
// 100 % depth, as on the oscillator.

namespace dawg
{

namespace
{
constexpr double fadeSeconds = 0.005; // a choked sample fades out over 5 ms
constexpr int controlInterval = 32;   // samples between evaluations of a note's expression
constexpr double grainSeconds = 0.05; // a grain's length when a note keeps the sample's length

constexpr std::array<const char*, 2> pitchModeChoices { "Tape", "Keep length" };
constexpr std::array<const char*, 2> takeChoices { "In turn", "Random" };
constexpr size_t mostTakes = 64; // takes of one zone counted when a note picks one

constexpr std::array<ParamInfo, 5> samplerParams { {
    { "pitch", "Pitch", -120.0f, 120.0f, 0.0f, ParamKind::continuous, "st" },
    { "level", "Level", -60.0f, 12.0f, 0.0f, ParamKind::continuous, "dB" },
    { "velocity", "Vel. amount", 0.0f, 100.0f, 100.0f, ParamKind::continuous,
      "%" }, // how much velocity changes the level
    { "pitchMode", "Pitch mode", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, pitchModeChoices },
    { "takes", "Takes", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, takeChoices },
} };

enum SamplerParam : size_t
{
    pitchParam,
    levelParam,
    velocityParam,
    pitchModeParam,
    takesParam,
};

constexpr std::array<PortInfo, 1> notesPort { { { PortType::events, "Notes" } } };
constexpr auto samplerInputs = withModulationInputs (notesPort);
constexpr std::array<PortInfo, 1> stereoOutput { { { PortType::audio, "Audio" } } };

// Main, then Out 2 to Out 16 (docs/design/multi-output-instruments.md, MO1): a zone plays from its output, or from Main
// while nothing is wired to that output (MO2). The graph view shows only the outputs in use.
constexpr std::array<PortInfo, maxSampleOutputs> samplerOutputs { {
    { PortType::audio, "Main" },
    { PortType::audio, "Out 2" },
    { PortType::audio, "Out 3" },
    { PortType::audio, "Out 4" },
    { PortType::audio, "Out 5" },
    { PortType::audio, "Out 6" },
    { PortType::audio, "Out 7" },
    { PortType::audio, "Out 8" },
    { PortType::audio, "Out 9" },
    { PortType::audio, "Out 10" },
    { PortType::audio, "Out 11" },
    { PortType::audio, "Out 12" },
    { PortType::audio, "Out 13" },
    { PortType::audio, "Out 14" },
    { PortType::audio, "Out 15" },
    { PortType::audio, "Out 16" },
} };

const NodeDescriptor samplerDescriptor { nodetype::sampler, "Sampler", samplerInputs,           samplerOutputs,
                                         samplerParams,     true,      NodeCategory::instrument };

const NodeDescriptor sampleVoiceDescriptor { nodetype::sampleVoice, "Sampler", samplerInputs,        stereoOutput,
                                             samplerParams,         true,      NodeCategory::utility };

// A run of frames held for interpolation, so a voice touches its source once per run, not once per sample.
struct Window
{
    static constexpr int size = 64;

    std::array<std::array<float, size>, 2> frames {};
    juce::int64 start = 0;
    bool valid = false;

    // The frames at `first`..`first + 3`, read from the source when they aren't here.
    [[nodiscard]] std::array<const float*, 2> at (const AudioSource& source, juce::int64 first) noexcept
    {
        if (! valid || first < start || first + 4 > start + size)
        {
            start = first;
            valid = true;

            if (! source.read ({ frames[0].data(), frames[1].data() }, start, size))
                for (auto& channel : frames)
                    channel.fill (0.0f);
        }

        const auto offset = static_cast<size_t> (first - start);
        return { frames[0].data() + offset, frames[1].data() + offset };
    }
};

[[nodiscard]] float cubic (const float* y, float t) noexcept
{
    const auto a = 0.5f * (y[3] - y[0]) + 1.5f * (y[1] - y[2]);
    const auto b = y[0] - 2.5f * y[1] + 2.0f * y[2] - 0.5f * y[3];
    const auto c = 0.5f * (y[2] - y[0]);
    return ((a * t + b) * t + c) * t + y[1];
}

// One zone of a note's sound: its place in the sample, its envelope and its fade when choked.
class SampleStrand
{
public:
    void prepare (double sampleRate) noexcept
    {
        m_sampleRate = sampleRate;
        m_fadeStep = static_cast<float> (1.0 / std::max (1.0, fadeSeconds * sampleRate));
        m_grainStep = 1.0 / std::max (1.0, grainSeconds * sampleRate);
        stop();
    }

    void stop() noexcept { m_zone = nullptr; }

    [[nodiscard]] bool sounding() const noexcept { return m_zone != nullptr; }

    // The output port the strand plays into: its zone's, or Main while nothing is wired to that one (MO2).
    [[nodiscard]] size_t outputPort (size_t ports, juce::uint32 wired) const noexcept
    {
        const auto port = m_zone != nullptr ? static_cast<size_t> (std::max (1, m_zone->zone.output) - 1) : 0;
        return port < ports && port < 32 && (wired & (1u << port)) != 0 ? port : 0;
    }

    // A new compilation: the sounding zone is found again by id, or stops if it's gone.
    void adoptMap (const SampleMap* map) noexcept
    {
        if (map == m_map)
            return;

        m_map = map;

        if (m_zone == nullptr)
            return;

        m_zone = nullptr;

        if (map != nullptr)
        {
            const auto found =
                std::ranges::find_if (map->zones, [this] (const CompiledSampleZone& z) { return z.id == m_zoneId; });

            if (found != map->zones.end())
            {
                m_zone = &*found;
                m_chokeCount = map->chokes[chokeGroup()];
                adoptRender();
            }
        }
    }

    // A sounding note's render, found again in a new compilation's zone by its pitch; without it, the note goes on
    // with grains until the renders are back (PM3).
    void adoptRender() noexcept
    {
        if (m_render == nullptr)
            return;

        const auto& zone = *m_zone;
        m_render = nullptr;

        if (m_renderSemitones == 0 && ! zone.pitched.empty())
            m_render = zone.source.get();

        for (const auto& pitched : zone.pitched)
            if (pitched.semitones == m_renderSemitones)
                m_render = pitched.source.get();

        for (auto& window : m_windows)
            window.valid = false;
    }

    // A zone's velocity crossfades (SF7), equal power as SFZ's default: zones overlapping in velocity blend into each
    // other instead of adding up.
    [[nodiscard]] static float velocityFade (const SampleZone& zone, float velocity) noexcept
    {
        const auto v = velocity * 127.0f;
        auto gain = 1.0f;

        if (zone.fadeInHighVelocity > zone.fadeInLowVelocity && v < static_cast<float> (zone.fadeInHighVelocity))
            gain *= std::sqrt (
                std::max (0.0f, (v - static_cast<float> (zone.fadeInLowVelocity))
                                    / static_cast<float> (zone.fadeInHighVelocity - zone.fadeInLowVelocity)));

        if (zone.fadeOutHighVelocity > zone.fadeOutLowVelocity && v > static_cast<float> (zone.fadeOutLowVelocity))
            gain *= std::sqrt (
                std::max (0.0f, (static_cast<float> (zone.fadeOutHighVelocity) - v)
                                    / static_cast<float> (zone.fadeOutHighVelocity - zone.fadeOutLowVelocity)));

        return gain;
    }

    // Starts a zone of the map this strand has adopted.
    void start (const CompiledSampleZone& candidate, const CompiledNote& note, float velocitySensitivity) noexcept
    {
        m_zone = &candidate;
        m_render = nullptr;
        const auto& zone = m_zone->zone;
        m_zoneId = m_zone->id;
        m_velocityGain = (1.0f - velocitySensitivity * (1.0f - note.velocity)) * velocityFade (zone, note.velocity);
        m_position = static_cast<double> (m_zone->start);
        m_choking = false;
        m_fade = 1.0f;
        m_stage = zone.attackMs > 0.0 ? Stage::attack : Stage::decay;
        m_envelope = zone.attackMs > 0.0 ? 0.0f : 1.0f;
        m_progress = 0.0f;
        m_shapeHint = 0.0;
        m_grainsLive = false;

        for (auto& window : m_windows)
            window.valid = false;
    }

    // The choke group's count when this strand started; it stops when the count moves on.
    void setChokeCount (juce::uint32 count) noexcept { m_chokeCount = count; }

    [[nodiscard]] size_t chokeGroup() const noexcept
    {
        return static_cast<size_t> (std::clamp (m_zone->zone.chokeGroup, 0, maxChokeGroup));
    }

    // The note ended: the envelope releases, except on a one-shot, which plays on whatever the note does.
    void release() noexcept
    {
        if (m_zone == nullptr || m_zone->zone.mode == SampleMode::oneShot || m_stage == Stage::release)
            return;

        m_stage = Stage::release;
        m_releaseFrom = m_envelope;
        m_progress = 0.0f;
    }

    void choke() noexcept { m_choking = true; }

    // Adds [from, to) to the output. pitchAt (s) gives the note's pitch at a sample of the block, in MIDI semitones.
    // With keepLength, the sample plays at its own speed whatever the pitch (docs/design/sampler-mapping.md, PM2).
    template <typename PitchAt>
    void render (const AudioOutput& out, int from, int to, const PitchAt& pitchAt, const float* levelSignal,
                 float level, bool keepLength) noexcept
    {
        if (m_zone == nullptr || from >= to)
            return;

        if (m_zone->source == nullptr)
        {
            m_zone = nullptr;
            return;
        }

        if (const auto group = chokeGroup(); group > 0 && m_map->chokes[group] != m_chokeCount)
            m_choking = true;

        const auto& zone = *m_zone;
        const auto& source = *zone.source;
        const auto rateScale = source.sampleRate() / m_sampleRate;
        const auto loops = zone.loopEnd > 0;
        // The zone's times and shapes are read each block, so an edit reaches a sounding note.
        const Envelope envelope { progressPerSample (zone.zone.attackMs),
                                  progressPerSample (zone.zone.decayMs),
                                  progressPerSample (std::max (1.0, zone.zone.releaseMs)),
                                  static_cast<float> (zone.zone.sustain),
                                  handle (zone.zone.attackHandleTime, zone.zone.attackHandleLevel),
                                  handle (zone.zone.decayHandleTime, zone.zone.decayHandleLevel),
                                  handle (zone.zone.releaseHandleTime, zone.zone.releaseHandleLevel) };

        for (int s = from; s < to; ++s)
        {
            if (! loops && m_position >= static_cast<double> (zone.end))
            {
                m_zone = nullptr;
                return;
            }

            if (! advanceEnvelope (envelope))
            {
                m_zone = nullptr;
                return;
            }

            const auto semitones = pitchAt (s) - static_cast<double> (zone.zone.rootKey) + zone.zone.tune;
            const auto rate = std::exp2 (semitones / 12.0) * rateScale;
            const auto gain = std::pow (10.0f, (levelSignal != nullptr ? levelSignal[s] : level) / 20.0f)
                              * m_velocityGain * m_envelope * m_fade;

            auto left = 0.0f;
            auto right = 0.0f;

            if (keepLength && ! zone.pitched.empty())
            {
                // The render nearest the note's pitch at its start, the rest made up tape style (PM2).
                if (m_render == nullptr)
                    chooseRender (zone, semitones);

                sampleAt (zone, *m_render, m_renderOffset, 0, m_position, left, right);
                m_position =
                    wrapped (zone, m_position + std::exp2 ((semitones - m_renderSemitones) / 12.0) * rateScale);
            }
            else if (keepLength)
            {
                // Until the renders are ready, grains (PM3).
                renderGrains (zone, rate, left, right);
                m_position = wrapped (zone, m_position + rateScale);
                advanceGrains();
            }
            else
            {
                sampleAt (zone, source, 0, 0, m_position, left, right);
                m_position = wrapped (zone, m_position + rate);
            }

            out[0][s] += left * gain * zone.gains[0];
            out[1][s] += right * gain * zone.gains[1];

            if (m_choking)
            {
                m_fade -= m_fadeStep;

                if (m_fade <= 0.0f)
                {
                    m_zone = nullptr;
                    return;
                }
            }
        }
    }

private:
    enum class Stage
    {
        attack,
        decay,
        sustain,
        release,
    };

    // The zone's envelope for one block: progress per sample through each segment, the sustain level and each
    // segment's tension point (as the ADSR node's, dawg/EnvelopeHandle.h).
    struct Envelope
    {
        float attack = 1.0f;
        float decay = 1.0f;
        float release = 1.0f;
        float sustain = 1.0f;
        EnvelopeHandle attackHandle;
        EnvelopeHandle decayHandle;
        EnvelopeHandle releaseHandle;
    };

    // The progress per sample through a segment of this length; a segment of no time passes in one sample.
    [[nodiscard]] float progressPerSample (double milliseconds) const noexcept
    {
        return milliseconds > 0.0 ? static_cast<float> (1000.0 / (milliseconds * m_sampleRate)) : 1.0f;
    }

    [[nodiscard]] static EnvelopeHandle handle (double time, double level) noexcept
    {
        return { static_cast<float> (std::clamp (time, 0.0, 1.0)), static_cast<float> (std::clamp (level, 0.0, 1.0)) };
    }

    // Moves the envelope one sample on. Returns false once it has ended the sound.
    [[nodiscard]] bool advanceEnvelope (const Envelope& envelope) noexcept
    {
        switch (m_stage)
        {
            case Stage::attack:
                m_progress += envelope.attack;

                if (m_progress >= 1.0f)
                {
                    m_envelope = 1.0f;
                    m_progress = 0.0f;
                    m_stage = Stage::decay;
                }
                else
                {
                    m_envelope = envelope.attackHandle.shape (m_progress, m_shapeHint);
                }

                return true;

            case Stage::decay:
                m_progress += envelope.decay;

                if (m_progress >= 1.0f)
                {
                    m_envelope = envelope.sustain;
                    m_stage = Stage::sustain;
                }
                else
                {
                    m_envelope =
                        1.0f - (1.0f - envelope.sustain) * envelope.decayHandle.shape (m_progress, m_shapeHint);
                }

                return true;

            case Stage::sustain:
                // A sustain moved while the note holds is followed.
                m_envelope = envelope.sustain;
                return envelope.sustain > 0.0f;

            case Stage::release:
                m_progress += envelope.release;

                if (m_progress >= 1.0f)
                    return false;

                m_envelope = m_releaseFrom * (1.0f - envelope.releaseHandle.shape (m_progress, m_shapeHint));
                return true;
        }

        return true;
    }

    // Two grains half a grain apart, each reading at the note's rate from where the sample had got to when it began,
    // under a Hann window: the two windows add to 1, and at the sample's own rate both read what tape would (PM2).
    void renderGrains (const CompiledSampleZone& zone, double rate, float& left, float& right) noexcept
    {
        if (! m_grainsLive)
        {
            m_grainPhase = 0.0;
            m_grainRead = { m_position, m_position };
            m_grainsLive = true;
        }

        for (size_t g = 0; g < 2; ++g)
        {
            const auto phase = std::fmod (m_grainPhase + 0.5 * static_cast<double> (g), 1.0);
            const auto window = static_cast<float> (std::sin (juce::MathConstants<double>::pi * phase));
            auto grainLeft = 0.0f;
            auto grainRight = 0.0f;

            // Past the end of a zone that doesn't loop is the next slot of its file, or nothing.
            if (zone.loopEnd > 0 || m_grainRead[g] < static_cast<double> (zone.end))
                sampleAt (zone, *zone.source, 0, 2 * g, m_grainRead[g], grainLeft, grainRight);

            left += grainLeft * window * window;
            right += grainRight * window * window;
            m_grainRead[g] = wrapped (zone, m_grainRead[g] + rate);
        }
    }

    // The grains' windows move on; a grain whose window has closed starts again where the sample is now.
    void advanceGrains() noexcept
    {
        const auto before = m_grainPhase;
        m_grainPhase += m_grainStep;

        for (size_t g = 0; g < 2; ++g)
        {
            const auto offset = 0.5 * static_cast<double> (g);

            if (! juce::exactlyEqual (std::floor (before + offset), std::floor (m_grainPhase + offset)))
                m_grainRead[g] = m_position;
        }

        if (m_grainPhase >= 1.0)
            m_grainPhase -= 1.0;
    }

    // A position moved on: in a loop, back into it once past its end.
    [[nodiscard]] static double wrapped (const CompiledSampleZone& zone, double position) noexcept
    {
        if (zone.loopEnd > zone.loopStart)
            while (position >= static_cast<double> (zone.loopEnd))
                position -= static_cast<double> (zone.loopEnd - zone.loopStart);

        return position;
    }

    // The render nearest a pitch, in semitones from the root, among the zone's and the sample itself (PM2).
    void chooseRender (const CompiledSampleZone& zone, double semitones) noexcept
    {
        m_render = zone.source.get();
        m_renderOffset = 0;
        m_renderSemitones = 0;

        for (const auto& pitched : zone.pitched)
            if (std::abs (semitones - pitched.semitones) < std::abs (semitones - m_renderSemitones))
            {
                m_render = pitched.source.get();
                m_renderOffset = zone.start; // a render starts at the zone's start
                m_renderSemitones = pitched.semitones;
            }

        for (auto& window : m_windows)
            window.valid = false;
    }

    // The sample at a position in the zone, read from `source` (the file, or a render starting `offset` frames into
    // it) through windows `window` and `window + 1`: near a loop's end it fades into what comes before the loop's
    // start, with equal power.
    void sampleAt (const CompiledSampleZone& zone, const AudioSource& source, juce::int64 offset, size_t window,
                   double position, float& left, float& right) noexcept
    {
        const auto shift = static_cast<double> (offset);
        read (source, window, position - shift, left, right);

        if (zone.loopEnd > 0 && zone.crossfade > 0 && position >= static_cast<double> (zone.loopEnd - zone.crossfade))
        {
            const auto x = static_cast<float> ((position - static_cast<double> (zone.loopEnd - zone.crossfade))
                                               / static_cast<double> (zone.crossfade));
            auto earlyLeft = 0.0f;
            auto earlyRight = 0.0f;
            read (source, window + 1, position - shift - static_cast<double> (zone.loopEnd - zone.loopStart), earlyLeft,
                  earlyRight);
            const auto fadingOut = std::cos (x * juce::MathConstants<float>::halfPi);
            const auto fadingIn = std::sin (x * juce::MathConstants<float>::halfPi);
            left = left * fadingOut + earlyLeft * fadingIn;
            right = right * fadingOut + earlyRight * fadingIn;
        }
    }

    void read (const AudioSource& source, size_t window, double position, float& left, float& right) noexcept
    {
        const auto whole = std::floor (position);
        const auto frames = m_windows[window].at (source, static_cast<juce::int64> (whole) - 1);
        const auto t = static_cast<float> (position - whole);
        left = cubic (frames[0], t);
        right = cubic (frames[1], t);
    }

    double m_sampleRate = 48000.0;
    float m_fadeStep = 1.0f;
    const SampleMap* m_map = nullptr;
    const CompiledSampleZone* m_zone = nullptr;
    ObjectId m_zoneId = 0;
    double m_position = 0.0;
    float m_velocityGain = 1.0f;
    float m_fade = 1.0f;
    bool m_choking = false;
    Stage m_stage = Stage::sustain;
    float m_envelope = 1.0f;
    float m_progress = 0.0f;    // through the current segment, 0..1
    double m_shapeHint = 0.0;   // the curved segment's last Bezier solution (EnvelopeHandle::shape())
    float m_releaseFrom = 0.0f; // the level the release started at
    juce::uint32 m_chokeCount = 0;
    double m_grainStep = 0.001;           // grain phase per sample
    double m_grainPhase = 0.0;            // the first grain's place in its window, 0..1
    std::array<double, 2> m_grainRead {}; // where each grain reads
    bool m_grainsLive = false;
    const AudioSource* m_render = nullptr; // the render this note plays, once chosen (PM2)
    juce::int64 m_renderOffset = 0;        // the file frame its first frame is
    int m_renderSemitones = 0;             // its pitch, from the root
    std::array<Window, 4> m_windows {};    // each grain's reads, and the loop's early side of each
};

// The sound of one note: a strand for each layer that holds it, each the first zone of its layer that does, or one of
// that zone's takes (docs/design/sampler-mapping.md, TL1, TL2).
class SampleSound
{
public:
    void prepare (double sampleRate) noexcept
    {
        for (auto& strand : m_strands)
            strand.prepare (sampleRate);
    }

    void stop() noexcept
    {
        for (auto& strand : m_strands)
            strand.stop();
    }

    [[nodiscard]] bool sounding() const noexcept
    {
        return std::ranges::any_of (m_strands, [] (const SampleStrand& strand) { return strand.sounding(); });
    }

    void adoptMap (const SampleMap* map) noexcept
    {
        m_map = map;

        for (auto& strand : m_strands)
            strand.adoptMap (map);
    }

    // Starts the note in each layer that holds it. No such layer leaves it silent.
    void start (const CompiledNote& note, float velocitySensitivity, bool randomTakes) noexcept
    {
        stop();

        if (m_map == nullptr)
            return;

        const auto key = static_cast<int> (std::lround (note.pitch));
        const auto velocity = static_cast<int> (std::lround (note.velocity * 127.0f));
        const auto& zones = m_map->zones;
        std::array<bool, static_cast<size_t> (maxChokeGroup) + 1> counted {};

        for (auto layer = 1; layer <= maxSampleLayers; ++layer)
        {
            const auto first =
                std::ranges::find_if (zones, [&] (const CompiledSampleZone& z)
                                      { return z.zone.layer == layer && holds (z.zone, key, velocity); });

            if (first == zones.end())
                continue;

            // Its takes: the zones of its layer with its keys and velocities (TL2).
            std::array<size_t, mostTakes> takes {};
            size_t count = 0;

            for (auto i = static_cast<size_t> (first - zones.begin()); i < zones.size() && count < mostTakes; ++i)
                if (sameTake (zones[i].zone, first->zone))
                    takes[count++] = i;

            const auto index = static_cast<size_t> (first - zones.begin());
            auto& strand = m_strands[static_cast<size_t> (layer - 1)];
            strand.start (zones[takes[pickTake (index, count, randomTakes)]], note, velocitySensitivity);

            // A note counts once in each choke group, so its own layers don't choke each other (TL3).
            if (const auto group = strand.chokeGroup(); group > 0)
            {
                if (! counted[group])
                {
                    ++m_map->chokes[group];
                    counted[group] = true;
                }

                strand.setChokeCount (m_map->chokes[group]);
            }
        }
    }

    void release() noexcept
    {
        for (auto& strand : m_strands)
            strand.release();
    }

    void choke() noexcept
    {
        for (auto& strand : m_strands)
            strand.choke();
    }

    // Each strand into its zone's output (MO1); `wired` has a bit per output port that leads somewhere.
    template <typename PitchAt>
    void render (std::span<const AudioOutput> outs, juce::uint32 wired, int from, int to, const PitchAt& pitchAt,
                 const float* levelSignal, float level, bool keepLength) noexcept
    {
        for (auto& strand : m_strands)
            if (strand.sounding())
                strand.render (outs[strand.outputPort (outs.size(), wired)], from, to, pitchAt, levelSignal, level,
                               keepLength);
    }

private:
    [[nodiscard]] static bool holds (const SampleZone& zone, int key, int velocity) noexcept
    {
        return key >= zone.lowKey && key <= zone.highKey && velocity >= zone.lowVelocity
               && velocity <= zone.highVelocity;
    }

    [[nodiscard]] static bool sameTake (const SampleZone& a, const SampleZone& b) noexcept
    {
        return a.layer == b.layer && a.lowKey == b.lowKey && a.highKey == b.highKey && a.lowVelocity == b.lowVelocity
               && a.highVelocity == b.highVelocity;
    }

    // Which of `count` takes plays: the next in turn, or any but the last at random (TL2).
    [[nodiscard]] size_t pickTake (size_t first, size_t count, bool random) const noexcept
    {
        if (count <= 1 || first >= m_map->takes.size())
            return 0;

        auto& last = m_map->takes[first];
        size_t pick = 0;

        if (random)
        {
            auto& state = m_map->random;
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const auto previous = last == 0 ? count : static_cast<size_t> (last - 1);
            pick = static_cast<size_t> (state % (previous < count ? count - 1 : count));

            if (previous < count && pick >= previous)
                ++pick;
        }
        else
        {
            pick = static_cast<size_t> (last) % count;
        }

        last = static_cast<juce::uint32> (pick + 1);
        return pick;
    }

    const SampleMap* m_map = nullptr;
    std::array<SampleStrand, maxSampleLayers> m_strands {};
};

// The track instrument: its own voices, which follow the notes' pitch curves and live expression.
class SamplerNode final : public Node
{
public:
    SamplerNode()
        : Node (samplerDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        for (auto& sound : m_sounds)
            sound.prepare (spec.sampleRate);

        reset();
    }

    void reset() noexcept override
    {
        m_voices.clear();

        for (auto& sound : m_sounds)
            sound.stop();
    }

    void configure (const GraphNode& node) override { m_bypassed.store (node.bypassed); }

    // The zones as last compiled or edited; they win over the step's, which only a recompile renews.
    void setSampleMap (std::shared_ptr<const SampleMap> map) override
    {
        if (map != nullptr)
            m_maps.publish (std::move (map));
    }

    void process (const ProcessContext& context) noexcept override
    {
        // Main, and every output that is wired (MO2).
        const auto clearOutputs = [&]
        {
            for (size_t port = 0; port < context.audioOutputs.size(); ++port)
                if (port == 0 || (port < 32 && (context.wiredOutputs & (1u << port)) != 0))
                    for (auto* channel : context.audioOutputs[port])
                        juce::FloatVectorOperations::clear (channel, context.numSamples);
        };

        clearOutputs();
        const auto* map = mapOf (context);

        for (auto& sound : m_sounds)
            sound.adoptMap (map);

        if (context.sequence != nullptr)
            m_voices.adoptSequence (*context.sequence);

        auto position = 0;

        if (context.eventsIn != nullptr)
        {
            for (const auto& event : context.eventsIn->events())
            {
                render (context, position, event.sampleOffset);
                position = std::max (position, event.sampleOffset);

                if (const auto started = m_voices.handle (event); started.has_value() && event.note != nullptr)
                    m_sounds[*started].start (*event.note, params().get (velocityParam) / 100.0f,
                                              params().get (takesParam) >= 0.5f);
            }
        }

        render (context, position, context.numSamples);

        if (m_bypassed.load())
            clearOutputs();
    }

private:
    void render (const ProcessContext& context, int from, int to) noexcept
    {
        for (size_t i = 0; i < VoiceAllocator::numVoices; ++i)
        {
            const auto state = m_voices.voices()[i].state;

            if (state == VoiceState::free)
                continue;

            auto& sound = m_sounds[i];

            if (state == VoiceState::releasing)
                sound.release();
            else if (state == VoiceState::choking)
                sound.choke();

            for (int s = from; s < to; s += controlInterval)
                renderSegment (context, i, s, std::min (to, s + controlInterval));

            if (! sound.sounding())
                m_voices.free (i);
        }
    }

    // Renders [from, to) of a voice with its pitch interpolated across the segment.
    void renderSegment (const ProcessContext& context, size_t index, int from, int to) noexcept
    {
        const auto& transport = *context.transport;
        const auto ticksPerSample = (transport.endTick - transport.startTick) / context.numSamples;
        const auto start = m_voices.voices()[index].expression.pitch;
        const auto end = m_voices.updateExpression (index, transport.startTick + ticksPerSample * to, transport).pitch;
        const auto* transpose = context.modulatedValues (pitchParam);
        const auto fixedTranspose = static_cast<double> (params().get (pitchParam));
        const auto length = static_cast<double> (to - from);
        const auto pitchAt = [&] (int s)
        {
            return start + (end - start) * static_cast<double> (s - from + 1) / length
                   + (transpose != nullptr ? static_cast<double> (transpose[s]) : fixedTranspose);
        };

        m_sounds[index].render (context.audioOutputs, context.wiredOutputs, from, to, pitchAt,
                                context.modulatedValues (levelParam), params().get (levelParam),
                                params().get (pitchModeParam) >= 0.5f);
    }

    VoiceAllocator m_voices { StealPolicy::releasedFirst };
    std::array<SampleSound, VoiceAllocator::numVoices> m_sounds {};
    std::atomic<bool> m_bypassed { false };
    SnapshotExchange<SampleMap> m_maps;

    // The zones to play: the newest handed over, else the step's.
    [[nodiscard]] const SampleMap* mapOf (const ProcessContext& context) noexcept
    {
        const auto* published = m_maps.acquire();
        return published != nullptr ? published : context.samples;
    }
};

// The same sound as one note of a Voice Instrument, which owns the note's life. A driven pitch parameter holds the
// note's pitch around middle C; left alone it transposes the note.
class SampleVoiceNode final : public Node
{
public:
    SampleVoiceNode()
        : Node (sampleVoiceDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sound.prepare (spec.sampleRate);
        reset();
    }

    void reset() noexcept override
    {
        m_sound.stop();
        m_held = false;
        m_pedal = false;
    }

    void configure (const GraphNode& node) override { m_bypassed.store (node.bypassed); }

    // The zones as last compiled or edited; they win over the step's, which only a recompile renews.
    void setSampleMap (std::shared_ptr<const SampleMap> map) override
    {
        if (map != nullptr)
            m_maps.publish (std::move (map));
    }

    void process (const ProcessContext& context) noexcept override
    {
        const auto& out = context.audioOutputs[0];
        juce::FloatVectorOperations::clear (out[0], context.numSamples);
        juce::FloatVectorOperations::clear (out[1], context.numSamples);

        m_sound.adoptMap (mapOf (context));
        auto position = 0;

        if (context.eventsIn != nullptr)
        {
            for (const auto& event : context.eventsIn->events())
            {
                render (context, position, std::min (event.sampleOffset, context.numSamples));
                position = std::max (position, event.sampleOffset);
                handle (event);
            }
        }

        render (context, position, context.numSamples);

        if (m_bypassed.load())
        {
            juce::FloatVectorOperations::clear (out[0], context.numSamples);
            juce::FloatVectorOperations::clear (out[1], context.numSamples);
        }
    }

private:
    void handle (const NoteEvent& event) noexcept
    {
        switch (event.type)
        {
            case NoteEventType::noteOn:
                if (event.note != nullptr)
                {
                    m_notePitch = event.note->pitch;
                    m_sound.start (*event.note, params().get (velocityParam) / 100.0f,
                                   params().get (takesParam) >= 0.5f);
                    m_held = true;
                }
                break;

            case NoteEventType::legato:
                // The sample plays on, at the new note's pitch.
                if (event.note != nullptr)
                {
                    m_notePitch = event.note->pitch;
                    m_held = true;
                }
                break;

            case NoteEventType::noteOff:
                m_held = false;
                if (! m_pedal)
                    m_sound.release();
                break;

            case NoteEventType::sustain:
                m_pedal = event.value >= 0.5f;
                if (! m_pedal && ! m_held)
                    m_sound.release();
                break;

            case NoteEventType::releaseAll:
                m_sound.release();
                break;

            case NoteEventType::choke:
            case NoteEventType::chokeAll:
            case NoteEventType::sourceChanged:
            case NoteEventType::expression:
            case NoteEventType::controller:
                break; // the Voice Instrument fades a choked voice; expression comes in through the pitch signal
        }
    }

    void render (const ProcessContext& context, int from, int to) noexcept
    {
        const auto* signal = context.modulatedValues (pitchParam);
        const auto fixed = static_cast<double> (params().get (pitchParam));
        const auto pitchAt = [&] (int s)
        { return signal != nullptr ? pitchSignalCentre + static_cast<double> (signal[s]) : m_notePitch + fixed; };

        m_sound.render (context.audioOutputs.first (1), 1u, from, to, pitchAt, context.modulatedValues (levelParam),
                        params().get (levelParam), params().get (pitchModeParam) >= 0.5f);
    }

    SampleSound m_sound;
    double m_notePitch = 60.0;
    bool m_held = false;
    bool m_pedal = false;
    std::atomic<bool> m_bypassed { false };
    SnapshotExchange<SampleMap> m_maps;

    // The zones to play: the newest handed over, else the step's.
    [[nodiscard]] const SampleMap* mapOf (const ProcessContext& context) noexcept
    {
        const auto* published = m_maps.acquire();
        return published != nullptr ? published : context.samples;
    }
};
} // namespace

void registerSampler (NodeRegistry& registry)
{
    registry.add (samplerDescriptor, [] { return std::make_unique<SamplerNode>(); });
    registry.add (sampleVoiceDescriptor, [] { return std::make_unique<SampleVoiceNode>(); });
}

} // namespace dawg
