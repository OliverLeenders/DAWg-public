#include "dawg/JsonPatch.h"

#include "dawg/VarUtils.h"

#include <algorithm>

namespace dawg
{

namespace
{
[[nodiscard]] juce::var makeOp (const char* op, const juce::String& path)
{
    return makeObject ({ { "op", op }, { "path", path } });
}

[[nodiscard]] juce::var makeOp (const char* op, const juce::String& path, const juce::var& value)
{
    return makeObject ({ { "op", op }, { "path", path }, { "value", value } });
}

[[nodiscard]] bool deepEquals (const juce::var& a, const juce::var& b);

[[nodiscard]] bool objectsEqual (const juce::DynamicObject& a, const juce::DynamicObject& b)
{
    const auto& propsA = a.getProperties();
    const auto& propsB = b.getProperties();

    if (propsA.size() != propsB.size())
        return false;

    return std::ranges::all_of (propsA, [&propsB] (const juce::NamedValueSet::NamedValue& prop)
                                { return propsB.contains (prop.name) && deepEquals (prop.value, propsB[prop.name]); });
}

[[nodiscard]] bool arraysEqual (const juce::Array<juce::var>& a, const juce::Array<juce::var>& b)
{
    if (a.size() != b.size())
        return false;

    for (int i = 0; i < a.size(); ++i)
        if (! deepEquals (a.getReference (i), b.getReference (i)))
            return false;

    return true;
}

// juce::var::operator== compares objects by identity and coerces between types, so compare deeply here.
bool deepEquals (const juce::var& a, const juce::var& b)
{
    const auto* objA = a.getDynamicObject();
    const auto* objB = b.getDynamicObject();

    if (objA != nullptr || objB != nullptr)
        return objA != nullptr && objB != nullptr && objectsEqual (*objA, *objB);

    const auto* arrA = a.getArray();
    const auto* arrB = b.getArray();

    if (arrA != nullptr || arrB != nullptr)
        return arrA != nullptr && arrB != nullptr && arraysEqual (*arrA, *arrB);

    return a.isString() == b.isString() && a == b;
}

[[nodiscard]] juce::String childPath (const juce::String& parent, const juce::String& key)
{
    return parent + "/" + escapePointerSegment (key);
}

void diffInto (juce::Array<juce::var>& ops, const juce::String& path, const juce::var& before, const juce::var& after)
{
    auto* objBefore = before.getDynamicObject();
    auto* objAfter = after.getDynamicObject();

    if (objBefore != nullptr && objAfter != nullptr)
    {
        for (const auto& prop : objBefore->getProperties())
            if (! objAfter->hasProperty (prop.name))
                ops.add (makeOp ("remove", childPath (path, prop.name.toString())));

        for (const auto& prop : objAfter->getProperties())
        {
            const auto propPath = childPath (path, prop.name.toString());

            if (objBefore->hasProperty (prop.name))
                diffInto (ops, propPath, objBefore->getProperty (prop.name), prop.value);
            else
                ops.add (makeOp ("add", propPath, prop.value));
        }

        return;
    }

    const auto* arrBefore = before.getArray();
    const auto* arrAfter = after.getArray();

    if (arrBefore != nullptr && arrAfter != nullptr && arrBefore->size() == arrAfter->size())
    {
        for (int i = 0; i < arrAfter->size(); ++i)
            diffInto (ops, path + "/" + juce::String (i), arrBefore->getReference (i), arrAfter->getReference (i));

        return;
    }

    if (! deepEquals (before, after))
        ops.add (makeOp ("replace", path, after));
}
} // namespace

juce::String escapePointerSegment (const juce::String& key)
{
    return key.replace ("~", "~0").replace ("/", "~1");
}

juce::Array<juce::var> diff (const juce::var& before, const juce::var& after)
{
    juce::Array<juce::var> ops;
    diffInto (ops, {}, before, after);
    return ops;
}

void appendDiff (juce::Array<juce::var>& ops, const juce::String& path, const juce::var& before, const juce::var& after)
{
    diffInto (ops, path, before, after);
}

} // namespace dawg
