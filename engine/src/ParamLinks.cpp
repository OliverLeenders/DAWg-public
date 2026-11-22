#include "dawg/ParamLinks.h"

#include <algorithm>
#include <cmath>
#include <string_view>

// The arithmetic of parameter links (docs/design/parameter-linking.md, L2, L4).

namespace dawg
{

namespace
{
[[nodiscard]] bool isFrequency (const ParamInfo& info) noexcept
{
    return info.modulationScale() == ModulationScale::octaves;
}

// A value in the domain's terms.
[[nodiscard]] double toDomain (LinkDomain domain, const ParamInfo* info, double value) noexcept
{
    switch (domain)
    {
        case LinkDomain::units:
            return value;
        case LinkDomain::octaves:
            return std::log2 (std::max (value, static_cast<double> (info->minValue)));
        case LinkDomain::normalised:
            break;
    }

    return info != nullptr ? paramToNormalised (*info, value) : std::clamp (value, 0.0, 1.0);
}

// A value back from the domain's terms, clamped to the parameter's range.
[[nodiscard]] float fromDomain (LinkDomain domain, const ParamInfo* info, double value) noexcept
{
    if (info == nullptr)
        return static_cast<float> (std::clamp (value, 0.0, 1.0));

    auto result = value;

    switch (domain)
    {
        case LinkDomain::units:
            break;
        case LinkDomain::octaves:
            result = std::exp2 (value);
            break;
        case LinkDomain::normalised:
            return paramFromNormalised (*info, value);
    }

    if (info->kind != ParamKind::continuous)
        result = std::round (result);

    return static_cast<float> (
        std::clamp (result, static_cast<double> (info->minValue), static_cast<double> (info->maxValue)));
}

// The value inverted and scaled mappings turn around: the one set, or the middle of the source's range.
[[nodiscard]] double centreOf (const LinkMapping& mapping, LinkDomain domain, const ParamInfo* source) noexcept
{
    if (mapping.centre.has_value())
        return *mapping.centre;

    if (source == nullptr || domain == LinkDomain::normalised)
        return 0.5;

    return (toDomain (domain, source, source->minValue) + toDomain (domain, source, source->maxValue)) / 2.0;
}

// A range's shape at t (0..1): its curve, or a straight line.
[[nodiscard]] double shapeAt (const Curve& curve, double t) noexcept
{
    if (curve.empty())
        return t;

    return std::clamp (evaluateCurve (curve, t * static_cast<double> (modulationShapeTicks), t), 0.0, 1.0);
}

// Whether a range's curve only rises or only falls, so it can be undone.
[[nodiscard]] bool isMonotonic (const Curve& curve) noexcept
{
    auto rises = false;
    auto falls = false;

    for (size_t i = 1; i < curve.size(); ++i)
    {
        rises = rises || curve[i].value > curve[i - 1].value;
        falls = falls || curve[i].value < curve[i - 1].value;
    }

    return ! (rises && falls);
}
} // namespace

LinkDomain linkDomain (const ParamInfo* source, const ParamInfo* target) noexcept
{
    if (source == nullptr || target == nullptr || std::string_view (source->unit) != std::string_view (target->unit))
        return LinkDomain::normalised;

    if (isFrequency (*source) && isFrequency (*target))
        return LinkDomain::octaves;

    // A frequency and a parameter with "Hz" that starts at 0 don't share a scale.
    return isFrequency (*source) == isFrequency (*target) ? LinkDomain::units : LinkDomain::normalised;
}

float mapLinkValue (const LinkMapping& mapping, const ParamInfo* source, const ParamInfo* target, float value) noexcept
{
    if (mapping.kind == LinkMappingKind::range)
    {
        const auto x = toDomain (LinkDomain::normalised, source, value);
        const auto width = mapping.inMax - mapping.inMin;
        const auto t = std::abs (width) > 1.0e-9 ? std::clamp ((x - mapping.inMin) / width, 0.0, 1.0)
                                                 : (x >= mapping.inMin ? 1.0 : 0.0);
        const auto y = mapping.outMin + (mapping.outMax - mapping.outMin) * shapeAt (mapping.curve, t);
        return fromDomain (LinkDomain::normalised, target, y);
    }

    const auto domain = linkDomain (source, target);
    const auto x = toDomain (domain, source, value);
    auto y = x;

    switch (mapping.kind)
    {
        case LinkMappingKind::identity:
        case LinkMappingKind::range:
            break;
        case LinkMappingKind::inverted:
            y = 2.0 * centreOf (mapping, domain, source) - x;
            break;
        case LinkMappingKind::offset:
            y = x + mapping.amount;
            break;
        case LinkMappingKind::scaled:
        {
            const auto c = centreOf (mapping, domain, source);
            y = c + mapping.amount * (x - c);
            break;
        }
    }

    return fromDomain (domain, target, y);
}

bool isInvertible (const LinkMapping& mapping) noexcept
{
    switch (mapping.kind)
    {
        case LinkMappingKind::identity:
        case LinkMappingKind::inverted:
        case LinkMappingKind::offset:
            return true;
        case LinkMappingKind::scaled:
            return std::abs (mapping.amount) > 1.0e-9;
        case LinkMappingKind::range:
            return std::abs (mapping.inMax - mapping.inMin) > 1.0e-9
                   && std::abs (mapping.outMax - mapping.outMin) > 1.0e-9 && isMonotonic (mapping.curve);
    }

    return false;
}

std::optional<float> invertLinkValue (const LinkMapping& mapping, const ParamInfo* source, const ParamInfo* target,
                                      float value) noexcept
{
    if (! isInvertible (mapping))
        return std::nullopt;

    if (mapping.kind == LinkMappingKind::range)
    {
        const auto y = toDomain (LinkDomain::normalised, target, value);
        const auto s = std::clamp ((y - mapping.outMin) / (mapping.outMax - mapping.outMin), 0.0, 1.0);
        auto t = s;

        // The curve rises or falls throughout: find where it reaches s by halving.
        if (! mapping.curve.empty())
        {
            const auto rising = shapeAt (mapping.curve, 1.0) >= shapeAt (mapping.curve, 0.0);
            auto low = 0.0;
            auto high = 1.0;

            for (int i = 0; i < 40; ++i)
            {
                const auto middle = (low + high) / 2.0;

                if ((shapeAt (mapping.curve, middle) < s) == rising)
                    low = middle;
                else
                    high = middle;
            }

            t = (low + high) / 2.0;
        }

        return fromDomain (LinkDomain::normalised, source, mapping.inMin + t * (mapping.inMax - mapping.inMin));
    }

    const auto domain = linkDomain (source, target);
    const auto y = toDomain (domain, target, value);
    auto x = y;

    switch (mapping.kind)
    {
        case LinkMappingKind::identity:
        case LinkMappingKind::range:
            break;
        case LinkMappingKind::inverted:
            x = 2.0 * centreOf (mapping, domain, source) - y;
            break;
        case LinkMappingKind::offset:
            x = y - mapping.amount;
            break;
        case LinkMappingKind::scaled:
        {
            const auto c = centreOf (mapping, domain, source);
            x = c + (y - c) / mapping.amount;
            break;
        }
    }

    return fromDomain (domain, source, x);
}

} // namespace dawg
