#include "dawg/NodeRegistry.h"

#include "dawg/BuiltinNodes.h"
#include "dawg/VarUtils.h"

namespace dawg
{

void NodeRegistry::add (const NodeDescriptor& descriptor, Factory factory)
{
    m_entries[descriptor.type] = { &descriptor, std::move (factory) };
}

const NodeDescriptor* NodeRegistry::find (const juce::String& type) const
{
    const auto it = m_entries.find (type);
    return it != m_entries.end() ? it->second.descriptor : nullptr;
}

std::unique_ptr<Node> NodeRegistry::create (const juce::String& type) const
{
    const auto it = m_entries.find (type);
    return it != m_entries.end() ? it->second.factory() : nullptr;
}

std::vector<const NodeDescriptor*> NodeRegistry::descriptors() const
{
    std::vector<const NodeDescriptor*> result;
    result.reserve (m_entries.size());

    for (const auto& [type, entry] : m_entries)
        result.push_back (entry.descriptor);

    return result;
}

namespace
{
[[nodiscard]] const char* categoryName (NodeCategory category) noexcept
{
    switch (category)
    {
        case NodeCategory::io:
            return "io";
        case NodeCategory::instrument:
            return "instrument";
        case NodeCategory::midiEffect:
            return "midiEffect";
        case NodeCategory::utility:
            return "utility";
        case NodeCategory::effect:
            break;
    }

    return "effect";
}

[[nodiscard]] const char* kindName (ParamKind kind) noexcept
{
    switch (kind)
    {
        case ParamKind::integer:
            return "integer";
        case ParamKind::choice:
            return "choice";
        case ParamKind::toggle:
            return "toggle";
        case ParamKind::continuous:
            break;
    }

    return "continuous";
}

[[nodiscard]] const char* polarityName (ModulationPolarity polarity) noexcept
{
    switch (polarity)
    {
        case ModulationPolarity::positive:
            return "positive";
        case ModulationPolarity::negative:
            return "negative";
        case ModulationPolarity::bipolar:
            break;
    }

    return "bipolar";
}

// How the UI names a ModulationScale.
[[nodiscard]] const char* scaleName (ModulationScale scale) noexcept
{
    switch (scale)
    {
        case ModulationScale::octaves:
            return "octaves";
        case ModulationScale::travel:
            return "travel";
        case ModulationScale::linear:
            break;
    }

    return "linear";
}

[[nodiscard]] juce::var portsVar (std::span<const PortInfo> ports)
{
    juce::Array<juce::var> result;

    for (const auto& port : ports)
        result.add (makeObject (
            { { "type", portTypeName (port.type) }, { "name", port.name }, { "channels", port.channels } }));

    return result;
}
} // namespace

juce::var toVar (const NodeDescriptor& descriptor)
{
    juce::Array<juce::var> params;

    for (const auto& p : descriptor.params)
    {
        juce::Array<juce::var> choices;

        for (const auto* choice : p.choices)
            choices.add (choice);

        params.add (makeObject ({ { "id", p.id },
                                  { "name", p.name },
                                  { "min", p.minValue },
                                  { "max", p.maxValue },
                                  { "default", p.defaultValue },
                                  { "kind", kindName (p.kind) },
                                  { "unit", juce::String::fromUTF8 (p.unit) }, // UTF-8, such as the degree sign
                                  { "mid", p.midValue },
                                  { "modulationSpan", p.modulationRange() },
                                  { "modulationScale", scaleName (p.modulationScale()) },
                                  { "modulationPolarity", polarityName (p.modulationPolarity) },
                                  { "modulatable", p.acceptsModulation() },
                                  { "automatable", p.acceptsAutomation() },
                                  { "choices", choices } }));
    }

    return makeObject ({ { "type", descriptor.type },
                         { "name", descriptor.displayName },
                         { "category", categoryName (descriptor.category) },
                         { "userEditable", descriptor.userEditable },
                         { "inputs", portsVar (descriptor.inputs) },
                         { "outputs", portsVar (descriptor.outputs) },
                         { "params", params } });
}

const NodeRegistry& NodeRegistry::builtin()
{
    static const NodeRegistry registry = []
    {
        NodeRegistry r;
        registerBuiltinNodes (r);
        return r;
    }();

    return registry;
}

} // namespace dawg
