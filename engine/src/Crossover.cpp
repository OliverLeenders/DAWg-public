#include "dawg/Crossover.h"

#include <juce_dsp/juce_dsp.h>

#include <chrono>
#include <cmath>
#include <numbers>

// The linear-phase crossover bank (docs/design/split-nodes.md, SP11).

namespace dawg
{

namespace
{
constexpr auto block = static_cast<size_t> (LinearPhaseCrossovers::blockSize);

[[nodiscard]] int orderOf (size_t size) noexcept
{
    int order = 0;

    while ((size_t { 1 } << order) < size)
        ++order;

    return order;
}

// A Linkwitz-Riley 24 dB/octave crossover's magnitudes at a frequency: the low side's, and the high side's is one
// minus it, so each pair adds up to one.
[[nodiscard]] double lowMagnitude (double hz, double crossover) noexcept
{
    const auto r = hz / crossover;
    return 1.0 / (1.0 + r * r * r * r);
}
} // namespace

int LinearPhaseCrossovers::kernelLength (double sampleRate) noexcept
{
    // About 0.17 seconds, a power of two less one: long enough for a crossover low in the bass.
    const auto wanted = static_cast<size_t> (std::ceil (0.17 * std::max (sampleRate, 8000.0)));
    return (1 << std::max (orderOf (wanted), 11)) - 1;
}

LinearPhaseCrossovers::LinearPhaseCrossovers()
{
    for (auto& state : m_states)
        state.store (idle);

    m_thread = std::thread ([this] { run(); });
}

LinearPhaseCrossovers::~LinearPhaseCrossovers()
{
    {
        const std::scoped_lock lock (m_wakeLock);
        m_stop = true;
    }

    m_wake.notify_all();
    m_thread.join();
}

void LinearPhaseCrossovers::prepare (double sampleRate, int channels)
{
    const std::scoped_lock lock (m_buildLock);
    m_sampleRate = sampleRate;
    m_taps = kernelLength (sampleRate);
    m_partitions = static_cast<size_t> (m_taps + 1) / block;
    m_bins = (block + 1) * 2;

    for (size_t i = 0; i < slotCount; ++i)
    {
        m_slots[i].count = 0;
        m_slots[i].spectra.assign (maxKernels * m_partitions * m_bins, 0.0f);
        m_states[i].store (idle);
    }

    m_pending.store (-1);
    m_current = -1;
    m_old = -1;
    m_hasBuilt = false;

    size_t delaySize = 1;

    while (delaySize < static_cast<size_t> (latency()) + 1)
        delaySize *= 2;

    m_delayMask = delaySize - 1;
    m_channels.assign (static_cast<size_t> (std::max (channels, 1)), {});

    for (auto& channel : m_channels)
    {
        channel.input.assign (2 * block, 0.0f);
        channel.history.assign (m_partitions * m_bins, 0.0f);
        channel.delay.assign (delaySize, 0.0f);

        for (auto& out : channel.out)
            out.assign (block, 0.0f);
    }

    m_blockFft = std::make_unique<juce::dsp::FFT> (orderOf (2 * block));
    m_scratch.assign (4 * block, 0.0f);
    m_sum.assign (m_bins, 0.0f);

    for (auto& fade : m_fadeOut)
        fade.assign (block, 0.0f);
}

void LinearPhaseCrossovers::reset() noexcept
{
    for (auto& channel : m_channels)
    {
        std::ranges::fill (channel.input, 0.0f);
        std::ranges::fill (channel.history, 0.0f);
        std::ranges::fill (channel.delay, 0.0f);

        for (auto& out : channel.out)
            std::ranges::fill (out, 0.0f);

        channel.written = 0;
        channel.newest = 0;
        channel.position = 0;
        channel.fading = false;
    }

    if (m_old >= 0)
    {
        m_states[static_cast<size_t> (m_old)].store (idle, std::memory_order_release);
        m_old = -1;
    }
}

void LinearPhaseCrossovers::setFrequencies (std::span<const float> hz) noexcept
{
    storeWanted (hz);

    // A fade ends once the first channel has played its block: the others ran the same samples in the same call.
    if (m_old >= 0 && ! m_channels.empty() && ! m_channels[0].fading)
    {
        m_states[static_cast<size_t> (m_old)].store (idle, std::memory_order_release);
        m_old = -1;

        for (auto& channel : m_channels)
            channel.fading = false;
    }

    if (m_old >= 0)
        return;

    if (const auto next = m_pending.exchange (-1, std::memory_order_acq_rel); next >= 0)
    {
        m_states[static_cast<size_t> (next)].store (inUse, std::memory_order_relaxed);

        if (m_current >= 0)
        {
            m_old = m_current;

            for (auto& channel : m_channels)
                channel.fading = true;
        }

        m_current = next;
    }
}

void LinearPhaseCrossovers::process (size_t channelIndex, const float* input, std::span<float* const> bands, float* dry,
                                     int numSamples) noexcept
{
    if (bands.empty() || channelIndex >= m_channels.size())
        return;

    auto& channel = m_channels[channelIndex];
    const auto last = bands.size() - 1;
    const auto delay = static_cast<size_t> (latency());

    for (int s = 0; s < numSamples; ++s)
    {
        const auto x = input[s];
        const auto position = static_cast<size_t> (channel.position);
        channel.input[block + position] = x;
        channel.delay[channel.written & m_delayMask] = x;
        const auto delayed = channel.delay[(channel.written - delay) & m_delayMask];
        ++channel.written;

        // The highest band is what the others leave of the delayed input, so they add up to it exactly.
        auto sum = 0.0f;

        for (size_t b = 0; b < last; ++b)
        {
            const auto value = b < maxKernels ? channel.out[b][position] : 0.0f;
            bands[b][s] = value;
            sum += value;
        }

        bands[last][s] = delayed - sum;

        if (dry != nullptr)
            dry[s] = delayed;

        if (++channel.position == block)
        {
            runBlock (channel);
            channel.position = 0;
        }
    }
}

void LinearPhaseCrossovers::runBlock (Channel& channel) noexcept
{
    // The last two blocks' spectrum joins the history (overlap-save).
    std::copy (channel.input.begin(), channel.input.end(), m_scratch.begin());
    std::fill (m_scratch.begin() + static_cast<std::ptrdiff_t> (2 * block), m_scratch.end(), 0.0f);
    m_blockFft->performRealOnlyForwardTransform (m_scratch.data(), true);
    channel.newest = (channel.newest + 1) % m_partitions;
    std::copy_n (m_scratch.begin(), m_bins,
                 channel.history.begin() + static_cast<std::ptrdiff_t> (channel.newest * m_bins));
    std::copy_n (channel.input.begin() + static_cast<std::ptrdiff_t> (block), block, channel.input.begin());

    const auto* current = m_current >= 0 ? &m_slots[static_cast<size_t> (m_current)] : nullptr;
    const auto* old = channel.fading && m_old >= 0 ? &m_slots[static_cast<size_t> (m_old)] : nullptr;
    const auto result = m_scratch.begin() + static_cast<std::ptrdiff_t> (block);

    for (size_t b = 0; b < maxKernels; ++b)
    {
        auto& out = channel.out[b];

        if (current != nullptr && b < current->count)
        {
            convolve (channel, *current, b);
            std::copy_n (result, block, out.begin());
        }
        else
        {
            std::ranges::fill (out, 0.0f);
        }

        if (old == nullptr)
            continue;

        // New kernels fade in over the block, from what the old ones give.
        auto& fade = m_fadeOut[b];

        if (b < old->count)
        {
            convolve (channel, *old, b);
            std::copy_n (result, block, fade.begin());
        }
        else
        {
            std::ranges::fill (fade, 0.0f);
        }

        for (size_t i = 0; i < block; ++i)
            out[i] = fade[i] + (out[i] - fade[i]) * static_cast<float> (i + 1) / static_cast<float> (block);
    }

    channel.fading = false;
}

void LinearPhaseCrossovers::convolve (const Channel& channel, const Kernels& kernels, size_t kernel) noexcept
{
    std::ranges::fill (m_sum, 0.0f);

    for (size_t p = 0; p < m_partitions; ++p)
    {
        const auto* x = channel.history.data() + ((channel.newest + m_partitions - p) % m_partitions) * m_bins;
        const auto* h = kernels.spectra.data() + (kernel * m_partitions + p) * m_bins;

        for (size_t i = 0; i < m_bins; i += 2)
        {
            m_sum[i] += x[i] * h[i] - x[i + 1] * h[i + 1];
            m_sum[i + 1] += x[i] * h[i + 1] + x[i + 1] * h[i];
        }
    }

    std::ranges::fill (m_scratch, 0.0f);
    std::ranges::copy (m_sum, m_scratch.begin());
    m_blockFft->performRealOnlyInverseTransform (m_scratch.data());
}

void LinearPhaseCrossovers::setInitial (std::span<const float> hz)
{
    storeWanted (hz);
    bool built = false;

    {
        const std::scoped_lock lock (m_buildLock);
        built = m_hasBuilt;
    }

    if (! built)
        buildIfWanted();

    m_wake.notify_all();
}

bool LinearPhaseCrossovers::settle()
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (5);

    while (std::chrono::steady_clock::now() < until)
    {
        {
            const std::scoped_lock lock (m_buildLock);

            if (m_partitions == 0)
                return false;

            if (m_hasBuilt && wanted().near (m_built))
                return true;
        }

        m_wake.notify_all();
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    return false;
}

bool LinearPhaseCrossovers::Wanted::near (const Wanted& other) const noexcept
{
    if (count != other.count)
        return false;

    for (size_t i = 0; i < count; ++i)
        if (std::abs (hz[i] - other.hz[i]) > 1.0e-4f * std::max (hz[i], other.hz[i]))
            return false;

    return true;
}

LinearPhaseCrossovers::Wanted LinearPhaseCrossovers::wanted() const noexcept
{
    Wanted result;
    result.count = std::min (m_wantedCount.load (std::memory_order_acquire), maxKernels);

    for (size_t i = 0; i < result.count; ++i)
        result.hz[i] = m_wantedHz[i].load (std::memory_order_relaxed);

    return result;
}

void LinearPhaseCrossovers::storeWanted (std::span<const float> hz) noexcept
{
    const auto count = std::min (hz.size(), maxKernels);

    for (size_t i = 0; i < count; ++i)
        m_wantedHz[i].store (hz[i], std::memory_order_relaxed);

    m_wantedCount.store (count, std::memory_order_release);
}

void LinearPhaseCrossovers::run()
{
    std::unique_lock lock (m_wakeLock);

    while (! m_stop)
    {
        // The audio thread can't wake this thread, so it looks for new frequencies every few milliseconds.
        m_wake.wait_for (lock, std::chrono::milliseconds (10), [this] { return m_stop; });

        if (m_stop)
            break;

        lock.unlock();
        buildIfWanted();
        lock.lock();
    }
}

void LinearPhaseCrossovers::buildIfWanted()
{
    const std::scoped_lock lock (m_buildLock);

    if (m_partitions == 0)
        return;

    const auto target = wanted();

    if (m_hasBuilt && target.near (m_built))
        return;

    for (size_t i = 0; i < slotCount; ++i)
    {
        auto expected = static_cast<int> (idle);

        if (! m_states[i].compare_exchange_strong (expected, building, std::memory_order_acquire))
            continue;

        build (m_slots[i], target);
        m_built = target;
        m_hasBuilt = true;
        m_states[i].store (waiting, std::memory_order_relaxed);

        // Kernels the audio thread never took go back to the free ones.
        if (const auto superseded = m_pending.exchange (static_cast<int> (i), std::memory_order_acq_rel);
            superseded >= 0)
            m_states[static_cast<size_t> (superseded)].store (idle, std::memory_order_release);

        return;
    }
}

void LinearPhaseCrossovers::build (Kernels& target, const Wanted& wanted)
{
    // Each band's magnitude on a grid twice the kernel's length, zero phase, turned into an impulse, windowed around
    // its centre and cut into the convolution's partitions. A window that is 1 at the centre keeps the kernels' sum,
    // with the highest band's, a pure delay.
    const auto taps = static_cast<size_t> (m_taps);
    const auto size = 2 * (taps + 1);
    const auto centre = (taps - 1) / 2;
    const juce::dsp::FFT design (orderOf (size));
    const juce::dsp::FFT partition (orderOf (2 * block));
    std::vector<float> spectrum (2 * size);
    std::vector<float> kernel (taps);
    std::vector<float> buffer (4 * block);

    for (size_t b = 0; b < wanted.count; ++b)
    {
        std::ranges::fill (spectrum, 0.0f);

        for (size_t k = 0; k <= size / 2; ++k)
        {
            const auto hz = static_cast<double> (k) * m_sampleRate / static_cast<double> (size);
            auto magnitude = lowMagnitude (hz, static_cast<double> (wanted.hz[b]));

            for (size_t j = 0; j < b; ++j)
                magnitude *= 1.0 - lowMagnitude (hz, static_cast<double> (wanted.hz[j]));

            spectrum[2 * k] = static_cast<float> (magnitude);
        }

        design.performRealOnlyInverseTransform (spectrum.data());

        for (size_t t = 0; t < taps; ++t)
        {
            const auto phase = 2.0 * std::numbers::pi * static_cast<double> (t) / static_cast<double> (taps - 1);
            const auto window = 0.42 - 0.5 * std::cos (phase) + 0.08 * std::cos (2.0 * phase);
            const auto index = (t + size - centre) % size;
            kernel[t] = spectrum[index] * static_cast<float> (window);
        }

        for (size_t p = 0; p < m_partitions; ++p)
        {
            std::ranges::fill (buffer, 0.0f);
            const auto first = p * block;
            const auto count = std::min (block, taps - std::min (taps, first));
            std::copy_n (kernel.begin() + static_cast<std::ptrdiff_t> (first), count, buffer.begin());
            partition.performRealOnlyForwardTransform (buffer.data(), true);
            std::copy_n (buffer.begin(), m_bins,
                         target.spectra.begin() + static_cast<std::ptrdiff_t> ((b * m_partitions + p) * m_bins));
        }
    }

    target.hz = wanted.hz;
    target.count = wanted.count;
}

} // namespace dawg
