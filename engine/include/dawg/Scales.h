#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>

namespace dawg
{

/**
    A scale as the pitch classes it holds above its root (docs/design/midi-effects.md, ME12). Scale Lock uses these; the
    piano roll's highlighting can use the same table (ui/src/music/scales.ts holds a copy, tested against this one).
*/
struct Scale
{
    /**
        Display name.
    */
    const char* name = "";

    /**
        Bit i is set when the pitch class i semitones above the root is in the scale. Bit 0 is always set.
    */
    std::uint16_t mask = 1;
};

/**
    Builds a scale mask from semitones above the root.

    @param degrees  The semitones, 0..11.
    @return         The mask.
*/
[[nodiscard]] constexpr std::uint16_t scaleMask (std::initializer_list<int> degrees) noexcept
{
    std::uint16_t mask = 0;

    for (const auto degree : degrees)
        mask = static_cast<std::uint16_t> (mask | (1u << static_cast<unsigned> (degree)));

    return mask;
}

/**
    The scales, in the order of Scale Lock's Scale choice.
*/
inline constexpr std::array<Scale, 15> scales { {
    { "Major", scaleMask ({ 0, 2, 4, 5, 7, 9, 11 }) },
    { "Natural minor", scaleMask ({ 0, 2, 3, 5, 7, 8, 10 }) },
    { "Harmonic minor", scaleMask ({ 0, 2, 3, 5, 7, 8, 11 }) },
    { "Melodic minor", scaleMask ({ 0, 2, 3, 5, 7, 9, 11 }) },
    { "Dorian", scaleMask ({ 0, 2, 3, 5, 7, 9, 10 }) },
    { "Phrygian", scaleMask ({ 0, 1, 3, 5, 7, 8, 10 }) },
    { "Lydian", scaleMask ({ 0, 2, 4, 6, 7, 9, 11 }) },
    { "Mixolydian", scaleMask ({ 0, 2, 4, 5, 7, 9, 10 }) },
    { "Locrian", scaleMask ({ 0, 1, 3, 5, 6, 8, 10 }) },
    { "Major pentatonic", scaleMask ({ 0, 2, 4, 7, 9 }) },
    { "Minor pentatonic", scaleMask ({ 0, 3, 5, 7, 10 }) },
    { "Blues", scaleMask ({ 0, 3, 5, 6, 7, 10 }) },
    { "Whole tone", scaleMask ({ 0, 2, 4, 6, 8, 10 }) },
    { "Diminished", scaleMask ({ 0, 1, 3, 4, 6, 7, 9, 10 }) },
    { "Chromatic", scaleMask ({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }) },
} };

/**
    Returns the pitch class of a key relative to a root, 0..11.

    @param key   A MIDI key.
    @param root  The root's pitch class, 0 (C) to 11 (B).
*/
[[nodiscard]] constexpr int degreeOf (int key, int root) noexcept
{
    return (((key - root) % 12) + 12) % 12;
}

/**
    Returns whether a key is in a scale.

    @param key   A MIDI key.
    @param root  The root's pitch class, 0 (C) to 11 (B).
    @param mask  The scale's mask.
*/
[[nodiscard]] constexpr bool inScale (int key, int root, std::uint16_t mask) noexcept
{
    return ((static_cast<unsigned> (mask) >> static_cast<unsigned> (degreeOf (key, root))) & 1u) != 0;
}

/**
    Which way a key outside the scale moves.
*/
enum class ScaleDirection
{
    nearest, ///< To the nearest key in the scale; ties go down.
    up,      ///< To the next key up in the scale.
    down,    ///< To the next key down in the scale.
};

/**
    Moves a key into a scale.

    @param key        A MIDI key.
    @param root       The root's pitch class, 0..11.
    @param mask       The scale's mask (bit 0 set, so some key is always found).
    @param direction  Which way to look.
    @return           The key, if in the scale, or the key it moves to.
*/
[[nodiscard]] constexpr int lockToScale (int key, int root, std::uint16_t mask, ScaleDirection direction) noexcept
{
    if (inScale (key, root, mask))
        return key;

    for (int distance = 1; distance < 12; ++distance)
    {
        if (direction != ScaleDirection::up && inScale (key - distance, root, mask))
            return key - distance;

        if (direction != ScaleDirection::down && inScale (key + distance, root, mask))
            return key + distance;
    }

    return key;
}

} // namespace dawg
