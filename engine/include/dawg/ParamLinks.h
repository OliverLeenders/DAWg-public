#pragma once

#include "dawg/Model.h"
#include "dawg/Node.h"

#include <optional>

namespace dawg
{

/**
    The terms a link's mapping works in (docs/design/parameter-linking.md, L2).
*/
enum class LinkDomain
{
    units,      ///< Both ends are built-in parameters with the same unit: the values themselves.
    octaves,    ///< Both ends are frequencies: log2 of the values, so offsets are in octaves.
    normalised, ///< Anything else, plugins included: 0..1 across each parameter's range, along its knob.
};

/**
    Returns the terms a link between two parameters works in.

    @param source  The source's description, or nullptr for a plugin parameter.
    @param target  The target's description, or nullptr for a plugin parameter.
*/
[[nodiscard]] LinkDomain linkDomain (const ParamInfo* source, const ParamInfo* target) noexcept;

/**
    Maps a source value to its target's (docs/design/parameter-linking.md, L2). Real-time safe.

    @param mapping  The mapping.
    @param source   The source's description, or nullptr for a plugin parameter.
    @param target   The target's description, or nullptr for a plugin parameter.
    @param value    The source's value: in its units, or normalised for a plugin.
    @return         The target's value, in its units (clamped to its range, whole for an integer, choice or toggle), or
                    normalised for a plugin.
*/
[[nodiscard]] float mapLinkValue (const LinkMapping& mapping, const ParamInfo* source, const ParamInfo* target,
                                  float value) noexcept;

/**
    Returns whether a mapping can be undone: every kind but a scale by 0, a range of no width, or a range along a curve
    that turns back (docs/design/parameter-linking.md, L4).

    @param mapping  The mapping.
*/
[[nodiscard]] bool isInvertible (const LinkMapping& mapping) noexcept;

/**
    The source value a target value comes from: the inverse of mapLinkValue(), for an edit of a target that goes to its
    source (L4).

    @param mapping  The mapping.
    @param source   The source's description, or nullptr for a plugin parameter.
    @param target   The target's description, or nullptr for a plugin parameter.
    @param value    The target's value, as mapLinkValue() returns it.
    @return         The source's value (clamped to its range), or nothing for a mapping that can't be undone.
*/
[[nodiscard]] std::optional<float> invertLinkValue (const LinkMapping& mapping, const ParamInfo* source,
                                                    const ParamInfo* target, float value) noexcept;

} // namespace dawg
