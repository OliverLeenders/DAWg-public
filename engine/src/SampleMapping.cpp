#include "dawg/SampleMapping.h"

#include "dawg/AudioToMidi.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

// Mapping imported samples to keys (docs/design/sampler-mapping.md): what a file's name says, where a file holding
// several notes is cut, the pitch of each sound, and the key and velocity ranges the sounds get.

namespace dawg
{

namespace
{
constexpr double windowSeconds = 0.01; // the level is measured in 10 ms windows
constexpr double quietSeconds = 0.15;  // a sound starts after this long below the threshold
constexpr double belowPeakDb = 45.0;   // the threshold, below the file's loudest window
constexpr double earlySeconds = 0.05;  // a slot without a sound of its own starts this much before its place
constexpr double gridSeconds = 0.1;    // a sound this close to its place on the grid is on it
constexpr double leadSeconds = 0.002;  // a slot's zone starts this much before its sound
constexpr double shortestSlot = 0.3;   // slots shorter than this aren't notes
constexpr int mostSlots = 32;
constexpr double skipSeconds = 0.05; // the pitch is measured from this far into a sound
constexpr double pitchSeconds = 1.0; // over this long
constexpr double pitchedShare = 0.4; // and found if this share of its frames has one
constexpr int maxLayers = 8;         // a Sampler's layers (dawg/Sampler.h, maxSampleLayers)
constexpr int firstDrumKey = 36;     // drums take keys from a kick's

// The tokens of a name: split at spaces and punctuation, with tracker-style naturals (C-2) joined and the Unicode
// accidentals made ASCII.
[[nodiscard]] juce::StringArray tokensOf (juce::String name)
{
    name = name.replace (juce::CharPointer_UTF8 ("\xe2\x99\xaf"), "#")
               .replace (juce::CharPointer_UTF8 ("\xe2\x99\xad"), "b");
    juce::String joined;

    for (int i = 0; i < name.length(); ++i)
    {
        const auto c = name[i];
        const auto noteBefore = i > 0 && juce::String ("ABCDEFGabcdefg").containsChar (name[i - 1])
                                && (i < 2 || ! juce::CharacterFunctions::isLetter (name[i - 2]));
        const auto digitAfter = i + 1 < name.length() && juce::CharacterFunctions::isDigit (name[i + 1])
                                && (i + 2 >= name.length() || ! juce::CharacterFunctions::isDigit (name[i + 2]));

        if (c == '-' && noteBefore && digitAfter)
            continue; // C-2 is C2

        joined += c;
    }

    juce::StringArray tokens;
    tokens.addTokens (joined, " _-.,()[]+&", "");
    tokens.removeEmptyStrings();
    return tokens;
}

// A token's note as a MIDI key (C4 = 60), or -1: a letter, an accidental and an octave from -1 to 9, after an optional
// "note" (noteF#5).
[[nodiscard]] int noteOf (juce::String token)
{
    if (token.startsWithIgnoreCase ("note"))
        token = token.substring (4);

    if (token.length() < 2 || token.length() > 4)
        return -1;

    static const std::map<juce::juce_wchar, int> steps { { 'c', 0 }, { 'd', 2 }, { 'e', 4 }, { 'f', 5 },
                                                         { 'g', 7 }, { 'a', 9 }, { 'b', 11 } };
    const auto letter = steps.find (juce::CharacterFunctions::toLowerCase (token[0]));

    if (letter == steps.end())
        return -1;

    auto rest = token.substring (1);
    auto accidental = 0;

    if (rest.startsWithChar ('#'))
        accidental = 1;
    else if (rest.startsWithChar ('b') && rest.length() > 1)
        accidental = -1;

    if (accidental != 0)
        rest = rest.substring (1);

    if (rest.isEmpty() || ! rest.containsOnly ("-0123456789") || rest.length() > 2
        || (rest.length() == 2 && rest[0] != '-'))
        return -1;

    const auto key = (rest.getIntValue() + 1) * 12 + letter->second + accidental;
    return key >= 0 && key <= 127 ? key : -1;
}

[[nodiscard]] int dynamicOf (const juce::String& token)
{
    static const std::map<juce::String, int> dynamics { { "ppp", 16 }, { "pp", 32 }, { "p", 48 },   { "mp", 64 },
                                                        { "mf", 80 },  { "f", 96 },  { "ff", 112 }, { "fff", 127 } };
    // A single p or f only in lower case: F is a note's letter too.
    const auto lower = token.length() == 1 ? token : token.toLowerCase();
    const auto found = dynamics.find (lower);
    return found == dynamics.end() ? -1 : found->second;
}

[[nodiscard]] bool isNumber (const juce::String& token)
{
    return token.isNotEmpty() && token.length() <= 3 && token.containsOnly ("0123456789");
}

[[nodiscard]] double median (std::vector<double> values)
{
    std::ranges::sort (values);
    const auto n = values.size();
    return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

// The level of each 10 ms window of the mono sum, in dBFS.
[[nodiscard]] std::vector<double> levels (const juce::AudioBuffer<float>& audio, int window)
{
    const auto frames = audio.getNumSamples();
    const auto channels = std::max (1, audio.getNumChannels());
    std::vector<double> result;
    result.reserve (static_cast<size_t> (frames / window + 1));

    for (int start = 0; start + window <= frames; start += window)
    {
        double sum = 0.0;

        for (int i = start; i < start + window; ++i)
        {
            double mono = 0.0;

            for (int c = 0; c < audio.getNumChannels(); ++c)
                mono += audio.getSample (c, i);

            mono /= channels;
            sum += mono * mono;
        }

        result.push_back (10.0 * std::log10 (sum / window + 1e-12));
    }

    return result;
}

// The pitch of a sound from `start` (SM4), or 0.
[[nodiscard]] double pitchFrom (const juce::AudioBuffer<float>& audio, double sampleRate, juce::int64 start,
                                juce::int64 end)
{
    const auto from =
        static_cast<int> (std::min<juce::int64> (start + static_cast<juce::int64> (skipSeconds * sampleRate), end));
    const auto length =
        static_cast<int> (std::min<juce::int64> (static_cast<juce::int64> (pitchSeconds * sampleRate), end - from));

    if (length < static_cast<int> (0.05 * sampleRate))
        return 0.0;

    juce::AudioBuffer<float> part (audio.getNumChannels(), length);

    for (int c = 0; c < audio.getNumChannels(); ++c)
        part.copyFrom (c, 0, audio, c, from, length);

    const auto frames = detectPitch (part, sampleRate);
    auto loudest = -120.0;

    for (const auto& frame : frames)
        loudest = std::max (loudest, frame.levelDb);

    std::vector<double> pitches;
    auto heard = 0;

    for (const auto& frame : frames)
    {
        if (frame.levelDb < loudest - 40.0)
            continue;

        ++heard;

        if (frame.pitch > 0.0)
            pitches.push_back (frame.pitch);
    }

    if (heard == 0 || static_cast<double> (pitches.size()) < pitchedShare * heard)
        return 0.0;

    return median (std::move (pitches));
}

// The slots of one file go up by the same interval: a slot without a pitch, or an octave off where its neighbours
// put it, takes its place in that run (SM4).
void evenOut (std::vector<SampleSlice>& slices)
{
    if (slices.size() < 2)
        return;

    std::map<int, int> intervals;

    for (size_t i = 1; i < slices.size(); ++i)
        if (slices[i - 1].pitch > 0.0 && slices[i].pitch > 0.0)
            ++intervals[static_cast<int> (std::lround (slices[i].pitch - slices[i - 1].pitch))];

    const auto found = std::ranges::count_if (slices, [] (const SampleSlice& s) { return s.pitch > 0.0; });

    if (found == 0)
        return;

    // The commonest step between neighbours, an octave if none is known. Steps an octave off count as the step they
    // would be (13 and -11 as 1), so a few octave errors don't hide a run of semitones; whole octaves (or the same
    // note over again) go by the steps as found.
    const auto folded = [] (int interval) { return ((interval % 12) + 12 + 5) % 12 - 5; };
    std::map<int, int> foldedVotes;

    for (const auto& [interval, count] : intervals)
        foldedVotes[folded (interval)] += count;

    auto step = 12;
    auto votes = 0;

    for (const auto& [interval, count] : foldedVotes)
        if (count > votes)
        {
            step = interval;
            votes = count;
        }

    if (step == 0)
    {
        step = 12;
        votes = 0;

        for (const auto& [interval, count] : intervals)
            if (folded (interval) == 0 && (count > votes || (count == votes && interval == 12)))
            {
                step = interval;
                votes = count;
            }
    }

    if (step <= 0)
        return;

    // When most neighbours agree, the run is clear and every slot takes its place in it: a pitch above the
    // detector's range is found as something else altogether.
    const auto pairs =
        std::accumulate (intervals.begin(), intervals.end(), 0, [] (int sum, const auto& e) { return sum + e.second; });
    const auto clearRun = votes >= 2 && votes * 2 > pairs;

    // The run's first key, as most slots put it.
    std::map<int, int> firsts;

    for (size_t i = 0; i < slices.size(); ++i)
        if (slices[i].pitch > 0.0)
            ++firsts[static_cast<int> (std::lround (slices[i].pitch)) - step * static_cast<int> (i)];

    const auto first = std::ranges::max_element (firsts, {}, [] (const auto& entry) { return entry.second; })->first;

    for (size_t i = 0; i < slices.size(); ++i)
    {
        const auto expected = first + step * static_cast<int> (i);
        const auto key = static_cast<int> (std::lround (slices[i].pitch));

        if (slices[i].pitch <= 0.0 || (std::abs (key - expected) % 12 == 0 && key != expected)
            || (clearRun && key != expected))
            slices[i].pitch = static_cast<double> (expected);
    }
}

// Velocity ranges for loudness layers, by their loudness: each reaches halfway to the next (SM8).
[[nodiscard]] std::map<int, std::pair<int, int>> velocityRanges (const std::vector<int>& loudnesses)
{
    std::map<int, std::pair<int, int>> ranges;
    std::vector<int> sorted (loudnesses);
    std::ranges::sort (sorted);
    sorted.erase (std::unique (sorted.begin(), sorted.end()), sorted.end());

    for (size_t i = 0; i < sorted.size(); ++i)
    {
        const auto low = i == 0 ? 0 : (sorted[i - 1] + sorted[i]) / 2 + 1;
        const auto high = i + 1 == sorted.size() ? 127 : (sorted[i] + sorted[i + 1]) / 2;
        ranges[sorted[i]] = { low, high };
    }

    return ranges;
}
} // namespace

SampleName parseSampleName (const juce::String& name)
{
    SampleName result;
    const auto tokens = tokensOf (name);

    juce::StringArray piece;
    auto skipNext = false;

    for (int i = 0; i < tokens.size(); ++i)
    {
        const auto& token = tokens[i];
        const auto lower = token.toLowerCase();
        auto named = false;

        if (const auto key = noteOf (token); key >= 0)
        {
            result.key = key; // the last one counts
            named = true;
        }

        if (const auto loudness = dynamicOf (token); loudness > 0)
        {
            result.velocity = loudness;
            named = true;
        }

        if ((lower == "velocity" || lower == "vel") && i + 1 < tokens.size() && isNumber (tokens[i + 1]))
        {
            result.velocity = std::clamp (tokens[i + 1].getIntValue(), 1, 127);
            named = skipNext = true;
        }
        else if ((lower.startsWith ("velocity") || lower.startsWith ("vel"))
                 && isNumber (lower.trimCharactersAtStart ("velocity")))
        {
            result.velocity = std::clamp (lower.trimCharactersAtStart ("velocity").getIntValue(), 1, 127);
            named = true;
        }

        // A side, a microphone, a take (TL4).
        if (lower == "l" || lower == "left")
        {
            result.side = -1;
            named = true;
        }
        else if (lower == "r" || lower == "right")
        {
            result.side = 1;
            named = true;
        }
        else if ((lower.endsWith ("mic") && lower.length() > 3) || lower == "oh" || lower == "ohl" || lower == "ohr"
                 || lower == "room" || lower == "close")
        {
            result.microphone = token;
            named = true;
        }
        else if (isNumber (token) && token.length() >= 2 && token[0] == '0')
        {
            result.take = true;
            named = true;
        }

        if (named)
            continue;

        if (skipNext && isNumber (token))
        {
            skipNext = false;
            continue;
        }

        piece.add (token);
    }

    // A trailing range: two numbers, the second above the first, neither a take's (rototom_1_05 is a take).
    const auto takeNumber = [] (const juce::String& token) { return token.length() >= 2 && token[0] == '0'; };

    if (tokens.size() >= 2 && isNumber (tokens[tokens.size() - 2]) && isNumber (tokens[tokens.size() - 1])
        && ! takeNumber (tokens[tokens.size() - 2]) && ! takeNumber (tokens[tokens.size() - 1]))
    {
        const auto low = tokens[tokens.size() - 2].getIntValue();
        const auto high = tokens[tokens.size() - 1].getIntValue();

        if (high <= 127 && high - low >= 4)
        {
            result.lowVelocity = low;
            result.highVelocity = high;
            piece.removeString (tokens[tokens.size() - 2]);
            piece.removeString (tokens[tokens.size() - 1]);
        }
    }

    result.piece = piece.joinIntoString (" ");

    // A MIDI number beside the note: an octave above the name's key when the name counts C3 as 60.
    if (result.key >= 0)
        for (const auto& token : tokens)
            if (isNumber (token) && token.length() >= 2)
            {
                const auto number = token.getIntValue();

                if (number == result.key + 12 || number == result.key - 12)
                    result.octaveHint = number - result.key;
            }

    return result;
}

double soundPitch (const juce::AudioBuffer<float>& audio, double sampleRate, juce::int64 start, juce::int64 end)
{
    return pitchFrom (audio, sampleRate, std::max<juce::int64> (0, start),
                      std::min<juce::int64> (end, audio.getNumSamples()));
}

std::vector<SampleSlice> analyseSample (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const auto frames = static_cast<juce::int64> (audio.getNumSamples());
    const auto window = std::max (1, static_cast<int> (windowSeconds * sampleRate));
    const auto level = levels (audio, window);

    if (level.empty())
        return { SampleSlice {} };

    const auto peak = *std::ranges::max_element (level);
    const auto threshold = std::max (peak - belowPeakDb, -90.0);
    const auto quietWindows = static_cast<int> (quietSeconds / windowSeconds);

    // Where sounds start: above the threshold after a while below it.
    std::vector<juce::int64> starts;
    auto quiet = quietWindows;

    for (size_t i = 0; i < level.size(); ++i)
    {
        if (level[i] > threshold)
        {
            if (quiet >= quietWindows)
                starts.push_back (static_cast<juce::int64> (i) * window);

            quiet = 0;
        }
        else
        {
            ++quiet;
        }
    }

    // The slots: the shortest period from the first sound whose grid the sounds fall on, a slot or two without a sound
    // of its own allowed, the last one as long as the file leaves (SM3).
    if (starts.size() >= 2)
    {
        const auto first = starts.front();
        const auto tolerance = gridSeconds * sampleRate;
        const auto shortest = shortestSlot * sampleRate;
        std::vector<double> periods;

        for (size_t j = 1; j < starts.size(); ++j)
            for (auto m = 1; m <= mostSlots; ++m)
                if (const auto period = static_cast<double> (starts[j] - first) / m; period >= shortest)
                    periods.push_back (period);

        std::ranges::sort (periods);

        for (const auto period : periods)
        {
            const auto n = static_cast<int> ((static_cast<double> (frames - first) - shortest) / period) + 1;

            if (n < 3 || n > mostSlots)
                continue;

            std::vector<std::optional<juce::int64>> sounds (static_cast<size_t> (n));
            auto hits = 0;

            for (auto k = 0; k < n; ++k)
            {
                const auto at = static_cast<double> (first) + k * period;
                const auto found = std::ranges::find_if (
                    starts, [&] (juce::int64 s) { return std::abs (static_cast<double> (s) - at) <= tolerance; });

                if (found != starts.end())
                {
                    sounds[static_cast<size_t> (k)] = *found;
                    ++hits;
                }
            }

            if (hits < 2 || (n - hits) * 4 > n)
                continue;

            const auto lead = static_cast<juce::int64> (leadSeconds * sampleRate);
            const auto early = static_cast<juce::int64> (earlySeconds * sampleRate);
            std::vector<SampleSlice> slices;

            for (auto k = 0; k < n; ++k)
            {
                const auto at = static_cast<juce::int64> (std::llround (static_cast<double> (first) + k * period));
                const auto& sound = sounds[static_cast<size_t> (k)];
                slices.push_back ({ std::max<juce::int64> (0, sound ? *sound - lead : at - early), frames, 0.0 });
            }

            for (size_t k = 0; k + 1 < slices.size(); ++k)
                slices[k].end = slices[k + 1].start;

            for (auto& slice : slices)
                slice.pitch = pitchFrom (audio, sampleRate, slice.start, slice.end);

            evenOut (slices);

            // A slot with nothing in it isn't a note.
            std::erase_if (slices,
                           [&] (const SampleSlice& slice)
                           {
                               const auto from = static_cast<size_t> (slice.start / window);
                               const auto to = std::min (level.size(), static_cast<size_t> (slice.end / window));
                               return std::none_of (level.begin() + static_cast<std::ptrdiff_t> (std::min (from, to)),
                                                    level.begin() + static_cast<std::ptrdiff_t> (to),
                                                    [&] (double l) { return l > threshold; });
                           });

            if (slices.size() >= 3)
                return slices;
        }
    }

    SampleSlice whole;
    whole.pitch = pitchFrom (audio, sampleRate, starts.empty() ? 0 : starts.front(), frames);
    return { whole };
}

namespace
{
// One sound to place: a slice of a file, with what its name and its pitch say.
struct Sound
{
    size_t file = 0;
    SampleSlice slice;
    SampleName name;
    juce::String sortName;
    int key = -1;
    bool exact = false; // the key the file stores: no octave convention moves it
};

// Numbered files without notes in their names (key01, key02…), one sound each: their pitches, in number order, go up
// by the same interval, as a file's slots do, so one found an octave off is put back (SM4).
void evenOutSeries (std::vector<Sound>& sounds)
{
    std::vector<std::pair<int, Sound*>> series;
    juce::String stem;

    for (auto& sound : sounds)
    {
        if (sound.slice.end != 0)
            continue;

        if (sound.name.key >= 0)
            return;

        const auto name = sound.sortName.trimEnd();
        const auto digits = name.length() - name.trimCharactersAtEnd ("0123456789").length();

        if (digits == 0)
            return;

        const auto own = name.dropLastCharacters (digits).trimCharactersAtEnd (" _-.");

        if (series.empty())
            stem = own;
        else if (own != stem)
            return;

        series.emplace_back (name.getLastCharacters (digits).getIntValue(), &sound);
    }

    if (series.size() < 3)
        return;

    std::ranges::sort (series, {}, &std::pair<int, Sound*>::first);
    std::vector<SampleSlice> run;

    for (const auto& [number, sound] : series)
        run.push_back (sound->slice);

    evenOut (run);

    for (size_t i = 0; i < series.size(); ++i)
        series[i].second->slice.pitch = run[i].pitch;
}
// The layer and pan of a sound (TL4): one layer per microphone among the files, in name order; else left and right in
// layers 1 and 2, panned to their sides.
[[nodiscard]] std::pair<int, double> layers (const std::vector<Sound>& sounds, const Sound& sound)
{
    const auto pan = static_cast<double> (sound.name.side);
    juce::StringArray microphones;

    for (const auto& other : sounds)
        if (other.name.microphone.isNotEmpty())
            microphones.addIfNotAlreadyThere (other.name.microphone, true);

    if (sound.name.microphone.isNotEmpty())
    {
        microphones.sortNatural();
        const auto index = microphones.indexOf (sound.name.microphone, true);
        return { std::clamp (index + 1, 1, maxLayers), pan };
    }

    return { sound.name.side > 0 ? 2 : 1, pan };
}

// Velocity ranges by the sounds' loudnesses, the same on every key (SM8).
[[nodiscard]] std::pair<int, int> velocitiesOf (const std::vector<Sound>& sounds, const Sound& sound)
{
    if (sound.name.lowVelocity >= 0)
        return { sound.name.lowVelocity, sound.name.highVelocity };

    if (sound.name.velocity <= 0)
        return { 0, 127 };

    std::vector<int> loudnesses;

    for (const auto& other : sounds)
        if (other.name.velocity > 0 && other.name.lowVelocity < 0)
            loudnesses.push_back (other.name.velocity);

    return velocityRanges (loudnesses)[sound.name.velocity];
}

// Drums: a key to each piece, from a kick's in name order, its files layered and taking turns (TL5).
[[nodiscard]] std::vector<PlannedZone> drumZones (const std::vector<Sound>& sounds)
{
    juce::StringArray pieces;

    for (const auto& sound : sounds)
    {
        // A file cut into slots is a piece per slot.
        const auto piece =
            sound.slice.end != 0 ? sound.sortName + " " + juce::String (sound.slice.start) : sound.name.piece;
        pieces.addIfNotAlreadyThere (piece.isEmpty() ? sound.sortName : piece, true);
    }

    pieces.sortNatural();
    std::vector<PlannedZone> zones;

    for (const auto& sound : sounds)
    {
        auto piece = sound.slice.end != 0 ? sound.sortName + " " + juce::String (sound.slice.start) : sound.name.piece;

        if (piece.isEmpty())
            piece = sound.sortName;

        PlannedZone zone;
        zone.file = sound.file;
        zone.start = sound.slice.start;
        zone.end = sound.slice.end;
        zone.rootKey = zone.lowKey = zone.highKey = std::clamp (firstDrumKey + pieces.indexOf (piece, true), 0, 127);
        std::tie (zone.lowVelocity, zone.highVelocity) = velocitiesOf (sounds, sound);
        std::tie (zone.layer, zone.pan) = layers (sounds, sound);
        zones.push_back (zone);
    }

    std::ranges::stable_sort (zones, [] (const PlannedZone& a, const PlannedZone& b)
                              { return std::tie (a.rootKey, a.lowVelocity) < std::tie (b.rootKey, b.lowVelocity); });
    return zones;
}
} // namespace

namespace
{
// The loops the files store, on the zones that play them whole (SM5a).
[[nodiscard]] std::vector<PlannedZone> withStoredLoops (std::vector<PlannedZone> zones,
                                                        const std::vector<SampleToMap>& files)
{
    for (auto& zone : zones)
    {
        const auto& file = files[zone.file];

        if (zone.end == 0 && file.loopStart >= 0 && file.loopEnd > file.loopStart)
        {
            zone.loopStart = file.loopStart;
            zone.loopEnd = file.loopEnd;
        }
    }

    return zones;
}
} // namespace

std::optional<std::vector<PlannedZone>> planSampleZones (const std::vector<SampleToMap>& files)
{
    std::vector<Sound> sounds;
    auto named = 0;
    auto pitched = 0;
    std::map<int, int> shifts;

    for (size_t f = 0; f < files.size(); ++f)
    {
        auto name = parseSampleName (files[f].name);
        const auto whole = files[f].slices.size() <= 1;

        // What the file stores goes first: its root key exactly, else a note its title names (SM5a). A stored middle C
        // against a different note in the name is the default many recorders write, so the name wins then.
        const auto stored = files[f].storedKey >= 0 && ! (files[f].storedKey == 60 && name.key >= 0 && name.key != 60);

        if (stored)
            name.key = std::clamp (files[f].storedKey, 0, 127);
        else if (name.key < 0 && files[f].title.isNotEmpty())
            name.key = parseSampleName (files[f].title).key;

        if (whole && name.key >= 0)
            ++named;

        for (const auto& slice : files[f].slices)
        {
            Sound sound { f, slice, name, files[f].name, -1, stored && whole };

            // A sliced file's notes are where its slices' pitches put them; a file of one sound goes by its name first.
            if (! whole)
                sound.name.key = -1;

            if (slice.pitch > 0.0)
                ++pitched;

            if (sound.name.key >= 0 && slice.pitch > 0.0 && ! sound.exact)
            {
                const auto off = static_cast<int> (std::lround ((slice.pitch - sound.name.key) / 12.0)) * 12;

                if (std::abs (off) == 12)
                    ++shifts[off];
                else if (off == 0)
                    ++shifts[0];
            }

            sounds.push_back (sound);
        }
    }

    if (sounds.empty())
        return std::nullopt;

    // Melodic or drums (SM6).
    const auto melodic =
        named * 2 >= static_cast<int> (files.size()) || pitched * 4 >= static_cast<int> (sounds.size()) * 3;

    if (! melodic)
        return sounds.size() > 1 ? std::optional (withStoredLoops (drumZones (sounds), files)) : std::nullopt;

    // The octave the names count middle C in (SM5): what the MIDI numbers beside them say, or else the pitches.
    std::map<int, int> hints;

    for (const auto& sound : sounds)
        if (sound.name.octaveHint != 0 && ! sound.exact)
            ++hints[sound.name.octaveHint];

    auto shift = 0;

    if (! hints.empty())
    {
        shift = std::ranges::max_element (hints, {}, [] (const auto& entry) { return entry.second; })->first;
    }
    else
    {
        const auto compared =
            std::accumulate (shifts.begin(), shifts.end(), 0, [] (int sum, const auto& e) { return sum + e.second; });

        for (const auto& [off, count] : shifts)
            if (off != 0 && count * 2 > compared)
                shift = off;
    }

    evenOutSeries (sounds);

    for (auto& sound : sounds)
    {
        if (sound.name.key >= 0)
            sound.key = std::clamp (sound.name.key + (sound.exact ? 0 : shift), 0, 127);
        else if (sound.slice.pitch > 0.0)
            sound.key = std::clamp (static_cast<int> (std::lround (sound.slice.pitch)), 0, 127);
    }

    // Takes of one piece are one note: the key most of them were found at (TL4).
    std::map<juce::String, std::map<int, int>> takeKeys;

    for (const auto& sound : sounds)
        if (sound.name.take && sound.name.key < 0 && sound.key >= 0)
            ++takeKeys[sound.name.piece][sound.key];

    for (auto& sound : sounds)
        if (sound.name.take && sound.name.key < 0 && takeKeys.contains (sound.name.piece))
            sound.key =
                std::ranges::max_element (takeKeys[sound.name.piece], {}, [] (const auto& e) { return e.second; })
                    ->first;

    // A sound with neither a name nor a pitch has no key to go on.
    std::erase_if (sounds, [] (const Sound& s) { return s.key < 0; });

    if (sounds.empty())
        return std::nullopt;

    std::ranges::stable_sort (sounds, [] (const Sound& a, const Sound& b)
                              { return a.sortName.compareNatural (b.sortName) < 0; });

    // A layer is a loudness, or none; key ranges are per layer (SM7).
    const auto layerOf = [] (const Sound& s) { return s.name.lowVelocity >= 0 ? -2 : s.name.velocity; };
    std::map<int, std::vector<int>> rootsByLayer;

    for (const auto& sound : sounds)
        rootsByLayer[layerOf (sound)].push_back (sound.key);

    std::map<int, std::map<int, std::pair<int, int>>> keyRanges;

    for (auto& [layer, roots] : rootsByLayer)
    {
        std::ranges::sort (roots);
        roots.erase (std::unique (roots.begin(), roots.end()), roots.end());

        for (size_t i = 0; i < roots.size(); ++i)
        {
            const auto low = i == 0 ? 0 : (roots[i - 1] + roots[i]) / 2 + 1;
            const auto high = i + 1 == roots.size() ? 127 : (roots[i] + roots[i + 1]) / 2;
            keyRanges[layer][roots[i]] = { low, high };
        }
    }

    // Every layer spans the keyboard, so the loudnesses split the velocities the same way on every key.
    std::vector<int> loudnesses;

    for (const auto& sound : sounds)
        if (sound.name.velocity > 0 && sound.name.lowVelocity < 0)
            loudnesses.push_back (sound.name.velocity);

    auto velocities = velocityRanges (loudnesses);

    std::vector<PlannedZone> zones;

    for (const auto& sound : sounds)
    {
        PlannedZone zone;
        zone.file = sound.file;
        zone.start = sound.slice.start;
        zone.end = sound.slice.end;
        zone.rootKey = sound.key;
        std::tie (zone.lowKey, zone.highKey) = keyRanges[layerOf (sound)][sound.key];

        if (sound.name.lowVelocity >= 0)
        {
            zone.lowVelocity = sound.name.lowVelocity;
            zone.highVelocity = sound.name.highVelocity;
        }
        else if (sound.name.velocity > 0)
        {
            std::tie (zone.lowVelocity, zone.highVelocity) = velocities[sound.name.velocity];
        }

        std::tie (zone.layer, zone.pan) = layers (sounds, sound);
        zones.push_back (zone);
    }

    // Loudest layers last, so a key's softer layers are found by velocity before a layer without one (SM8).
    std::ranges::stable_sort (zones, [] (const PlannedZone& a, const PlannedZone& b)
                              { return std::tie (a.rootKey, a.lowVelocity) < std::tie (b.rootKey, b.lowVelocity); });
    return withStoredLoops (std::move (zones), files);
}

} // namespace dawg
