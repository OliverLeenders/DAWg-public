#include "Node.h"

#include <algorithm>
#include <cassert>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
 #include <xmmintrin.h>
#endif

namespace dawg::reverb
{

namespace
{
// Flushes denormals to zero for the lifetime of the object (DAWg: juce::ScopedNoDenormals).
class ScopedNoDenormals final
{
public:
    ScopedNoDenormals() noexcept
    {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        m_saved = _mm_getcsr();
        _mm_setcsr (m_saved | 0x8040); // flush-to-zero and denormals-are-zero
#endif
    }

    ~ScopedNoDenormals()
    {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        _mm_setcsr (m_saved);
#endif
    }

    ScopedNoDenormals (const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator= (const ScopedNoDenormals&) = delete;
    ScopedNoDenormals (ScopedNoDenormals&&) = delete;
    ScopedNoDenormals& operator= (ScopedNoDenormals&&) = delete;

private:
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
    unsigned int m_saved = 0;
#endif
};
} // namespace

int NodeDescriptor::indexOfParam (std::string_view id) const noexcept
{
    for (size_t i = 0; i < params.size(); ++i)
        if (id == params[i].id)
            return static_cast<int> (i);

    return -1;
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
    assert (index < m_infos.size());
    m_values[index].store (std::clamp (value, m_infos[index].minValue, m_infos[index].maxValue));
}

Node::Node (const NodeDescriptor& descriptor)
    : m_descriptor (descriptor),
      m_params (descriptor.params)
{
}

void EffectNode::process (const ProcessContext& context) noexcept
{
    const ScopedNoDenormals noDenormals;

    if (! m_bypassed.load())
    {
        render (context);
        return;
    }

    const auto& in = context.audioInputs[0];
    const auto& out = context.audioOutputs[0];

    for (size_t ch = 0; ch < 2; ++ch)
        std::copy_n (in[ch], context.numSamples, out[ch]);
}

} // namespace dawg::reverb
