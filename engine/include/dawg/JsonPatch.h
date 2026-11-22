#pragma once

#include <juce_core/juce_core.h>

namespace dawg
{

/**
    Computes the JSON-Patch style operations (an RFC 6902 subset) that turn one value into another.

    Emitted ops have the shape { op: "add" | "remove" | "replace", path, value? }. Objects are diffed key by
    key. Arrays of equal length are diffed element by element; arrays whose length changed are replaced
    wholesale, which keeps the algorithm simple and the patches correct, if not always minimal.

    @param before  The old value.
    @param after   The new value.
    @return        The operations, in the order they must be applied.
*/
[[nodiscard]] juce::Array<juce::var> diff (const juce::var& before, const juce::var& after);

/**
    Like diff(), for two values found at `path` in a larger document: appends their operations to `ops`, with
    paths below `path`.

    @param ops     The operations so far.
    @param path    The JSON pointer of both values ("" for the root).
    @param before  The old value.
    @param after   The new value.
*/
void appendDiff (juce::Array<juce::var>& ops, const juce::String& path, const juce::var& before,
                 const juce::var& after);

/**
    Escapes a key for use as a JSON pointer segment.

    @param key  The raw object key.
    @return     The key with "~" replaced by "~0" and "/" by "~1".
*/
[[nodiscard]] juce::String escapePointerSegment (const juce::String& key);

} // namespace dawg
