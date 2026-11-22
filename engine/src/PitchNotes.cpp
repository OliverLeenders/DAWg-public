#include "dawg/PitchNotes.h"

#include "dawg/PitchAnalysis.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace dawg
{

namespace
{
constexpr double rampSeconds = 0.01;          // the correction fades in and out over 10 ms at a note's voiced edges
constexpr double phraseGapSeconds = 0.1;      // notes closer than this render together (section 5.2)
constexpr double audibleSemitones = 0.005;    // half a cent: below this a phrase plays the file
constexpr double vibratoSlowSeconds = 0.15;   // the detail's slow part, removed to leave the vibrato
constexpr double vibratoJitterSeconds = 0.04; // the detail's fast part, removed too

[[nodiscard]] bool sameCurve (const Curve& a, const Curve& b) noexcept
{
    // Exact comparison is intended: an edit changed the curve or not.
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    return std::ranges::equal (a, b,
                               [] (const CurvePoint& p, const CurvePoint& q)
                               {
                                   return p.time == q.time && p.value == q.value && p.shape == q.shape
                                          && p.tension == q.tension && p.handleTime == q.handleTime
                                          && p.handleValue == q.handleValue;
                               });
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
}

// A moving average over `half` values either side, shrinking at the ends.
[[nodiscard]] std::vector<double> averaged (const std::vector<double>& v, int half)
{
    std::vector<double> out (v.size());

    for (size_t i = 0; i < v.size(); ++i)
    {
        const auto from = static_cast<size_t> (std::max<std::ptrdiff_t> (0, static_cast<std::ptrdiff_t> (i) - half));
        const auto to = std::min (v.size(), i + static_cast<size_t> (half) + 1);
        auto sum = 0.0;

        for (auto j = from; j < to; ++j)
            sum += v[j];

        out[i] = sum / static_cast<double> (to - from);
    }

    return out;
}

// One note's correction, evaluated anywhere inside it (section 4.3).
class NoteCorrection
{
public:
    NoteCorrection (const PitchNote& note, double sampleRate, const PitchTrack* track)
        : m_note (note),
          m_shape (note.shape)
    {
        if (track == nullptr || track->size() == 0 || std::abs (note.vibrato - 1.0) < 1.0e-9)
            return;

        // The vibrato: the detail (what the detected shape doesn't hold) without its slow part and its jitter, on
        // the track's frames inside the note.
        m_first = static_cast<std::ptrdiff_t> (std::ceil (track->indexOf (note.start)));
        const auto last = static_cast<std::ptrdiff_t> (std::floor (track->indexOf (note.end - 1)));
        m_track = track;

        if (last < m_first || m_first < 0 || static_cast<size_t> (last) >= track->size())
            return;

        std::vector<double> detail;

        for (auto i = m_first; i <= last; ++i)
        {
            const auto index = static_cast<size_t> (i);
            const auto frame = static_cast<double> (track->audioFrameOf (static_cast<double> (i)));
            const auto shape = evaluateCurve (note.detectedShape, frame - static_cast<double> (note.start), 0.0);
            detail.push_back (track->voiced (index) ? track->pitch[index] - note.detectedCentre - shape : 0.0);
        }

        const auto hop = track->hopSeconds;
        const auto slow = averaged (detail, static_cast<int> (std::lround (vibratoSlowSeconds / hop / 2.0)));

        for (size_t i = 0; i < detail.size(); ++i)
            detail[i] -= slow[i];

        m_vibrato = averaged (detail, std::max (1, static_cast<int> (std::lround (vibratoJitterSeconds / hop / 2.0))));
        juce::ignoreUnused (sampleRate);
    }

    // The correction at a file frame inside the note, without its edge ramps.
    [[nodiscard]] double at (double frame) const
    {
        const auto t = frame - static_cast<double> (m_note.start);
        auto delta = m_note.targetCentre - m_note.detectedCentre + evaluateCurve (m_shape, t, 0.0)
                     - evaluateCurve (m_note.detectedShape, t, 0.0);

        if (! m_vibrato.empty())
        {
            const auto index = m_track->indexOf (static_cast<juce::int64> (frame)) - static_cast<double> (m_first);
            const auto i = static_cast<std::ptrdiff_t> (std::floor (index));

            if (i >= 0 && static_cast<size_t> (i) < m_vibrato.size())
            {
                const auto j = std::min (static_cast<size_t> (i) + 1, m_vibrato.size() - 1);
                const auto f = index - static_cast<double> (i);
                const auto vibrato = m_vibrato[static_cast<size_t> (i)] * (1.0 - f) + m_vibrato[j] * f;
                delta += vibrato * (m_note.vibrato - 1.0);
            }
        }

        return delta;
    }

private:
    const PitchNote& m_note;
    Curve m_shape;
    const PitchTrack* m_track = nullptr;
    std::ptrdiff_t m_first = 0;
    std::vector<double> m_vibrato;
};
} // namespace

bool PitchNote::untouched() const noexcept
{
    // Exact comparison is intended: an edit changed the centre or the vibrato or not.
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    return targetCentre == detectedCentre && vibrato == 1.0 && sameCurve (shape, detectedShape);
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
}

void PitchNote::recentre()
{
    const auto length = static_cast<double> (end - start);

    if (length <= 0.0 || shape.empty())
        return;

    // Sampled evenly over the middle 70 %, as PitchAnalysis' centreOf leaves out 15 % at each end.
    constexpr int samples = 64;
    auto sum = 0.0;

    for (int k = 0; k < samples; ++k)
    {
        const auto time = length * (0.15 + 0.7 * (k + 0.5) / samples);
        sum += evaluateCurve (shape, time, 0.0) - evaluateCurve (detectedShape, time, 0.0);
    }

    auto by = sum / samples;
    by = std::clamp (targetCentre + by, 0.0, 127.0) - targetCentre;

    if (std::abs (by) < 1.0e-9)
        return;

    targetCentre += by;

    for (auto& point : shape)
        point.value -= by;
}

bool FilePitch::edited() const noexcept
{
    return std::ranges::any_of (notes, [] (const PitchNote& note) { return ! note.untouched(); });
}

bool FilePitch::soundsLike (const FilePitch& other) const noexcept
{
    // Exact comparison is intended: an edit changed a value or not.
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    return std::ranges::equal (notes, other.notes,
                               [] (const PitchNote& a, const PitchNote& b)
                               {
                                   return a.leadStart == b.leadStart && a.start == b.start && a.end == b.end
                                          && a.detectedCentre == b.detectedCentre && a.targetCentre == b.targetCentre
                                          && a.vibrato == b.vibrato && a.transitionMs == b.transitionMs
                                          && sameCurve (a.detectedShape, b.detectedShape)
                                          && sameCurve (a.shape, b.shape);
                               });
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
}

const PitchNote* FilePitch::find (juce::int64 id) const noexcept
{
    const auto it = std::ranges::find (notes, id, &PitchNote::id);
    return it != notes.end() ? &*it : nullptr;
}

std::vector<float> pitchCorrection (const FilePitch& pitch, double sampleRate, juce::int64 start, size_t count, int hop,
                                    const PitchTrack* track)
{
    std::vector<float> result (count, 0.0f);
    const auto& notes = pitch.notes;

    if (notes.empty() || count == 0)
        return result;

    // Built on first use: a phrase's range touches only its own notes, and building one filters its vibrato.
    std::vector<std::optional<NoteCorrection>> built (notes.size());
    const auto correction = [&] (size_t i) -> const NoteCorrection&
    {
        if (! built[i])
            built[i].emplace (notes[i], sampleRate, track);

        return *built[i];
    };

    // Whether note i's end meets note i + 1's start closely enough to ease between them rather than ramp to 0.
    const auto widthOf = [&] (size_t i)
    {
        const auto gap = static_cast<double> (notes[i + 1].start - notes[i].end);
        return std::max (notes[i + 1].transitionMs * sampleRate / 1000.0, gap);
    };
    const auto joined = [&] (size_t i)
    {
        return i + 1 < notes.size()
               && static_cast<double> (notes[i + 1].start - notes[i].end)
                      <= notes[i + 1].transitionMs * sampleRate / 1000.0;
    };
    const auto ramp = rampSeconds * sampleRate;

    for (size_t k = 0; k < count; ++k)
    {
        const auto frame = static_cast<double> (start + static_cast<juce::int64> (k) * hop);

        // The last note starting at or before this frame (or the first, before them all).
        const auto next = std::ranges::upper_bound (notes, frame, {},
                                                    [] (const PitchNote& n) { return static_cast<double> (n.start); });
        auto i = static_cast<size_t> (std::max<std::ptrdiff_t> (0, std::distance (notes.begin(), next) - 1));

        // Easing across a join: centred between the notes, as wide as transitionMs or the gap.
        auto eased = false;

        for (auto j : { i, i - 1 })
        {
            if (j >= notes.size() || ! joined (j))
                continue;

            const auto middle = (static_cast<double> (notes[j].end) + static_cast<double> (notes[j + 1].start)) / 2.0;
            const auto width = widthOf (j);

            if (std::abs (frame - middle) < width / 2.0)
            {
                const auto x = (frame - (middle - width / 2.0)) / width;
                const auto s = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * x);
                const auto a = correction (j).at (std::min (frame, static_cast<double> (notes[j].end - 1)));
                const auto b = correction (j + 1).at (std::max (frame, static_cast<double> (notes[j + 1].start)));
                result[k] = static_cast<float> (a + (b - a) * s);
                eased = true;
                break;
            }
        }

        if (eased)
            continue;

        const auto& note = notes[i];

        if (frame < static_cast<double> (note.start) || frame >= static_cast<double> (note.end))
            continue;

        const auto in =
            i > 0 && joined (i - 1) ? 1.0 : std::clamp ((frame - static_cast<double> (note.start)) / ramp, 0.0, 1.0);
        const auto out = joined (i) ? 1.0 : std::clamp ((static_cast<double> (note.end) - frame) / ramp, 0.0, 1.0);
        result[k] = static_cast<float> (correction (i).at (frame) * std::min (in, out));
    }

    return result;
}

std::vector<PitchPhrase> pitchPhrases (const FilePitch& pitch, double sampleRate, const PitchTrack* track)
{
    std::vector<PitchPhrase> phrases;
    const auto& notes = pitch.notes;
    const auto gap = static_cast<juce::int64> (phraseGapSeconds * sampleRate);
    const auto hop = std::max (1, static_cast<int> (sampleRate / 1000.0)); // checked every millisecond

    for (size_t first = 0; first < notes.size();)
    {
        auto last = first;

        while (last + 1 < notes.size() && notes[last + 1].leadStart - notes[last].end < gap)
            ++last;

        const auto chain = std::span (notes).subspan (first, last - first + 1);

        if (std::ranges::any_of (chain, [] (const PitchNote& n) { return ! n.untouched(); }))
        {
            const PitchPhrase phrase { notes[first].leadStart, notes[last].end };
            const auto values =
                pitchCorrection (pitch, sampleRate, phrase.start,
                                 static_cast<size_t> ((phrase.end - phrase.start) / hop + 1), hop, track);

            if (std::ranges::any_of (values, [] (float v) { return std::abs (v) > audibleSemitones; }))
                phrases.push_back (phrase);
        }

        first = last + 1;
    }

    return phrases;
}

} // namespace dawg
