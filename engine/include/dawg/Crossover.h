#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace juce::dsp
{
class FFT;
}

namespace dawg
{

/**
    Splits a signal into 2 to 4 bands for the Band Split node (docs/design/split-nodes.md, SP2, SP11): minimum phase
    (MinimumPhaseCrossovers) or linear phase (LinearPhaseCrossovers). The node's Solo and port handling sit outside
    it.

    @note  prepare(): message thread. setFrequencies() and process(): audio thread, without allocating.
*/
class CrossoverBank
{
public:
    /**
        The most bands, and so the most crossovers plus one.
    */
    static constexpr int maxBands = 4;

    virtual ~CrossoverBank() = default;

    /**
        Sizes the bank and clears it.

        @param sampleRate  The sample rate.
        @param channels    The most channels process() is called for.
    */
    virtual void prepare (double sampleRate, int channels) = 0;

    /**
        Clears every channel's state.
    */
    virtual void reset() noexcept = 0;

    /**
        Sets the crossovers for the samples processed next, lowest first, each above the one before.

        @param hz  The crossover frequencies: one fewer than the bands.
    */
    virtual void setFrequencies (std::span<const float> hz) noexcept = 0;

    /**
        Returns the delay the bank adds, in samples.
    */
    [[nodiscard]] virtual int latency() const noexcept = 0;

    /**
        Splits one channel's samples.

        @param channel     The channel, below what prepare() was given.
        @param input       numSamples samples.
        @param bands       One destination per band, lowest first; the number of bands is its size.
        @param dry         Where the input goes, through what the bands went through (all-passes, a delay), so it
                           matches their sum (SP3).
        @param numSamples  How many samples.
    */
    virtual void process (size_t channel, const float* input, std::span<float* const> bands, float* dry,
                          int numSamples) noexcept = 0;

protected:
    /** @cond */
    CrossoverBank() = default;
    CrossoverBank (const CrossoverBank&) = default;
    CrossoverBank& operator= (const CrossoverBank&) = default;
    CrossoverBank (CrossoverBank&&) = default;
    CrossoverBank& operator= (CrossoverBank&&) = default;
    /** @endcond */
};

namespace crossover
{
/**
    A Butterworth second-order section as a TPT state-variable filter (V. Zavalishin, "The Art of VA Filter Design"):
    its coefficients at a frequency, and its low-, band- and high-pass outputs. As a bilinear-transform design, two in
    series make a Linkwitz-Riley crossover whose low and high sum exactly to the section's all-pass.
*/
struct Section
{
    float k = 1.41421356f; ///< Damping, 1 / Q: sqrt 2 for Butterworth.
    float a1 = 0.0f;       ///< 1 / (1 + g (g + k)), with g = tan (pi f / rate).
    float a2 = 0.0f;       ///< g a1.
    float a3 = 0.0f;       ///< g a2.

    /**
        A section at a frequency, kept below 0.49 of the sample rate.

        @param hz          The frequency.
        @param sampleRate  The sample rate.
    */
    [[nodiscard]] static Section at (double hz, double sampleRate) noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        const auto g = std::tan (pi * std::min (hz, 0.49 * sampleRate) / sampleRate);
        Section section;
        const auto k = static_cast<double> (section.k);
        section.a1 = static_cast<float> (1.0 / (1.0 + g * (g + k)));
        section.a2 = static_cast<float> (g) * section.a1;
        section.a3 = static_cast<float> (g) * section.a2;
        return section;
    }
};

/**
    One section's state.
*/
struct State
{
    float ic1 = 0.0f; ///< The first integrator's state.
    float ic2 = 0.0f; ///< The second integrator's state.
};

/**
    A section's outputs for one sample.
*/
struct Outputs
{
    float low;  ///< The low-pass output.
    float band; ///< The band-pass output.
    float high; ///< The high-pass output.
};

[[nodiscard]] inline Outputs tick (const Section& c, State& s, float x) noexcept
{
    const auto v3 = x - s.ic2;
    const auto v1 = c.a1 * s.ic1 + c.a2 * v3;
    const auto v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;
    return { v2, v1, x - c.k * v1 - v2 };
}

/**
    The section's all-pass: what a Linkwitz-Riley crossover's two bands add up to.
*/
[[nodiscard]] inline float allpass (const Section& c, State& s, float x) noexcept
{
    const auto out = tick (c, s, x);
    return x - 2.0f * c.k * out.band;
}

/**
    A 24 dB/octave Linkwitz-Riley crossover's state: one section for the first stage of both sides, one more each for
    the second.
*/
struct LinkwitzRiley4
{
    State first; ///< The first stage, both sides.
    State low;   ///< The low side's second stage.
    State high;  ///< The high side's second stage.

    /**
        Splits a sample: { low, high }, which add up to the all-pass.
    */
    [[nodiscard]] std::array<float, 2> split (const Section& c, float x) noexcept
    {
        const auto stage = tick (c, first, x);
        return { tick (c, low, stage.low).low, tick (c, high, stage.high).high };
    }
};
} // namespace crossover

/**
    The minimum-phase crossover bank (SP2): Linkwitz-Riley 24 dB/octave crossovers in a chain, the low side of each
    through the all-passes of the crossovers above it, so the bands always add up to an all-pass of the input. No
    latency; the frequencies can move every few samples without clicks.
*/
class MinimumPhaseCrossovers final : public CrossoverBank
{
public:
    void prepare (double sampleRate, int channels) override
    {
        m_sampleRate = sampleRate;
        m_channels.assign (static_cast<size_t> (std::max (channels, 1)), {});
    }

    void reset() noexcept override
    {
        for (auto& channel : m_channels)
            channel = {};
    }

    void setFrequencies (std::span<const float> hz) noexcept override
    {
        m_count = std::min (hz.size(), m_sections.size());

        for (size_t i = 0; i < m_count; ++i)
            m_sections[i] = crossover::Section::at (static_cast<double> (hz[i]), m_sampleRate);
    }

    [[nodiscard]] int latency() const noexcept override { return 0; }

    void process (size_t channel, const float* input, std::span<float* const> bands, float* dry,
                  int numSamples) noexcept override
    {
        auto& state = m_channels[channel];
        const auto crossovers = std::min (m_count, bands.empty() ? size_t { 0 } : bands.size() - 1);

        for (int s = 0; s < numSamples; ++s)
        {
            const auto x = input[s];
            auto rest = x;

            for (size_t i = 0; i < crossovers; ++i)
            {
                const auto [low, high] = state.crossovers[i].split (m_sections[i], rest);
                auto band = low;

                // Through the all-passes of the crossovers above, as the bands above went through those crossovers.
                for (size_t j = i + 1; j < crossovers; ++j)
                    band = crossover::allpass (m_sections[j], state.compensation[i][j], band);

                bands[i][s] = band;
                rest = high;
            }

            if (! bands.empty())
                bands[crossovers][s] = rest;

            if (dry != nullptr)
            {
                auto through = x;

                for (size_t i = 0; i < crossovers; ++i)
                    through = crossover::allpass (m_sections[i], state.dry[i], through);

                dry[s] = through;
            }
        }
    }

private:
    static constexpr size_t maxCrossovers = static_cast<size_t> (maxBands - 1);

    struct Channel
    {
        std::array<crossover::LinkwitzRiley4, maxCrossovers> crossovers {};
        std::array<std::array<crossover::State, maxCrossovers>, maxCrossovers> compensation {};
        std::array<crossover::State, maxCrossovers> dry {};
    };

    double m_sampleRate = 48000.0;
    std::array<crossover::Section, maxCrossovers> m_sections {};
    size_t m_count = 0;
    std::vector<Channel> m_channels;
};

/**
    The linear-phase crossover bank (SP11): each band but the highest is a linear-phase FIR band-pass with the
    magnitude of its Linkwitz-Riley counterpart, applied by uniformly partitioned FFT convolution; the highest band is
    the delayed input minus the others. So the bands add up to the input delayed by latency(), exactly, with no phase
    turn, and the Dry port is that delayed input.

    The kernels are about 0.17 seconds long whatever the crossovers (so the latency is fixed for a sample rate) and are
    built off the audio thread, by a thread of the bank's own: the audio thread asks for new frequencies through
    setFrequencies() and switches to the new kernels at a block boundary, crossfading over one block.

    @note  prepare(), setInitial() and settle(): message thread. setFrequencies() and process(): audio thread, without
           allocating or blocking.
*/
class LinearPhaseCrossovers final : public CrossoverBank
{
public:
    /**
        The convolution's block: input is gathered this many samples at a time, which is part of the latency.
    */
    static constexpr int blockSize = 512;

    /**
        The kernels' length in taps at a sample rate: odd, so they have a centre.

        @param sampleRate  The sample rate.
    */
    [[nodiscard]] static int kernelLength (double sampleRate) noexcept;

    /**
        The latency at a sample rate: the block plus half a kernel.

        @param sampleRate  The sample rate.
    */
    [[nodiscard]] static int latencyFor (double sampleRate) noexcept
    {
        return blockSize + (kernelLength (sampleRate) - 1) / 2;
    }

    LinearPhaseCrossovers();
    ~LinearPhaseCrossovers() override;

    /** @cond */
    LinearPhaseCrossovers (const LinearPhaseCrossovers&) = delete;
    LinearPhaseCrossovers& operator= (const LinearPhaseCrossovers&) = delete;
    LinearPhaseCrossovers (LinearPhaseCrossovers&&) = delete;
    LinearPhaseCrossovers& operator= (LinearPhaseCrossovers&&) = delete;
    /** @endcond */

    void prepare (double sampleRate, int channels) override;
    void reset() noexcept override;
    void setFrequencies (std::span<const float> hz) noexcept override;
    [[nodiscard]] int latency() const noexcept override { return latencyFor (m_sampleRate); }
    void process (size_t channel, const float* input, std::span<float* const> bands, float* dry,
                  int numSamples) noexcept override;

    /**
        Sets the crossovers and, if the bank has no kernels yet, builds them now, so the first block already splits.
        Later changes go through setFrequencies().

        @param hz  The crossover frequencies, lowest first.
    */
    void setInitial (std::span<const float> hz);

    /**
        Waits until the kernels for the latest frequencies are built (tests).

        @return  False if that took more than five seconds.
    */
    [[nodiscard]] bool settle();

private:
    static constexpr size_t maxKernels = static_cast<size_t> (maxBands - 1);
    static constexpr size_t slotCount = 4; // in use, fading out, waiting, being built

    enum SlotState : int
    {
        idle,
        building,
        waiting,
        inUse
    };

    struct Kernels
    {
        std::array<float, maxKernels> hz {};
        size_t count = 0;
        std::vector<float> spectra; // [kernel][partition][bins]
    };

    struct Channel
    {
        std::vector<float> input;                       // the last two blocks
        std::vector<float> history;                     // [partition][bins], a ring of input spectra
        std::array<std::vector<float>, maxKernels> out; // the bands of the block playing now
        std::vector<float> delay;                       // the input, for the Dry port and the highest band
        size_t written = 0;
        size_t newest = 0;
        int position = 0;
        bool fading = false;
    };

    struct Wanted
    {
        std::array<float, maxKernels> hz {};
        size_t count = 0;

        [[nodiscard]] bool near (const Wanted& other) const noexcept;
    };

    void run();
    void buildIfWanted();
    void build (Kernels& target, const Wanted& wanted);
    void runBlock (Channel& channel) noexcept;
    void convolve (const Channel& channel, const Kernels& kernels, size_t kernel) noexcept;
    [[nodiscard]] Wanted wanted() const noexcept;
    void storeWanted (std::span<const float> hz) noexcept;

    double m_sampleRate = 48000.0;
    int m_taps = 0;
    size_t m_partitions = 0;
    size_t m_bins = 0; // floats per spectrum: (block + 1) complex values
    size_t m_delayMask = 0;

    std::array<Kernels, slotCount> m_slots;
    std::array<std::atomic<int>, slotCount> m_states {};
    std::atomic<int> m_pending { -1 };
    int m_current = -1; // audio thread
    int m_old = -1;     // audio thread: the kernels fading out

    std::vector<Channel> m_channels;
    std::unique_ptr<juce::dsp::FFT> m_blockFft;           // audio thread
    std::vector<float> m_scratch;                         // audio thread
    std::vector<float> m_sum;                             // audio thread
    std::array<std::vector<float>, maxKernels> m_fadeOut; // audio thread: the old kernels' block

    std::array<std::atomic<float>, maxKernels> m_wantedHz {};
    std::atomic<size_t> m_wantedCount { 0 };

    std::mutex m_buildLock; // one build at a time, worker or message thread
    Wanted m_built;
    bool m_hasBuilt = false;

    std::mutex m_wakeLock;
    std::condition_variable m_wake;
    bool m_stop = false;
    std::thread m_thread;
};

} // namespace dawg
