#include "dawg/Node.h"

#include <algorithm>
#include <cmath>

namespace dawg
{

namespace
{
// The base of the exponential curve that puts midValue at the middle (1 for a linear range).
[[nodiscard]] double curveBaseOf (const ParamInfo& info) noexcept
{
    const double min = info.minValue;
    const double max = info.maxValue;
    const double mid = info.midValue;

    if (mid <= min || mid >= max)
        return 1.0;

    return std::pow ((max - mid) / (mid - min), 2.0);
}

[[nodiscard]] bool isLinear (double base) noexcept
{
    return std::abs (base - 1.0) < 1.0e-6;
}
} // namespace

float paramFromNormalised (const ParamInfo& info, double t) noexcept
{
    const auto clamped = std::clamp (t, 0.0, 1.0);
    const auto r = curveBaseOf (info);
    const auto shaped = isLinear (r) ? clamped : (std::pow (r, clamped) - 1.0) / (r - 1.0);
    const auto value = info.minValue + (info.maxValue - info.minValue) * shaped;
    return static_cast<float> (info.kind == ParamKind::continuous ? value : std::round (value));
}

float modulatedParamValue (const ParamInfo& info, float base, float amount) noexcept
{
    auto value = base;

    switch (info.modulationScale())
    {
        case ModulationScale::octaves:
            value = std::max (base, info.minValue) * std::exp2 (amount * info.modulationRange());
            break;
        case ModulationScale::travel:
            return paramFromNormalised (info, paramToNormalised (info, base) + static_cast<double> (amount));
        case ModulationScale::linear:
            value = base + amount * info.modulationRange();
            break;
    }

    if (info.kind == ParamKind::integer)
        value = std::round (value);

    return std::clamp (value, info.minValue, info.maxValue);
}

double paramToNormalised (const ParamInfo& info, double value) noexcept
{
    const auto range = static_cast<double> (info.maxValue - info.minValue);
    const auto linear = range > 0.0 ? std::clamp ((value - info.minValue) / range, 0.0, 1.0) : 0.0;
    const auto r = curveBaseOf (info);
    return isLinear (r) ? linear : std::log (1.0 + linear * (r - 1.0)) / std::log (r);
}

AnalyserTap::AnalyserTap()
    : m_left (std::make_unique<std::atomic<float>[]> (capacity)), // NOLINT(modernize-avoid-c-arrays)
      m_right (std::make_unique<std::atomic<float>[]> (capacity)) // NOLINT(modernize-avoid-c-arrays)
{
    for (size_t i = 0; i < capacity; ++i)
    {
        m_left[i].store (0.0f, std::memory_order_relaxed);
        m_right[i].store (0.0f, std::memory_order_relaxed);
    }
}

void AnalyserTap::push (const AudioOutput& output, int numSamples) noexcept
{
    auto position = m_written.load (std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i, ++position)
    {
        m_left[position & (capacity - 1)].store (output[0][i], std::memory_order_relaxed);
        m_right[position & (capacity - 1)].store (output[1][i], std::memory_order_relaxed);
    }

    m_written.store (position, std::memory_order_release);
}

void AnalyserTap::read (std::span<float> left, std::span<float> right) const noexcept
{
    const auto count = std::min ({ left.size(), right.size(), capacity });
    const auto end = m_written.load (std::memory_order_acquire);

    for (size_t i = 0; i < count; ++i)
    {
        const auto position = (end - count + i) & (capacity - 1);
        left[i] = m_left[position].load (std::memory_order_relaxed);
        right[i] = m_right[position].load (std::memory_order_relaxed);
    }
}

void Node::setTapEnabled (bool enabled)
{
    if (enabled && m_tap == nullptr)
        m_tap = std::make_unique<AnalyserTap>();

    m_activeTap.store (enabled ? m_tap.get() : nullptr);
}

int NodeDescriptor::indexOfParam (const juce::String& id) const noexcept
{
    for (size_t i = 0; i < params.size(); ++i)
        if (id == params[i].id)
            return static_cast<int> (i);

    return -1;
}

bool EventBuffer::add (const NoteEvent& event) noexcept
{
    if (m_size == capacity)
    {
        ++m_dropped;
        return false;
    }

    // Insertion keeps the buffer sorted; blocks hold few events, so this is cheaper than sorting later.
    auto position = m_size;

    while (position > 0 && m_events[position - 1].sampleOffset > event.sampleOffset)
    {
        m_events[position] = m_events[position - 1];
        --position;
    }

    m_events[position] = event;
    ++m_size;
    return true;
}

ParamBlock::ParamBlock (std::span<const ParamInfo> params)
    : m_infos (params),
      m_values (std::make_unique<std::atomic<float>[]> (params.size())) // NOLINT(modernize-avoid-c-arrays)
{
    for (size_t i = 0; i < params.size(); ++i)
        m_values[i].store (params[i].defaultValue);
}

void ParamBlock::set (size_t index, float value) noexcept
{
    jassert (index < m_infos.size());
    m_values[index].store (std::clamp (value, m_infos[index].minValue, m_infos[index].maxValue));
}

namespace
{
std::atomic<juce::uint64> nextNodeSerial { 1 };
} // namespace

Node::Node (const NodeDescriptor& descriptor)
    : m_descriptor (descriptor),
      m_serial (nextNodeSerial.fetch_add (1)),
      m_params (descriptor.params)
{
}

} // namespace dawg
