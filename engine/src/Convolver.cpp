#include "dawg/Convolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

// The Convolution Reverb's engine (docs/design/convolution-reverb.md). The convolver splits an IR into a direct head
// and three levels of uniformly partitioned overlap-save convolution with a frequency-domain delay line (I2); the
// builder shapes, resamples and transforms IRs on its own thread (I4) and hands programs to the audio thread (I6).

namespace dawg::convolution
{

namespace
{
constexpr double pi = juce::MathConstants<double>::pi;

// The largest level's FFT size, and its spectra's interleaved length.
constexpr int largestFft = 2 * partitionSizes.back();
constexpr int largestBins = partitionSizes.back() + 1;

// A new history takes at most this many bytes of the old one's newest level-3 spectra per channel (I6).
constexpr size_t maxHistoryCopyBytes = size_t { 2 } << 20;

constexpr std::array<Route, 2> monoRoutes { { { 0, 0, 0 }, { 0, 1, 1 } } };
constexpr std::array<Route, 2> stereoRoutes { { { 0, 0, 0 }, { 1, 1, 1 } } };
constexpr std::array<Route, 4> trueStereoRoutes { { { 0, 0, 0 }, { 1, 0, 1 }, { 2, 1, 0 }, { 3, 1, 1 } } };

[[nodiscard]] int orderOf (int fftSize) noexcept
{
    int order = 0;

    while ((1 << order) < fftSize)
        ++order;

    return order;
}

// How many partitions of a level an IR of this length fills.
[[nodiscard]] int partitionsOf (size_t level, int length) noexcept
{
    const auto size = partitionSizes[level];
    const auto start = levelStarts[level];
    const auto end = level + 1 < levelStarts.size() ? std::min (length, levelStarts[level + 1]) : length;
    return end > start ? (end - start + size - 1) / size : 0;
}

// A raised-cosine ramp from 0 to 1 over `length` samples, at sample i.
[[nodiscard]] float ramp (int i, int length) noexcept
{
    return static_cast<float> (0.5 - 0.5 * std::cos (pi * (i + 0.5) / length));
}

// The zeroth-order modified Bessel function, for the Kaiser window.
[[nodiscard]] double besselI0 (double x) noexcept
{
    auto sum = 1.0;
    auto term = 1.0;

    for (int k = 1; k < 50; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;

        if (term < 1.0e-12 * sum)
            break;
    }

    return sum;
}

// A deterministic noise source, the same on every platform.
class Noise
{
public:
    explicit Noise (std::uint32_t seed) noexcept
        : m_state (seed != 0 ? seed : 1u)
    {
    }

    [[nodiscard]] float next() noexcept
    {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;
        return static_cast<float> (m_state) / 2147483648.0f - 1.0f;
    }

private:
    std::uint32_t m_state;
};

// The energy reaching each output, averaged over the two (I5).
[[nodiscard]] double outputEnergy (const juce::AudioBuffer<float>& ir, Layout layout, int frames) noexcept
{
    std::array<double, 4> channelEnergy {};

    for (int ch = 0; ch < std::min (channelsOf (layout), ir.getNumChannels()); ++ch)
    {
        const auto* data = ir.getReadPointer (ch);

        for (int i = 0; i < frames; ++i)
            channelEnergy[static_cast<size_t> (ch)] += static_cast<double> (data[i]) * data[i];
    }

    auto sum = 0.0;

    for (const auto& route : routesOf (layout))
        sum += channelEnergy[static_cast<size_t> (route.channel)];

    return sum / 2.0;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------
// Layouts

Layout layoutFor (int channels) noexcept
{
    if (channels == 1)
        return Layout::mono;

    return channels == 4 ? Layout::trueStereo : Layout::stereo;
}

int channelsOf (Layout layout) noexcept
{
    switch (layout)
    {
        case Layout::mono:
            return 1;
        case Layout::stereo:
            return 2;
        case Layout::trueStereo:
            break;
    }

    return 4;
}

std::span<const Route> routesOf (Layout layout) noexcept
{
    switch (layout)
    {
        case Layout::mono:
            return monoRoutes;
        case Layout::stereo:
            return stereoRoutes;
        case Layout::trueStereo:
            return trueStereoRoutes;
    }

    return stereoRoutes;
}

int level3Partitions (int length) noexcept
{
    return partitionsOf (2, length);
}

// ---------------------------------------------------------------------------------------------------------
// History

History::History (int level3Capacity)
{
    for (size_t level = 0; level < levels.size(); ++level)
    {
        const auto size = partitionSizes[level];
        auto& history = levels[level];
        history.capacity = level < 2 ? partitionsOf (level, levelStarts[level + 1]) : std::max (1, level3Capacity);

        for (size_t ch = 0; ch < 2; ++ch)
        {
            history.window[ch].assign (2 * static_cast<size_t> (size), 0.0f);
            history.ring[ch].assign (static_cast<size_t> (history.capacity) * static_cast<size_t> (size + 1) * 2, 0.0f);
        }
    }
}

void History::clear() noexcept
{
    for (auto& ring : headRing)
        ring.fill (0.0f);

    headPosition = 0;

    for (auto& level : levels)
    {
        for (size_t ch = 0; ch < 2; ++ch)
        {
            std::fill (level.window[ch].begin(), level.window[ch].end(), 0.0f);
            std::fill (level.ring[ch].begin(), level.ring[ch].end(), 0.0f);
        }

        level.newest = 0;
    }
}

// ---------------------------------------------------------------------------------------------------------
// ProgramExchange

void ProgramExchange::publish (std::shared_ptr<const Program> program)
{
    collectGarbage();
    const auto* raw = program.get();
    m_owned.push_back (std::move (program));

    // A program still waiting in the slot was never seen by the audio thread, so it can go.
    if (const auto* superseded = m_latest.exchange (raw))
        release (superseded);
}

void ProgramExchange::collectGarbage()
{
    while (const auto retired = m_retired.pop())
        release (*retired);
}

void ProgramExchange::release (const Program* program)
{
    std::erase_if (m_owned, [program] (const auto& owned) { return owned.get() == program; });
}

// ---------------------------------------------------------------------------------------------------------
// Convolver

Convolver::Convolver() = default;

void Convolver::prepare()
{
    for (size_t level = 0; level < m_ffts.size(); ++level)
        if (m_ffts[level] == nullptr)
            m_ffts[level] = std::make_unique<juce::dsp::FFT> (orderOf (2 * partitionSizes[level]));

    for (auto& slot : m_slots)
    {
        for (size_t level = 0; level < slot.output.size(); ++level)
            for (auto& channel : slot.output[level])
                channel.assign (static_cast<size_t> (partitionSizes[level]), 0.0f);

        for (auto& sum : slot.spread)
            sum.assign (2 * static_cast<size_t> (largestBins), 0.0f);
    }

    m_scratch.assign (2 * static_cast<size_t> (largestFft), 0.0f);

    for (auto& accumulator : m_accumulator)
        accumulator.assign (2 * static_cast<size_t> (largestBins), 0.0f);

    m_position = 0;
    m_fadePosition = 0;
}

void Convolver::clear (ProgramExchange& exchange) noexcept
{
    // Finish any change at once: the newest program stays.
    if (m_preparing >= 0)
    {
        if (m_current >= 0)
        {
            exchange.retire (m_slots[static_cast<size_t> (m_current)].program);
            m_slots[static_cast<size_t> (m_current)].program = nullptr;
        }

        m_current = m_preparing;
        m_preparing = -1;
    }

    if (m_previous >= 0)
    {
        exchange.retire (m_slots[static_cast<size_t> (m_previous)].program);
        m_slots[static_cast<size_t> (m_previous)].program = nullptr;
        m_previous = -1;
    }

    const auto* current = m_current >= 0 ? m_slots[static_cast<size_t> (m_current)].program : nullptr;

    for (const auto* program : { current, m_pending })
        if (program != nullptr)
            program->history->clear();

    if (current != nullptr)
        install (current, m_current);

    m_position = 0;
    m_fadePosition = 0;
}

void Convolver::reset (ProgramExchange& exchange) noexcept
{
    for (auto& slot : m_slots)
    {
        if (slot.program != nullptr)
        {
            slot.program->history->clear();
            exchange.retire (slot.program);
            slot.program = nullptr;
        }
    }

    if (m_pending != nullptr)
    {
        m_pending->history->clear();
        exchange.retire (m_pending);
        m_pending = nullptr;
    }

    m_current = m_previous = m_preparing = -1;
    m_position = 0;
    m_fadePosition = 0;
}

const Kernel* Convolver::kernelOf (int slot) const noexcept
{
    if (slot < 0)
        return nullptr;

    const auto* program = m_slots[static_cast<size_t> (slot)].program;
    const auto* kernel = program != nullptr ? program->kernel.get() : nullptr;
    return kernel != nullptr && juce::approximatelyEqual (kernel->sampleRate, m_rate) ? kernel : nullptr;
}

int Convolver::freeSlot() const noexcept
{
    for (int i = 0; i < static_cast<int> (m_slots.size()); ++i)
        if (i != m_current && i != m_previous && i != m_preparing)
            return i;

    return -1;
}

void Convolver::install (const Program* program, int slot) noexcept
{
    auto& target = m_slots[static_cast<size_t> (slot)];
    target.program = program;

    for (auto& level : target.output)
        for (auto& channel : level)
            std::fill (channel.begin(), channel.end(), 0.0f);

    for (auto& sum : target.spread)
        std::fill (sum.begin(), sum.end(), 0.0f);
}

// A larger history takes over the old one's newest state, so the tail carries on (I6).
void Convolver::adoptHistory (const Program* from, const Program* to) noexcept
{
    auto& fresh = *to->history;

    if (! fresh.fresh)
        return;

    fresh.fresh = false;

    if (from == nullptr || from->history.get() == &fresh)
        return;

    const auto& old = *from->history;
    fresh.headRing = old.headRing;
    fresh.headPosition = old.headPosition;

    for (size_t level = 0; level < fresh.levels.size(); ++level)
    {
        const auto& source = old.levels[level];
        auto& target = fresh.levels[level];
        const auto bins = static_cast<size_t> (partitionSizes[level] + 1) * 2;
        const auto limit = static_cast<int> (maxHistoryCopyBytes / (bins * sizeof (float)));
        const auto count = std::min ({ source.capacity, target.capacity, limit });

        for (size_t ch = 0; ch < 2; ++ch)
        {
            std::copy (source.window[ch].begin(), source.window[ch].end(), target.window[ch].begin());

            for (int k = 0; k < count; ++k)
            {
                const auto older = (source.newest - k + source.capacity) % source.capacity;
                const auto into = count - 1 - k;
                std::memcpy (target.ring[ch].data() + static_cast<size_t> (into) * bins,
                             source.ring[ch].data() + static_cast<size_t> (older) * bins, bins * sizeof (float));
            }
        }

        target.newest = std::max (0, count - 1);
    }
}

int Convolver::histories (std::array<History*, 3>& result) const noexcept
{
    int count = 0;

    for (const auto slot : { m_current, m_previous, m_preparing })
    {
        if (slot < 0)
            continue;

        auto* history = m_slots[static_cast<size_t> (slot)].program->history.get();

        if (std::find (result.begin(), result.begin() + count, history) == result.begin() + count)
            result[static_cast<size_t> (count++)] = history;
    }

    return count;
}

// The level's input block into the spectra of every history in use, and its window moved on.
void Convolver::transform (size_t level) noexcept
{
    std::array<History*, 3> targets {};
    const auto count = histories (targets);

    if (count == 0)
        return;

    const auto size = partitionSizes[level];
    const auto bins = static_cast<size_t> (size + 1) * 2;
    const auto& fft = *m_ffts[level];

    for (size_t ch = 0; ch < 2; ++ch)
    {
        // Every history has seen the same input, so the first one's window serves all.
        const auto& window = targets[0]->levels[level].window[ch];
        std::fill (m_scratch.begin(), m_scratch.begin() + 4 * static_cast<std::ptrdiff_t> (size), 0.0f);
        std::copy (window.begin(), window.end(), m_scratch.begin());
        fft.performRealOnlyForwardTransform (m_scratch.data(), true);

        for (int i = 0; i < count; ++i)
        {
            auto& state = targets[static_cast<size_t> (i)]->levels[level];

            if (ch == 0)
                state.newest = (state.newest + 1) % state.capacity;

            std::copy_n (m_scratch.begin(), bins,
                         state.ring[ch].begin()
                             + static_cast<std::ptrdiff_t> (static_cast<size_t> (state.newest) * bins));

            // The current block becomes the previous one.
            std::copy (state.window[ch].begin() + size, state.window[ch].end(), state.window[ch].begin());
        }
    }
}

// Adds partitions [first, last) of a level, times the input spectra they meet, into one sum per output.
void Convolver::addPartitions (int slot, size_t level, int first, int last, std::array<float*, 2> sums) noexcept
{
    const auto* kernel = kernelOf (slot);

    if (kernel == nullptr)
        return;

    const auto& history = m_slots[static_cast<size_t> (slot)].program->history->levels[level];
    const auto& spectra = kernel->levels[level];
    last = std::min ({ last, spectra.partitions, history.capacity });
    const auto bins = static_cast<size_t> (partitionSizes[level] + 1) * 2;

    for (const auto& route : routesOf (kernel->layout))
    {
        auto* sum = sums[static_cast<size_t> (route.output)];
        const auto& ring = history.ring[static_cast<size_t> (route.input)];

        for (int k = first; k < last; ++k)
        {
            const auto slotIndex = (history.newest - k + history.capacity) % history.capacity;
            const auto* x = ring.data() + static_cast<size_t> (slotIndex) * bins;
            const auto* h =
                spectra.spectra.data() + (static_cast<size_t> (route.channel * spectra.partitions + k)) * bins;

            for (size_t b = 0; b < bins; b += 2)
            {
                sum[b] += x[b] * h[b] - x[b + 1] * h[b + 1];
                sum[b + 1] += x[b] * h[b + 1] + x[b + 1] * h[b];
            }
        }
    }
}

// Overlap-save: the inverse transform of each output's sum, whose second half is the block's output.
void Convolver::inverse (size_t level, const std::array<float*, 2>& sums,
                         std::array<std::vector<float>, 2>& outputs) noexcept
{
    const auto size = partitionSizes[level];
    const auto bins = static_cast<size_t> (size + 1) * 2;
    const auto& fft = *m_ffts[level];

    for (size_t o = 0; o < 2; ++o)
    {
        std::fill (m_scratch.begin(), m_scratch.begin() + 4 * static_cast<std::ptrdiff_t> (size), 0.0f);
        std::copy_n (sums[o], bins, m_scratch.begin());
        fft.performRealOnlyInverseTransform (m_scratch.data());
        std::copy_n (m_scratch.begin() + size, size, outputs[o].begin());
    }
}

// Levels 1 and 2: the input block in, and the next block's output from every partition at once.
void Convolver::runLevel (size_t level) noexcept
{
    transform (level);
    const auto bins = static_cast<size_t> (partitionSizes[level] + 1) * 2;

    for (const auto slot : { m_current, m_previous })
    {
        if (slot < 0)
            continue;

        auto& outputs = m_slots[static_cast<size_t> (slot)].output[level];

        if (kernelOf (slot) == nullptr)
        {
            for (auto& channel : outputs)
                std::fill (channel.begin(), channel.end(), 0.0f);

            continue;
        }

        for (auto& accumulator : m_accumulator)
            std::fill_n (accumulator.begin(), bins, 0.0f);

        const std::array<float*, 2> sums { m_accumulator[0].data(), m_accumulator[1].data() };
        addPartitions (slot, level, 0, std::numeric_limits<int>::max(), sums);
        inverse (level, sums, outputs);
    }
}

// Level 3: one of the cycle's steps of partitions, for every program that plays the coming cycle (I2).
void Convolver::spreadStep (int step) noexcept
{
    for (const auto slot : { m_current, m_previous, m_preparing })
    {
        const auto* kernel = kernelOf (slot);

        if (kernel == nullptr)
            continue;

        const auto partitions = kernel->levels[2].partitions;
        const auto first = partitions * step / spreadSteps;
        const auto last = partitions * (step + 1) / spreadSteps;
        auto& spread = m_slots[static_cast<size_t> (slot)].spread;
        addPartitions (slot, 2, first, last, { spread[0].data(), spread[1].data() });
    }
}

void Convolver::cycleBoundary (ProgramExchange& exchange) noexcept
{
    // Level 3's output for the coming cycle, summed over the one that ended.
    for (const auto slot : { m_current, m_previous, m_preparing })
    {
        if (slot < 0)
            continue;

        auto& state = m_slots[static_cast<size_t> (slot)];

        if (kernelOf (slot) != nullptr)
            inverse (2, { state.spread[0].data(), state.spread[1].data() }, state.output[2]);
        else
            for (auto& channel : state.output[2])
                std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& sum : state.spread)
            std::fill (sum.begin(), sum.end(), 0.0f);
    }

    // A fade ends, a prepared program fades in, and a waiting one starts preparing (I6).
    if (m_previous >= 0)
    {
        exchange.retire (m_slots[static_cast<size_t> (m_previous)].program);
        m_slots[static_cast<size_t> (m_previous)].program = nullptr;
        m_previous = -1;
    }

    if (m_preparing >= 0)
    {
        m_previous = m_current;
        m_current = m_preparing;
        m_preparing = -1;
        m_fadePosition = 0;
    }

    if (m_pending != nullptr)
    {
        const auto slot = freeSlot();
        adoptHistory (m_current >= 0 ? m_slots[static_cast<size_t> (m_current)].program : nullptr, m_pending);
        install (m_pending, slot);
        m_preparing = slot;
        m_pending = nullptr;
    }

    transform (2);
}

// A program sent from the message thread: starts at once if nothing plays, else waits for the cycle's end.
void Convolver::takeProgram (ProgramExchange& exchange) noexcept
{
    if (const auto* next = exchange.take())
    {
        if (m_current < 0 && m_preparing < 0 && m_pending == nullptr)
        {
            // Nothing playing: start at once, from silence.
            install (next, 0);
            m_current = 0;
            next->history->fresh = false;
        }
        else
        {
            exchange.retire (m_pending);
            m_pending = next;
        }
    }
}

// Adds one output sample of the program in a slot, at a gain, to a sum: its head's taps plus each level's output.
void Convolver::addOutput (int slot, int position, std::array<float, 2>& sum, float gain) const noexcept
{
    const auto* kernel = kernelOf (slot);

    if (kernel == nullptr)
        return;

    const auto& state = m_slots[static_cast<size_t> (slot)];
    const auto& history = *state.program->history;
    std::array<float, 2> y {};

    for (const auto& route : routesOf (kernel->layout))
    {
        const auto* x = history.headRing[static_cast<size_t> (route.input)].data() + history.headPosition + 1;
        const auto& taps = kernel->head[static_cast<size_t> (route.channel)];
        auto acc = 0.0f;

        for (int t = 0; t < headLength; ++t)
            acc += taps[static_cast<size_t> (t)] * x[t];

        y[static_cast<size_t> (route.output)] += acc;
    }

    for (size_t level = 0; level < state.output.size(); ++level)
    {
        const auto index = static_cast<size_t> (position % partitionSizes[level]);

        for (size_t o = 0; o < 2; ++o)
            y[o] += state.output[level][o][index];
    }

    sum[0] += gain * y[0];
    sum[1] += gain * y[1];
}

// One input sample into the head rings of the histories being fed (each ring holds its window twice).
void Convolver::pushHeads (const std::array<History*, 3>& targets, int count, float left, float right) noexcept
{
    for (int i = 0; i < count; ++i)
    {
        auto& target = *targets[static_cast<size_t> (i)];
        target.headPosition = (target.headPosition + 1) % headLength;
        const std::array<float, 2> x { left, right };

        for (size_t ch = 0; ch < 2; ++ch)
        {
            target.headRing[ch][static_cast<size_t> (target.headPosition)] = x[ch];
            target.headRing[ch][static_cast<size_t> (target.headPosition) + static_cast<size_t> (headLength)] = x[ch];
        }
    }
}

void Convolver::process (ProgramExchange& exchange, std::array<const float*, 2> input, std::array<float*, 2> output,
                         int numSamples, double sampleRate) noexcept
{
    m_rate = sampleRate;

    takeProgram (exchange);

    int done = 0;

    while (done < numSamples)
    {
        const auto count = std::min (numSamples - done, headLength - m_position % headLength);
        std::array<History*, 3> targets {};
        const auto historyCount = histories (targets);

        for (int s = 0; s < count; ++s)
        {
            pushHeads (targets, historyCount, input[0][done + s], input[1][done + s]);

            std::array<float, 2> sum {};
            const auto fade =
                m_previous >= 0 ? static_cast<float> (m_fadePosition + s) / static_cast<float> (cycleLength) : 1.0f;
            addOutput (m_current, m_position + s, sum, fade);
            addOutput (m_previous, m_position + s, sum, 1.0f - fade);
            output[0][done + s] = sum[0];
            output[1][done + s] = sum[1];
        }

        // The levels' windows take the chunk.
        for (int i = 0; i < historyCount; ++i)
        {
            auto& target = *targets[static_cast<size_t> (i)];

            for (size_t level = 0; level < target.levels.size(); ++level)
            {
                const auto size = partitionSizes[level];
                const auto offset =
                    static_cast<std::ptrdiff_t> (size) + static_cast<std::ptrdiff_t> (m_position % size);

                for (size_t ch = 0; ch < 2; ++ch)
                    std::copy_n (input[ch] + done, count, target.levels[level].window[ch].begin() + offset);
            }
        }

        m_position += count;
        m_fadePosition += count;
        done += count;

        if (m_position % headLength != 0)
            continue;

        spreadStep (m_position / headLength - 1);

        if (m_position == cycleLength)
        {
            m_position = 0;
            cycleBoundary (exchange);
        }

        for (size_t level = 0; level < 2; ++level)
            if (m_position % partitionSizes[level] == 0)
                runLevel (level);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Building

juce::AudioBuffer<float> defaultImpulse (double sampleRate)
{
    constexpr double seconds = 2.5;
    constexpr double decaySeconds = 2.0;
    const auto length = static_cast<int> (std::lround (seconds * sampleRate));
    const auto onset = std::max (1, static_cast<int> (std::lround (0.005 * sampleRate)));
    const auto fadeOut = std::max (1, static_cast<int> (std::lround (0.25 * sampleRate)));
    juce::AudioBuffer<float> result (2, length);

    for (int ch = 0; ch < 2; ++ch)
    {
        Noise noise (ch == 0 ? 0x9e3779b9u : 0x7f4a7c15u);
        auto* data = result.getWritePointer (ch);
        auto low = 0.0;

        for (int i = 0; i < length; ++i)
        {
            const auto t = i / sampleRate;
            const auto cutoff = 12000.0 * std::pow (3000.0 / 12000.0, std::min (t / decaySeconds, 1.0));
            const auto pole = std::exp (-2.0 * pi * std::min (cutoff, 0.45 * sampleRate) / sampleRate);
            low = (1.0 - pole) * noise.next() + pole * low;
            auto gain = std::pow (10.0, -3.0 * t / decaySeconds);

            if (i < onset)
                gain *= static_cast<double> (i) / onset;

            if (i >= length - fadeOut)
                gain *= ramp (length - 1 - i, fadeOut);

            data[i] = static_cast<float> (low * gain);
        }
    }

    return result;
}

double estimateDecay (const juce::AudioBuffer<float>& ir, double sampleRate)
{
    const auto length = ir.getNumSamples();

    if (length < 2 || sampleRate <= 0.0)
        return 0.0;

    // The Schroeder backward integral of the energy summed over the channels.
    std::vector<double> integral (static_cast<size_t> (length) + 1, 0.0);

    for (int i = length - 1; i >= 0; --i)
    {
        auto energy = 0.0;

        for (int ch = 0; ch < ir.getNumChannels(); ++ch)
            energy += static_cast<double> (ir.getSample (ch, i)) * ir.getSample (ch, i);

        integral[static_cast<size_t> (i)] = integral[static_cast<size_t> (i) + 1] + energy;
    }

    const auto total = integral[0];

    if (total <= 0.0)
        return 0.0;

    const auto levelAt = [&] (int i)
    { return 10.0 * std::log10 (std::max (integral[static_cast<size_t> (i)] / total, 1.0e-30)); };
    const auto firstBelow = [&] (double db)
    {
        for (int i = 0; i < length; ++i)
            if (levelAt (i) <= db)
                return i;

        return -1;
    };

    const auto from = firstBelow (-5.0);
    auto to = firstBelow (-35.0);

    if (to < 0)
        to = firstBelow (-25.0);

    if (from < 0 || to <= from + 1)
        return 0.0;

    // A least-squares line through the decay curve between the two levels.
    const auto step = std::max (1, (to - from) / 2000);
    auto n = 0.0;
    auto sumX = 0.0;
    auto sumY = 0.0;
    auto sumXx = 0.0;
    auto sumXy = 0.0;

    for (int i = from; i <= to; i += step)
    {
        const auto x = static_cast<double> (i);
        const auto y = levelAt (i);
        n += 1.0;
        sumX += x;
        sumY += y;
        sumXx += x * x;
        sumXy += x * y;
    }

    const auto denominator = n * sumXx - sumX * sumX;

    if (denominator <= 0.0)
        return 0.0;

    const auto slope = (n * sumXy - sumX * sumY) / denominator; // dB per sample

    return slope < 0.0 ? -60.0 / slope / sampleRate : 0.0;
}

Analysis analyse (const juce::AudioBuffer<float>& ir, double sampleRate)
{
    Analysis result;
    const auto length = ir.getNumSamples();
    result.seconds = sampleRate > 0.0 ? length / sampleRate : 0.0;
    result.decaySeconds = estimateDecay (ir, sampleRate);

    if (length == 0)
        return result;

    constexpr int maxPoints = 256;
    const auto span = std::max (1, (length + maxPoints - 1) / maxPoints);

    for (int start = 0; start < length; start += span)
    {
        const auto end = std::min (length, start + span);
        auto loudest = 0.0;

        for (int ch = 0; ch < ir.getNumChannels(); ++ch)
        {
            auto sum = 0.0;

            for (int i = start; i < end; ++i)
                sum += static_cast<double> (ir.getSample (ch, i)) * ir.getSample (ch, i);

            loudest = std::max (loudest, sum / (end - start));
        }

        result.envelope.push_back (static_cast<float> (10.0 * std::log10 (std::max (loudest, 1.0e-14))));
    }

    return result;
}

juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& input, double ratio)
{
    const auto length = input.getNumSamples();

    if (std::abs (ratio - 1.0) < 1.0e-9 || length == 0)
        return input;

    const auto outLength = std::max (1, static_cast<int> (std::lround (length * ratio)));
    juce::AudioBuffer<float> result (input.getNumChannels(), outLength);

    // A Kaiser-windowed sinc, cut off at 0.95 of the lower Nyquist, 32 zero crossings each side, tabulated.
    constexpr double zeroCrossings = 32.0;
    constexpr double beta = 9.0;
    constexpr int phases = 256;
    const auto cutoff = 0.95 * std::min (1.0, ratio);
    const auto halfWidth = zeroCrossings / cutoff; // in input samples
    const auto tableSize = static_cast<size_t> (std::ceil (halfWidth * phases)) + 2;
    std::vector<float> table (tableSize);
    const auto norm = besselI0 (beta);

    for (size_t i = 0; i < tableSize; ++i)
    {
        const auto t = static_cast<double> (i) / phases;
        const auto r = t / halfWidth;

        if (r >= 1.0)
        {
            table[i] = 0.0f;
            continue;
        }

        const auto x = pi * cutoff * t;
        const auto sinc = t < 1.0e-12 ? 1.0 : std::sin (x) / x;
        table[i] = static_cast<float> (cutoff * sinc * besselI0 (beta * std::sqrt (1.0 - r * r)) / norm);
    }

    for (int ch = 0; ch < input.getNumChannels(); ++ch)
    {
        const auto* in = input.getReadPointer (ch);
        auto* out = result.getWritePointer (ch);

        for (int m = 0; m < outLength; ++m)
        {
            const auto centre = m / ratio;
            const auto first = std::max (0, static_cast<int> (std::ceil (centre - halfWidth)));
            const auto last = std::min (length - 1, static_cast<int> (std::floor (centre + halfWidth)));
            auto sum = 0.0;

            for (int i = first; i <= last; ++i)
            {
                const auto position = std::abs (centre - i) * phases;
                const auto index = static_cast<size_t> (position);
                const auto fraction = static_cast<float> (position - static_cast<double> (index));
                const auto weight =
                    index + 1 < tableSize ? table[index] + fraction * (table[index + 1] - table[index]) : 0.0f;
                sum += static_cast<double> (in[i]) * weight;
            }

            out[m] = static_cast<float> (sum);
        }
    }

    return result;
}

Shaped shapeImpulse (const juce::AudioBuffer<float>& file, double fileRate, Layout layout, double deviceRate,
                     const Shape& shape, double decaySeconds)
{
    Shaped result;
    const auto channels = channelsOf (layout);
    const auto length = file.getNumSamples();
    const auto stretch = std::clamp (shape.stretchPercent, 50.0, 200.0) / 100.0;
    const auto ratio = deviceRate * stretch / fileRate;

    if (length == 0 || file.getNumChannels() == 0)
    {
        result.ir.setSize (channels, 1);
        result.ir.clear();
        return result;
    }

    const auto start = std::clamp (static_cast<int> (std::lround (shape.startMs / 1000.0 * fileRate)), 0, length - 1);
    const auto remaining = length - start;
    auto kept =
        std::max (1, static_cast<int> (std::lround (remaining * std::clamp (shape.lengthPercent, 1.0, 100.0) / 100.0)));
    const auto cap = std::max (1, static_cast<int> (std::floor (maxSeconds * fileRate / stretch)));
    result.truncated = kept > cap;
    kept = std::min (kept, cap);

    juce::AudioBuffer<float> ir (channels, kept);

    for (int ch = 0; ch < channels; ++ch)
        ir.copyFrom (ch, 0, file, std::min (ch, file.getNumChannels() - 1), start, kept);

    // A short fade-in after a cut start, and a fade-out where Length or the cap cut the end.
    const auto fadeIn =
        start > 0 ? std::min (kept, std::max (1, static_cast<int> (std::lround (0.001 * fileRate)))) : 0;
    const auto shortened = shape.lengthPercent < 100.0 || result.truncated;
    const auto fadeOut =
        shortened ? std::min (kept, std::max (static_cast<int> (std::lround (0.005 * fileRate)), kept / 10)) : 0;

    // Decay: the slope that makes the IR fall 60 dB in Decay × its decay time (I8).
    const auto decay = std::clamp (shape.decayPercent, 10.0, 100.0) / 100.0;
    const auto slope = decay < 1.0 && decaySeconds > 0.0 ? std::log (1000.0) / decaySeconds * (1.0 / decay - 1.0) : 0.0;

    for (int ch = 0; ch < channels; ++ch)
    {
        auto* data = ir.getWritePointer (ch);

        for (int i = 0; i < kept; ++i)
        {
            auto gain = slope > 0.0 ? static_cast<float> (std::exp (-slope * i / fileRate)) : 1.0f;

            if (i < fadeIn)
                gain *= ramp (i, fadeIn);

            if (i >= kept - fadeOut)
                gain *= ramp (kept - 1 - i, fadeOut);

            data[i] *= gain;
        }

        if (shape.reverse)
            std::reverse (data, data + kept);
    }

    result.ir = resample (ir, ratio);

    // The level (I5): normalised on the whole file, or kept as the file's.
    auto gain = 1.0 / ratio;

    if (shape.normalise)
    {
        const auto energy = outputEnergy (file, layout, length) * ratio;
        gain = energy > 1.0e-12 ? 1.0 / std::sqrt (energy) : 1.0;
    }

    result.ir.applyGain (static_cast<float> (gain));
    return result;
}

std::shared_ptr<const Kernel> makeKernel (const juce::AudioBuffer<float>& ir, Layout layout, double sampleRate)
{
    auto kernel = std::make_shared<Kernel>();
    kernel->layout = layout;
    kernel->sampleRate = sampleRate;
    kernel->length = ir.getNumSamples();
    const auto channels = std::min (channelsOf (layout), ir.getNumChannels());
    const auto length = kernel->length;

    for (int ch = 0; ch < channels; ++ch)
        for (int t = 0; t < std::min (headLength, length); ++t)
            kernel->head[static_cast<size_t> (ch)][static_cast<size_t> (headLength - 1 - t)] = ir.getSample (ch, t);

    std::vector<float> buffer (static_cast<size_t> (2 * largestFft));

    for (size_t level = 0; level < partitionSizes.size(); ++level)
    {
        const auto size = partitionSizes[level];
        const auto end = level + 1 < levelStarts.size() ? std::min (length, levelStarts[level + 1]) : length;
        auto& target = kernel->levels[level];
        target.partitions = partitionsOf (level, length);
        const auto bins = static_cast<size_t> (size + 1) * 2;
        target.spectra.assign (static_cast<size_t> (channelsOf (layout) * target.partitions) * bins, 0.0f);
        const juce::dsp::FFT fft (orderOf (2 * size));

        for (int ch = 0; ch < channels; ++ch)
        {
            for (int k = 0; k < target.partitions; ++k)
            {
                std::fill (buffer.begin(), buffer.end(), 0.0f);
                const auto first = levelStarts[level] + k * size;
                const auto count = std::min (size, end - first);

                if (count > 0)
                    std::copy_n (ir.getReadPointer (ch, first), count, buffer.begin());

                fft.performRealOnlyForwardTransform (buffer.data(), true);
                std::copy_n (
                    buffer.begin(), bins,
                    target.spectra.begin()
                        + static_cast<std::ptrdiff_t> (static_cast<size_t> (ch * target.partitions + k) * bins));
            }
        }
    }

    return kernel;
}

// ---------------------------------------------------------------------------------------------------------
// Inbox and Builder

PanelInfo Inbox::panel() const
{
    const std::scoped_lock lock (m_panelLock);
    return m_panel;
}

Builder& Builder::shared()
{
    static Builder builder;
    return builder;
}

Builder::Builder()
    : m_thread ([this] { run(); })
{
}

Builder::~Builder()
{
    {
        const std::scoped_lock lock (m_lock);
        m_stop = true;
    }

    m_wake.notify_all();
    m_thread.join();
}

void Builder::request (const std::shared_ptr<Inbox>& inbox, Request request)
{
    {
        const std::scoped_lock lock (m_lock);
        inbox->m_request = std::move (request);
        inbox->m_pending.store (true);

        const auto same = [&inbox] (const std::weak_ptr<Inbox>& entry) { return entry.lock() == inbox; };

        if (std::ranges::none_of (m_queue, same))
            m_queue.push_back (inbox);

        if (std::ranges::none_of (m_known, same))
            m_known.push_back (inbox);
    }

    m_wake.notify_all();
}

void Builder::waitIdle()
{
    std::unique_lock lock (m_lock);
    m_idle.wait (lock, [this] { return m_queue.empty() && ! m_busy; });
}

void Builder::run()
{
    for (;;)
    {
        std::shared_ptr<Inbox> inbox;
        std::optional<Request> request;
        std::vector<std::shared_ptr<Inbox>> collect;

        {
            std::unique_lock lock (m_lock);
            m_wake.wait_for (lock, std::chrono::milliseconds (500), [this] { return m_stop || ! m_queue.empty(); });

            if (m_stop)
                return;

            if (m_queue.empty())
            {
                // Now and then, free what the audio threads retired.
                std::erase_if (m_known, [] (const auto& entry) { return entry.expired(); });

                for (const auto& entry : m_known)
                    if (auto alive = entry.lock())
                        collect.push_back (std::move (alive));
            }
            else
            {
                inbox = m_queue.front().lock();
                m_queue.erase (m_queue.begin());

                if (inbox != nullptr)
                {
                    request = std::move (inbox->m_request);
                    inbox->m_request.reset();
                }

                m_busy = request.has_value();
            }
        }

        for (const auto& entry : collect)
            entry->exchange.collectGarbage();

        collect.clear();

        if (request.has_value())
        {
            try
            {
                build (*inbox, *request);
            }
            catch (...) // NOLINT(bugprone-empty-catch): a failed build leaves the last program playing
            {
            }
        }

        {
            const std::scoped_lock lock (m_lock);
            m_busy = false;

            if (inbox != nullptr && ! inbox->m_request.has_value())
                inbox->m_pending.store (false);
        }

        inbox.reset(); // a node gone meanwhile is freed here, not under the lock
        m_idle.notify_all();
    }
}

void Builder::build (Inbox& inbox, const Request& request)
{
    const auto missing = ! request.useDefault && request.source == nullptr;

    // The file, read and analysed once per file (or rate, for the default).
    if (request.useDefault)
    {
        if (! inbox.m_loadedDefault || ! juce::approximatelyEqual (inbox.m_loadedRate, request.sampleRate))
        {
            inbox.m_file = defaultImpulse (request.sampleRate);
            inbox.m_fileRate = request.sampleRate;
            inbox.m_layout = Layout::stereo;
            inbox.m_analysis = analyse (inbox.m_file, inbox.m_fileRate);
            inbox.m_loadedDefault = true;
            inbox.m_loadedSource = nullptr;
            inbox.m_loadedRate = request.sampleRate;
        }
    }
    else if (request.source != nullptr && request.source.get() != inbox.m_loadedSource)
    {
        const auto& source = *request.source;
        // Enough for the longest IR at the smallest Stretch, after the latest Start.
        const auto frames = static_cast<int> (std::min<juce::int64> (
            source.frames(), static_cast<juce::int64> ((2.0 * maxSeconds + 1.0) * source.sampleRate())));
        inbox.m_file = source.readAllChannels (0, frames);
        inbox.m_fileRate = source.sampleRate();
        inbox.m_layout = layoutFor (source.channels());
        inbox.m_analysis = analyse (inbox.m_file, inbox.m_fileRate);
        inbox.m_loadedSource = request.source.get();
        inbox.m_loadedDefault = false;
    }

    auto program = std::make_shared<Program>();
    auto truncated = false;

    if (! missing && inbox.m_file.getNumSamples() > 0)
    {
        auto shaped = shapeImpulse (inbox.m_file, inbox.m_fileRate, inbox.m_layout, request.sampleRate, request.shape,
                                    inbox.m_analysis.decaySeconds);
        truncated = shaped.truncated;
        program->kernel = makeKernel (shaped.ir, inbox.m_layout, request.sampleRate);
    }

    // The history grows with the IR, with some room so small increases don't need a new one (I6).
    const auto needed = program->kernel != nullptr ? level3Partitions (program->kernel->length) : 0;

    if (inbox.m_history == nullptr || inbox.m_history->levels[2].capacity < needed)
        inbox.m_history = std::make_shared<History> ((needed + needed / 4 + 15) / 16 * 16);

    program->history = inbox.m_history;
    inbox.exchange.publish (std::move (program));

    const std::scoped_lock lock (inbox.m_panelLock);
    inbox.m_panel.analysis = missing ? Analysis {} : inbox.m_analysis;
    inbox.m_panel.missing = missing;
    inbox.m_panel.truncated = truncated;
    inbox.m_panel.ready = true;
}

} // namespace dawg::convolution
