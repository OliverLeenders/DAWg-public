#include "dawg/Sfz.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

// Reading SFZ patches into Sampler zones (docs/design/sampler-mapping.md, section SF).

namespace dawg
{

namespace
{
constexpr int mostIncludes = 8; // how deep #include goes
constexpr int mostLayers = 8;   // a Sampler's layers (dawg/Sampler.h, maxSampleLayers)
constexpr int mostChokeGroups = 8;

using Opcodes = std::map<juce::String, juce::String>;

// The levels opcodes pass down from; anything else (<curve>, <effect>, <midi>, <sample>) isn't read.
enum class Level
{
    control,
    global,
    master,
    group,
    region,
    other,
};

[[nodiscard]] Level levelOf (const juce::String& header)
{
    static const std::map<juce::String, Level> levels { { "control", Level::control },
                                                        { "global", Level::global },
                                                        { "master", Level::master },
                                                        { "group", Level::group },
                                                        { "region", Level::region } };
    const auto found = levels.find (header.toLowerCase());
    return found == levels.end() ? Level::other : found->second;
}

// The text with its comments gone, its includes in place and its #defines applied, line by line.
[[nodiscard]] juce::String preprocess (const juce::String& text, const juce::File& folder,
                                       const std::function<juce::String (const juce::File&)>& readInclude,
                                       std::map<juce::String, juce::String>& defines, int depth)
{
    juce::String result;
    auto inBlock = false;

    for (auto line : juce::StringArray::fromLines (text))
    {
        // Comments: /* */ across lines, // to the end of one.
        juce::String kept;

        for (int i = 0; i < line.length(); ++i)
        {
            if (inBlock)
            {
                if (line.substring (i, i + 2) == "*/")
                {
                    inBlock = false;
                    ++i;
                }

                continue;
            }

            if (line.substring (i, i + 2) == "/*")
            {
                inBlock = true;
                ++i;
                continue;
            }

            if (line.substring (i, i + 2) == "//")
                break;

            kept += line[i];
        }

        for (const auto& [name, value] : defines)
            kept = kept.replace (name, value);

        const auto trimmed = kept.trim();

        if (trimmed.startsWith ("#define"))
        {
            const auto rest = trimmed.substring (7).trim();
            const auto name = rest.upToFirstOccurrenceOf (" ", false, false).trim();

            if (name.startsWithChar ('$'))
                defines[name] = rest.fromFirstOccurrenceOf (" ", false, false).trim();

            continue;
        }

        if (trimmed.startsWith ("#include"))
        {
            const auto path = trimmed.substring (8).trim().unquoted().replaceCharacter ('\\', '/');

            if (depth < mostIncludes)
                result << preprocess (readInclude (folder.getChildFile (path)), folder, readInclude, defines, depth + 1)
                       << "\n";

            continue;
        }

        result << kept << "\n";
    }

    return result;
}

// A key: a MIDI number, or a name with C4 at 60 (c4, C#4, db4).
[[nodiscard]] std::optional<int> keyOf (const juce::String& value)
{
    const auto text = value.trim();

    if (text.containsOnly ("-0123456789") && text.isNotEmpty())
        return text.getIntValue();

    static const std::map<juce::juce_wchar, int> steps { { 'c', 0 }, { 'd', 2 }, { 'e', 4 }, { 'f', 5 },
                                                         { 'g', 7 }, { 'a', 9 }, { 'b', 11 } };
    const auto letter = steps.find (juce::CharacterFunctions::toLowerCase (text[0]));

    if (letter == steps.end())
        return std::nullopt;

    auto rest = text.substring (1);
    auto accidental = 0;

    if (rest.startsWithChar ('#'))
        accidental = 1;
    else if (rest.startsWithChar ('b') && rest.length() > 1)
        accidental = -1;

    if (accidental != 0)
        rest = rest.substring (1);

    if (rest.isEmpty() || ! rest.containsOnly ("-0123456789"))
        return std::nullopt;

    return (rest.getIntValue() + 1) * 12 + letter->second + accidental;
}

// The opcodes on a line: `name=value`, a value running to the next opcode or header, spaces and all.
void readLine (const juce::String& line, const std::function<void (const juce::String&)>& onHeader,
               const std::function<void (const juce::String&, const juce::String&)>& onOpcode)
{
    // Where each header and opcode starts.
    struct Mark
    {
        int start = 0;
        bool header = false;
    };

    std::vector<Mark> marks;

    for (int i = 0; i < line.length(); ++i)
    {
        if (line[i] == '<')
        {
            marks.push_back ({ i, true });
            continue;
        }

        const auto atWordStart = i == 0 || juce::CharacterFunctions::isWhitespace (line[i - 1]) || line[i - 1] == '>';

        if (! atWordStart || ! (juce::CharacterFunctions::isLetterOrDigit (line[i]) || line[i] == '_'))
            continue;

        auto j = i;

        while (j < line.length() && (juce::CharacterFunctions::isLetterOrDigit (line[j]) || line[j] == '_'))
            ++j;

        if (j < line.length() && line[j] == '=')
            marks.push_back ({ i, false });
    }

    for (size_t m = 0; m < marks.size(); ++m)
    {
        const auto end = m + 1 < marks.size() ? marks[m + 1].start : line.length();
        const auto part = line.substring (marks[m].start, end);

        if (marks[m].header)
            onHeader (part.fromFirstOccurrenceOf ("<", false, false).upToFirstOccurrenceOf (">", false, false).trim());
        else
            onOpcode (part.upToFirstOccurrenceOf ("=", false, false).trim().toLowerCase(),
                      part.fromFirstOccurrenceOf ("=", false, false).trim());
    }
}

[[nodiscard]] double number (const Opcodes& opcodes, const juce::String& name, double fallback)
{
    const auto found = opcodes.find (name);
    return found == opcodes.end() ? fallback : found->second.getDoubleValue();
}

[[nodiscard]] const juce::String* find (const Opcodes& opcodes, std::initializer_list<const char*> names)
{
    for (const auto* name : names)
        if (const auto found = opcodes.find (name); found != opcodes.end())
            return &found->second;

    return nullptr;
}

// The opcodes a zone takes something from (SF3 to SF5); the rest are reported as ignored (SF6).
[[nodiscard]] bool known (const juce::String& opcode)
{
    static const juce::StringArray names {
        "sample",        "lokey",       "hikey",        "key",          "pitch_keycenter", "lovel",
        "hivel",         "offset",      "end",          "tune",         "pitch",           "transpose",
        "volume",        "amplitude",   "pan",          "loop_mode",    "loopmode",        "loop_start",
        "loopstart",     "loop_end",    "loopend",      "ampeg_attack", "ampeg_decay",     "ampeg_sustain",
        "ampeg_release", "group",       "off_by",       "seq_length",   "seq_position",    "lorand",
        "hirand",        "trigger",     "default_path", "note_offset",  "octave_offset",   "xfin_lovel",
        "xfin_hivel",    "xfout_lovel", "xfout_hivel",  "amp_veltrack", "global_label",    "master_label",
        "group_label",   "region_label"
    };
    return names.contains (opcode);
}

// How an opcode is named among the ignored: a numbered family once (amp_velcurve_N), not every number of it.
[[nodiscard]] juce::String ignoredName (const juce::String& opcode)
{
    const auto digits = opcode.length() - opcode.trimCharactersAtEnd ("0123456789").length();
    return digits > 0 && opcode.trimCharactersAtEnd ("0123456789").endsWithChar ('_')
               ? opcode.trimCharactersAtEnd ("0123456789") + "N"
               : opcode;
}

// One region, before its layer is known.
struct Region
{
    juce::File sample;
    juce::DynamicObject::Ptr fields;
    int lowKey = 0;
    int highKey = 127;
    int lowVelocity = 0;
    int highVelocity = 127;
    bool alternates = false;
    int sequence = 1;
    double randomLow = 0.0;
    double randomHigh = 1.0;
    juce::String group;
    juce::String offBy;
};
} // namespace

SfzPatch readSfz (const juce::String& text, const juce::File& folder,
                  const std::function<juce::String (const juce::File&)>& readInclude,
                  const std::function<bool (const juce::File&)>& exists)
{
    SfzPatch patch;
    std::map<juce::String, juce::String> defines;
    const auto source = preprocess (text, folder, readInclude, defines, 0);

    std::map<Level, Opcodes> levels;
    auto current = Level::other;
    auto inRegion = false;
    std::vector<Region> regions;
    juce::String defaultPath;
    auto keyOffset = 0;

    const auto finishRegion = [&]
    {
        if (! inRegion)
            return;

        inRegion = false;
        Opcodes opcodes;

        for (const auto level : { Level::global, Level::master, Level::group, Level::region })
            for (const auto& [name, value] : levels[level])
                opcodes[name] = value;

        for (const auto& [name, value] : opcodes)
            if (! known (name))
                patch.ignored.addIfNotAlreadyThere (ignoredName (name));

        const auto* sample = find (opcodes, { "sample" });
        const auto* trigger = find (opcodes, { "trigger" });

        if (sample == nullptr || (trigger != nullptr && trigger->startsWithIgnoreCase ("release")))
        {
            ++patch.skipped;
            return;
        }

        Region region;
        region.sample = folder.getChildFile ((defaultPath + *sample).replaceCharacter ('\\', '/'));

        if (! exists (region.sample))
        {
            ++patch.skipped;
            return;
        }

        const auto keyIn = [&] (const char* name, int fallback)
        {
            const auto* value = find (opcodes, { name });
            const auto key = value != nullptr ? keyOf (*value) : std::nullopt;
            return std::clamp (key ? *key + keyOffset : fallback, 0, 127);
        };

        auto root = 60;

        if (find (opcodes, { "key" }) != nullptr)
            region.lowKey = region.highKey = root = keyIn ("key", 60);

        region.lowKey = keyIn ("lokey", region.lowKey);
        region.highKey = std::max (region.lowKey, keyIn ("hikey", region.highKey));

        if (const auto* centre = find (opcodes, { "pitch_keycenter" }); centre != nullptr && keyOf (*centre))
            root = keyIn ("pitch_keycenter", root);
        else if (find (opcodes, { "key" }) == nullptr && region.lowKey == region.highKey)
            root = region.lowKey;

        region.lowVelocity = std::clamp (static_cast<int> (number (opcodes, "lovel", 0)), 0, 127);
        region.lowVelocity = region.lowVelocity <= 1 ? 0 : region.lowVelocity;
        region.highVelocity = std::clamp (static_cast<int> (number (opcodes, "hivel", 127)), region.lowVelocity, 127);

        auto* fields = new juce::DynamicObject();
        region.fields = fields;
        fields->setProperty ("rootKey", root);
        fields->setProperty ("lowKey", region.lowKey);
        fields->setProperty ("highKey", region.highKey);
        fields->setProperty ("lowVelocity", region.lowVelocity);
        fields->setProperty ("highVelocity", region.highVelocity);

        if (const auto offset = number (opcodes, "offset", 0); offset > 0)
            fields->setProperty ("start", static_cast<juce::int64> (offset));

        // SFZ's ends are the last frame played.
        if (const auto end = number (opcodes, "end", 0); end > 0)
            fields->setProperty ("end", static_cast<juce::int64> (end) + 1);

        const auto tune =
            number (opcodes, "tune", number (opcodes, "pitch", 0)) / 100.0 + number (opcodes, "transpose", 0);

        if (! juce::exactlyEqual (tune, 0.0))
            fields->setProperty ("tune", std::clamp (tune, -48.0, 48.0));

        auto gain = number (opcodes, "volume", 0);

        if (const auto amplitude = number (opcodes, "amplitude", 100);
            amplitude > 0 && ! juce::exactlyEqual (amplitude, 100.0))
            gain += 20.0 * std::log10 (amplitude / 100.0);

        if (! juce::exactlyEqual (gain, 0.0))
            fields->setProperty ("gainDb", std::clamp (gain, -60.0, 24.0));

        if (const auto pan = number (opcodes, "pan", 0); ! juce::exactlyEqual (pan, 0.0))
            fields->setProperty ("pan", std::clamp (pan / 100.0, -1.0, 1.0));

        const auto* loopMode = find (opcodes, { "loop_mode", "loopmode" });
        const auto mode = loopMode == nullptr              ? juce::String ("gate")
                          : *loopMode == "one_shot"        ? juce::String ("oneShot")
                          : loopMode->startsWith ("loop_") ? juce::String ("loop")
                                                           : juce::String ("gate");
        fields->setProperty ("mode", mode);

        if (mode == "loop")
        {
            if (const auto* start = find (opcodes, { "loop_start", "loopstart" }))
                fields->setProperty ("loopStart", static_cast<juce::int64> (start->getLargeIntValue()));

            if (const auto* end = find (opcodes, { "loop_end", "loopend" }))
                fields->setProperty ("loopEnd", static_cast<juce::int64> (end->getLargeIntValue()) + 1);
        }

        // The amplitude envelope, in seconds and percent; a region without one keeps a new zone's.
        if (find (opcodes, { "ampeg_attack" }) != nullptr)
            fields->setProperty ("attackMs", std::clamp (number (opcodes, "ampeg_attack", 0) * 1000.0, 0.0, 10000.0));

        if (find (opcodes, { "ampeg_decay" }) != nullptr)
            fields->setProperty ("decayMs", std::clamp (number (opcodes, "ampeg_decay", 0) * 1000.0, 0.0, 10000.0));

        if (find (opcodes, { "ampeg_sustain" }) != nullptr)
            fields->setProperty ("sustain", std::clamp (number (opcodes, "ampeg_sustain", 100) / 100.0, 0.0, 1.0));

        if (find (opcodes, { "ampeg_release" }) != nullptr)
            fields->setProperty ("releaseMs", std::clamp (number (opcodes, "ampeg_release", 0) * 1000.0, 0.0, 10000.0));

        // Velocity crossfades (SF7), and how much velocity changes the level, for the whole Sampler.
        for (const auto& [opcode, field] :
             { std::pair { "xfin_lovel", "fadeInLowVelocity" }, std::pair { "xfin_hivel", "fadeInHighVelocity" },
               std::pair { "xfout_lovel", "fadeOutLowVelocity" }, std::pair { "xfout_hivel", "fadeOutHighVelocity" } })
            if (find (opcodes, { opcode }) != nullptr)
                fields->setProperty (field, std::clamp (static_cast<int> (number (opcodes, opcode, 0)), 0, 127));

        if (find (opcodes, { "amp_veltrack" }) != nullptr && patch.velocityTracking < 0.0)
            patch.velocityTracking = std::clamp (number (opcodes, "amp_veltrack", 100), 0.0, 100.0);

        const auto random = find (opcodes, { "lorand", "hirand" }) != nullptr;
        patch.randomTakes = patch.randomTakes || random;
        region.alternates = random || number (opcodes, "seq_length", 1) > 1;
        region.sequence = static_cast<int> (number (opcodes, "seq_position", 1));
        region.randomLow = number (opcodes, "lorand", 0.0);
        region.randomHigh = number (opcodes, "hirand", 1.0);

        if (const auto* group = find (opcodes, { "group" }))
            region.group = *group;

        if (const auto* offBy = find (opcodes, { "off_by" }))
            region.offBy = *offBy;

        regions.push_back (std::move (region));
    };

    for (const auto& line : juce::StringArray::fromLines (source))
        readLine (
            line,
            [&] (const juce::String& header)
            {
                finishRegion();
                current = levelOf (header);

                // A header starts its level over, and the levels under it.
                if (current != Level::other)
                    for (auto& [level, opcodes] : levels)
                        if (level >= current)
                            opcodes.clear();

                inRegion = current == Level::region;
            },
            [&] (const juce::String& name, const juce::String& value)
            {
                if (current == Level::control)
                {
                    if (name == "default_path")
                        defaultPath = value.replaceCharacter ('\\', '/');
                    else if (name == "note_offset")
                        keyOffset += value.getIntValue();
                    else if (name == "octave_offset")
                        keyOffset += 12 * value.getIntValue();
                    else if (! known (name))
                        patch.ignored.addIfNotAlreadyThere (ignoredName (name));

                    return;
                }

                if (current != Level::other)
                    levels[current][name] = value;
            });

    finishRegion();

    // Choke groups: the groups another region is cut off by, numbered as they appear (SF4).
    juce::StringArray chokes;

    for (const auto& region : regions)
        if (region.offBy.isNotEmpty() && chokes.size() < mostChokeGroups)
            chokes.addIfNotAlreadyThere (region.offBy);

    for (auto& region : regions)
        if (const auto index = chokes.indexOf (region.group); region.group.isNotEmpty() && index >= 0)
            region.fields->setProperty ("chokeGroup", index + 1);

    // Layers: each region in the lowest layer it overlaps nothing in; takes in the layer of the first of them (SF5).
    std::vector<std::vector<const Region*>> layers (mostLayers);

    for (auto& region : regions)
    {
        const auto overlaps = [&] (const Region* other)
        {
            return region.lowKey <= other->highKey && other->lowKey <= region.highKey
                   && region.lowVelocity <= other->highVelocity && other->lowVelocity <= region.highVelocity;
        };
        // Takes of each other: the same keys and velocities, each played at a different turn or random value; with the
        // same turn and value they play together, so they're layers.
        const auto sameTake = [&] (const Region* other)
        {
            const auto turns = region.sequence != other->sequence
                               || ! juce::exactlyEqual (region.randomLow, other->randomLow)
                               || ! juce::exactlyEqual (region.randomHigh, other->randomHigh);
            return region.alternates && other->alternates && turns && region.lowKey == other->lowKey
                   && region.highKey == other->highKey && region.lowVelocity == other->lowVelocity
                   && region.highVelocity == other->highVelocity;
        };

        auto chosen = mostLayers - 1;

        for (auto layer = 0; layer < mostLayers; ++layer)
        {
            const auto& placed = layers[static_cast<size_t> (layer)];

            if (std::ranges::none_of (placed,
                                      [&] (const Region* other) { return overlaps (other) && ! sameTake (other); }))
            {
                chosen = layer;
                break;
            }
        }

        layers[static_cast<size_t> (chosen)].push_back (&region);
        region.fields->setProperty ("layer", chosen + 1);
    }

    // Takes in seq_position order; the others keep theirs (a layer's other regions don't overlap, so their order
    // doesn't matter).
    std::ranges::stable_sort (regions, {}, [] (const Region& r) { return r.alternates ? r.sequence : 0; });

    for (auto& region : regions)
        patch.regions.push_back ({ region.sample, juce::var (region.fields.get()) });

    return patch;
}

SfzPatch readSfzFile (const juce::File& file)
{
    return readSfz (
        file.loadFileAsString(), file.getParentDirectory(),
        [] (const juce::File& include) { return include.existsAsFile() ? include.loadFileAsString() : juce::String(); },
        [] (const juce::File& sample) { return sample.existsAsFile(); });
}

} // namespace dawg
