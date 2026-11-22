#pragma once

#include <algorithm>
#include <cmath>

/**
    The envelopes of the Drum Synth's voices (docs/design/drum-synth.md, DR5): a level envelope with a linear attack,
    a hold and an exponential decay, and a pitch sweep that falls exponentially to the voice's pitch. Both cost a
    multiply or two per sample and allocate nothing.
*/
namespace dawg
{

/**
    A one-shot level envelope: a linear attack from 0 to 1, a hold at 1, then an exponential decay that reaches -60 dB
    at the decay time. A choke fades it out linearly from wherever it is.
*/
class DrumEnvelope final
{
public:
    /**
        The level below which a decaying envelope counts as finished, -100 dB.
    */
    static constexpr double silence = 1.0e-5;

    /**
        Starts the envelope.

        @param sampleRate     The sample rate.
        @param attackSeconds  The attack's length; 0 starts at full level.
        @param holdSeconds    How long the level stays at 1 after the attack.
        @param decaySeconds   The time from the decay's start to -60 dB.
    */
    void start (double sampleRate, double attackSeconds, double holdSeconds, double decaySeconds) noexcept
    {
        const auto attack = std::max (0.0, attackSeconds * sampleRate);
        m_attackStep = attack >= 1.0 ? 1.0 / attack : 1.0;
        m_value = attack >= 1.0 ? 0.0 : 1.0;
        m_holdLeft = static_cast<long long> (std::llround (std::max (0.0, holdSeconds * sampleRate)));
        // 1e-3 (-60 dB) after the decay's samples: c^n = 10^-3.
        m_decay = std::pow (10.0, -3.0 / std::max (1.0, decaySeconds * sampleRate));
        m_stage = m_value < 1.0 ? Stage::attack : Stage::hold;
        m_fade = 1.0;
        m_fadeStep = 0.0;
    }

    /**
        Fades the envelope out linearly over a time, as a choke or a stolen voice does. A finished envelope stays
        finished.

        @param sampleRate  The sample rate.
        @param seconds     The fade's length.
    */
    void choke (double sampleRate, double seconds) noexcept
    {
        if (m_stage == Stage::finished)
            return;

        m_fadeStep = std::min (m_fade, 1.0 / std::max (1.0, seconds * sampleRate));
    }

    /**
        Returns this sample's level and advances by one sample.
    */
    [[nodiscard]] float next() noexcept
    {
        const auto level = m_value * m_fade;

        switch (m_stage)
        {
            case Stage::attack:
                m_value += m_attackStep;

                if (m_value >= 1.0 - 1.0e-9) // the steps' rounding may leave it a hair short
                {
                    m_value = 1.0;
                    m_stage = Stage::hold;
                }
                break;

            case Stage::hold:
                if (m_holdLeft > 0)
                    --m_holdLeft;
                else
                    m_stage = Stage::decay;

                if (m_stage == Stage::decay)
                    m_value *= m_decay;
                break;

            case Stage::decay:
                m_value *= m_decay;

                if (m_value < silence)
                    finish();
                break;

            case Stage::finished:
                break;
        }

        if (m_fadeStep > 0.0)
        {
            m_fade -= m_fadeStep;

            if (m_fade <= 1.0e-9)
                finish();
        }

        return static_cast<float> (level);
    }

    /**
        Returns true once the envelope has decayed below silence or a choke has faded it out.
    */
    [[nodiscard]] bool finished() const noexcept { return m_stage == Stage::finished; }

    /**
        Returns true while a choke is fading the envelope out.
    */
    [[nodiscard]] bool choking() const noexcept { return m_fadeStep > 0.0 && ! finished(); }

private:
    enum class Stage
    {
        attack,
        hold,
        decay,
        finished,
    };

    void finish() noexcept
    {
        m_stage = Stage::finished;
        m_value = 0.0;
        m_fade = 0.0;
        m_fadeStep = 0.0;
    }

    Stage m_stage = Stage::finished;
    double m_value = 0.0;
    double m_attackStep = 1.0;
    long long m_holdLeft = 0;
    double m_decay = 0.0;
    double m_fade = 0.0;
    double m_fadeStep = 0.0;
};

/**
    A pitch sweep: an offset in semitones that starts at the sweep and falls exponentially towards 0, by a factor of e
    each sweep time, as a kick's or a tom's pitch drops onto its note.
*/
class PitchSweep final
{
public:
    /**
        Starts the sweep.

        @param sampleRate  The sample rate.
        @param semitones   The offset at the start.
        @param seconds     The time constant: the offset falls to 1 / e of where it was in this time.
    */
    void start (double sampleRate, double semitones, double seconds) noexcept
    {
        m_offset = semitones;
        m_factor = std::exp (-1.0 / std::max (1.0, seconds * sampleRate));
    }

    /**
        Returns this sample's offset in semitones and advances by one sample.
    */
    [[nodiscard]] double next() noexcept
    {
        const auto offset = m_offset;
        m_offset *= m_factor;
        return offset;
    }

    /**
        Returns this sample's frequency ratio, 2^(offset / 12), and advances by one sample.
    */
    [[nodiscard]] double nextRatio() noexcept
    {
        const auto offset = next();
        // Under a ten-thousandth of a semitone the ratio is 1 to within 6e-6: the sweep has landed.
        return std::abs (offset) < 1.0e-4 ? 1.0 : std::exp2 (offset / 12.0);
    }

private:
    double m_offset = 0.0;
    double m_factor = 0.0;
};

} // namespace dawg
