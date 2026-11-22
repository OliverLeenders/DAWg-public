#pragma once

#include "dawg/Node.h"

#include <array>
#include <span>
#include <vector>

namespace dawg
{

class NodeRegistry;

/**
    Registers the MIDI effects: Arpeggiator, Chord, Scale Lock and Note Repeat (docs/design/midi-effects.md). Called by
    registerBuiltinNodes().

    @param registry  The registry to add them to.
*/
void registerMidiEffects (NodeRegistry& registry);

/**
    What the MIDI effects share, for them and their tests (docs/design/midi-effects.md).
*/
namespace midifx
{

/**
    The id of a note a MIDI effect makes (ME4): negative, as live notes' are, so instruments never look for it in the
    sequence, and far below the live ids, which count down from -1.

    @param serial   The effect node's Node::serial().
    @param counter  The effect's count of notes made so far.
    @return         The id.
*/
[[nodiscard]] constexpr ObjectId derivedNoteId (juce::uint64 serial, juce::uint32 counter) noexcept
{
    constexpr auto base = ObjectId { 1 } << 52;
    return -(base + static_cast<ObjectId> (serial & 0xffffffffu) * (ObjectId { 1 } << 20)
             + static_cast<ObjectId> (counter & 0xfffffu));
}

/**
    A chord shape: semitones above the played note, the played note (0) first.
*/
struct ChordShape
{
    /**
        Display name.
    */
    const char* name = "";

    /**
        The notes; count of them are used.
    */
    std::array<int, 6> notes {};

    /**
        How many notes.
    */
    int count = 1;
};

/**
    The Chord's shapes, in the order of its Shape choice, without Custom, which comes last there (ME11).
*/
inline constexpr std::array<ChordShape, 16> chordShapes { {
    { "Major", { 0, 4, 7 }, 3 },
    { "Minor", { 0, 3, 7 }, 3 },
    { "5", { 0, 7 }, 2 },
    { "sus2", { 0, 2, 7 }, 3 },
    { "sus4", { 0, 5, 7 }, 3 },
    { "6", { 0, 4, 7, 9 }, 4 },
    { "m6", { 0, 3, 7, 9 }, 4 },
    { "7", { 0, 4, 7, 10 }, 4 },
    { "maj7", { 0, 4, 7, 11 }, 4 },
    { "m7", { 0, 3, 7, 10 }, 4 },
    { "m7\xe2\x99\xad"
      "5",
      { 0, 3, 6, 10 },
      4 },
    { "dim", { 0, 3, 6 }, 3 },
    { "dim7", { 0, 3, 6, 9 }, 4 },
    { "aug", { 0, 4, 8 }, 3 },
    { "add9", { 0, 4, 7, 14 }, 4 },
    { "Octave", { 0, 12 }, 2 },
} };

/**
    A chord's notes after an inversion: positive inversions move the lowest notes up an octave, negative ones the
    highest down, one note each.

    @param notes      Semitones above the played note, ascending.
    @param count      How many, 1..6.
    @param inversion  -3..3, limited to count - 1 either way.
    @return           The moved notes, in the same order as given (so note 0 stays the played note's).
*/
[[nodiscard]] std::array<int, 6> invert (const std::array<int, 6>& notes, int count, int inversion) noexcept;

/**
    The arpeggiator's modes, in the order of its Mode choice (ME10).
*/
enum class ArpMode
{
    up,
    down,
    upDown,    ///< Up then down, the ends once.
    downUp,    ///< Down then up, the ends once.
    upAndDown, ///< Up then down, the ends twice.
    asPlayed,
    random,
    chord, ///< Every held note at each step.
};

/**
    What one step of an arpeggio plays.
*/
struct ArpPick
{
    /**
        The key, an index into the held keys (sorted by pitch, or in played order for asPlayed); -1 for all of them.
    */
    int key = 0;

    /**
        Octaves above the key.
    */
    int octave = 0;
};

/**
    Picks what a step of an arpeggio plays.

    @param mode     The mode.
    @param count    How many keys are held, at least 1.
    @param octaves  How many octaves, 1..4.
    @param step     The step, counted from the pattern's start.
    @param random   A random number, for the random mode.
    @return         The key and octave.
*/
[[nodiscard]] ArpPick arpPick (ArpMode mode, int count, int octaves, juce::uint64 step, juce::uint32 random) noexcept;

/**
    The first grid line at or after (or with `after`, strictly after) a tick, the grid restarting at every bar line
    and every second line swung (ME7, ME9).

    @param map        The tempo map, for its bars.
    @param tick       The tick.
    @param stepTicks  The grid's cell length.
    @param swing      The swing, 0.5..0.75.
    @param after      True to skip a line on the tick itself.
    @return           The line's tick.
*/
[[nodiscard]] double nextGridLine (const TempoMap& map, double tick, double stepTicks, double swing,
                                   bool after) noexcept;

/**
    What renderMidiEffects() made: notes without ids, and the links between them by index.
*/
struct RenderedNotes
{
    /**
        The notes, in content positions of a clip without offset or loop.
    */
    std::vector<Note> notes;

    /**
        A link from one note to a later one: indices into notes, and the glide.
    */
    struct Link
    {
        /// The note the link starts from, as an index into the rendered notes.
        size_t from = 0;
        /// The note it leads to, as an index into the rendered notes.
        size_t to = 0;
        /// The glide time in milliseconds.
        double glideMs = 0.0;
    };

    /**
        The links.
    */
    std::vector<Link> links;
};

/**
    Plays a clip through MIDI effects offline and collects what comes out (docs/design/midi-effects.md, ME17): the
    clip's own notes only, from its start, as they would play from there, and cut at its end. Each note keeps its
    source's curves from where it starts and its vibrato.

    @param effects  The effects in order, as in the model; their parameters and bypass are used.
    @param clip     The clip.
    @param map      The project's tempo map.
    @return         The notes.
*/
[[nodiscard]] RenderedNotes renderMidiEffects (std::span<const GraphNode* const> effects, const NoteClip& clip,
                                               const TempoMap& map);

} // namespace midifx

} // namespace dawg
