#pragma once

#include <juce_core/juce_core.h>

#include <initializer_list>
#include <utility>

namespace dawg
{

/**
    Creates an empty JSON object to fill with setProperty().

    @return  A new reference-counted DynamicObject.
*/
[[nodiscard]] inline juce::DynamicObject::Ptr newObject()
{
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): DynamicObject is reference-counted by Ptr and var.
    return juce::DynamicObject::Ptr (new juce::DynamicObject());
}

/**
    Builds a JSON object from key/value pairs.

    Wraps the one place where juce::var takes ownership of a raw DynamicObject pointer, so the rest of the
    code never calls new directly.

    @param properties  The properties to set, in order.
    @return            A var holding a new DynamicObject.
*/
[[nodiscard]] inline juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    const auto object = newObject();
    for (const auto& [key, value] : properties)
        object->setProperty (key, value);

    return juce::var (object.get());
}

} // namespace dawg
