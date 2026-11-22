#include "MainComponent.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/VarUtils.h"
#include "dawg/VoiceAllocator.h"

#include <juce_dsp/juce_dsp.h>

// Device panels (docs/design/phase11-faust-panels.md, P1 and P2): response curves on request, and the analysers of the
// node the dock shows, measured only while it shows them.

namespace dawg
{

namespace
{
constexpr int fftOrder = 13; // 8192 points: 5.9 Hz bins at 48 kHz
constexpr int fftSize = 1 << fftOrder;
constexpr int fftBins = fftSize / 2;
constexpr int spectrumBins = 256;
constexpr double spectrumTilt = 3.0;     // dB per octave about 1 kHz, so that pink noise reads flat
constexpr double spectrumAttack = 0.03;  // seconds: the averaging's time constant for rising bins
constexpr double spectrumRelease = 0.35; // seconds, for falling ones: about 12 dB per second
constexpr int scopePoints = 256;
constexpr int correlationFrames = 2048;

// The power of each FFT bin of the newest frames (Hann window; a full-scale sine's peak at 1) of the mid,
// (L + R) / 2, or with `side` the side, (L - R) / 2, averaged into `powers` over time: quickly while a bin rises and
// slowly while it falls, as power, so a bin falls at a steady rate in dB. `seconds` is the time since the last
// update (0 for the first).
void averageBinPowers (juce::dsp::FFT& fft, std::vector<float>& data, const std::vector<float>& left,
                       const std::vector<float>& right, bool side, std::vector<float>& powers, double seconds)
{
    const auto sign = side ? -1.0f : 1.0f;

    data.assign (2 * static_cast<size_t> (fftSize), 0.0f);

    for (size_t i = 0; i < static_cast<size_t> (fftSize); ++i)
    {
        const auto window = 0.5f
                            - 0.5f
                                  * std::cos (juce::MathConstants<float>::twoPi * static_cast<float> (i)
                                              / static_cast<float> (fftSize));
        data[i] = 0.5f * (left[i] + sign * right[i]) * window;
    }

    fft.performFrequencyOnlyForwardTransform (data.data(), true);

    // A Hann-windowed full-scale sine peaks at N / 4.
    const auto scale = 4.0f / static_cast<float> (fftSize);
    const auto fresh = powers.size() != static_cast<size_t> (fftBins);
    const auto rise = static_cast<float> (1.0 - std::exp (-seconds / spectrumAttack));
    const auto fall = static_cast<float> (1.0 - std::exp (-seconds / spectrumRelease));

    if (fresh)
        powers.assign (static_cast<size_t> (fftBins), 0.0f);

    for (size_t bin = 0; bin < static_cast<size_t> (fftBins); ++bin)
    {
        const auto power = data[bin] * scale * data[bin] * scale;
        auto& average = powers[bin];
        average = fresh ? power : average + (power - average) * (power > average ? rise : fall);
    }
}

// The spectrum in 256 log-spaced bands from 20 Hz to 20 kHz (the first and last at those frequencies), in dB with a
// full-scale sine at 0 dB at 1 kHz, tilted by spectrumTilt. A band at least two bins wide takes its loudest bin, so
// a tone reads at its level; a narrower one (the low end) is interpolated between bins, so the curve has no steps.
[[nodiscard]] juce::Array<juce::var> spectrumBands (const std::vector<float>& powers, double sampleRate)
{
    const auto binWidth = sampleRate / fftSize;
    const auto step = std::pow (1000.0, 1.0 / (spectrumBins - 1));
    const auto db = [&powers] (int bin)
    { return 10.0 * std::log10 (std::max (powers[static_cast<size_t> (std::clamp (bin, 1, fftBins - 1))], 1.0e-12f)); };
    juce::Array<juce::var> result;
    result.ensureStorageAllocated (spectrumBins);

    for (int band = 0; band < spectrumBins; ++band)
    {
        const auto centre = 20.0 * std::pow (step, band);
        const auto first = static_cast<int> (std::ceil (centre / std::sqrt (step) / binWidth));
        const auto last = std::min (static_cast<int> (std::floor (centre * std::sqrt (step) / binWidth)), fftBins - 1);
        double level = 0.0;

        if (last - first >= 1)
        {
            level = db (first);

            for (int bin = first + 1; bin <= last; ++bin)
                level = std::max (level, db (bin));
        }
        else
        {
            // Catmull-Rom through the four bins around the centre, in dB.
            const auto position = centre / binWidth;
            const auto k = static_cast<int> (std::floor (position));
            const auto t = position - k;
            const auto p0 = db (k - 1);
            const auto p1 = db (k);
            const auto p2 = db (k + 1);
            const auto p3 = db (k + 2);
            level =
                p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
        }

        level += spectrumTilt * std::log2 (centre / 1000.0);
        result.add (std::round (std::max (level, -120.0) * 10.0) / 10.0);
    }

    return result;
}

// The correlation of the last frames (1: mono, 0: unrelated, -1: out of phase), and their side-to-mid ratio.
[[nodiscard]] std::pair<double, double> correlationAndWidth (const std::vector<float>& left,
                                                             const std::vector<float>& right)
{
    double lr = 0.0;
    double ll = 0.0;
    double rr = 0.0;
    double mid = 0.0;
    double side = 0.0;

    for (auto i = left.size() - static_cast<size_t> (correlationFrames); i < left.size(); ++i)
    {
        const double l = left[i];
        const double r = right[i];
        lr += l * r;
        ll += l * l;
        rr += r * r;
        mid += (l + r) * (l + r);
        side += (l - r) * (l - r);
    }

    const auto energy = std::sqrt (ll * rr);
    return { energy > 1.0e-12 ? lr / energy : 1.0, mid > 1.0e-12 ? std::sqrt (side / mid) : 0.0 };
}

// The RMS level (dB) of the newest frames, one 30 Hz frame long, per channel: the Loop Reverb's live strip.
[[nodiscard]] juce::Array<juce::var> level (const std::vector<float>& left, const std::vector<float>& right,
                                            double sampleRate)
{
    const auto frames = std::min (left.size(), static_cast<size_t> (sampleRate / 30.0));
    juce::Array<juce::var> result;

    for (const auto* channel : { &left, &right })
    {
        double sum = 0.0;

        for (auto i = channel->size() - frames; i < channel->size(); ++i)
            sum += static_cast<double> ((*channel)[i]) * (*channel)[i];

        const auto db = 10.0 * std::log10 (sum / static_cast<double> (std::max<size_t> (frames, 1)) + 1.0e-12);
        result.add (std::round (db * 10.0) / 10.0);
    }

    return result;
}

// The newest frames as [l, r, l, r, ...], every fourth one, for the goniometer.
[[nodiscard]] juce::Array<juce::var> scope (const std::vector<float>& left, const std::vector<float>& right)
{
    juce::Array<juce::var> result;
    const auto start = left.size() - static_cast<size_t> (scopePoints * 4);

    for (auto i = start; i < left.size(); i += 4)
    {
        result.add (std::round (left[i] * 1000.0f) / 1000.0f);
        result.add (std::round (right[i] * 1000.0f) / 1000.0f);
    }

    return result;
}
} // namespace

void MainComponent::registerPanelHandlers()
{
    // A built-in effect's response curve for the given parameter values (P1).
    m_bridge.on ("nodes.response",
                 [this] (const juce::var& p)
                 {
                     std::map<juce::String, double> params;

                     if (const auto* values = p["params"].getDynamicObject())
                         for (const auto& property : values->getProperties())
                             params[property.name.toString()] = static_cast<double> (property.value);

                     return effectResponse (p["type"].toString(), params, currentSpec().sampleRate);
                 });

    // The nodes a window shows, whose analysers run (P2): { trackId, nodeId }, { nodes: [{ trackId, nodeId }] } for
    // the channel strip's several (docs/design/channel-strip.md, C6), or null to stop them. `window` names the window
    // asking ("main" when left out): each window keeps its own list, and the nodes of all of them run.
    m_bridge.on ("nodes.watch",
                 [this] (const juce::var& p)
                 {
                     std::vector<WatchedNode> nodes;
                     const auto add = [&nodes] (const juce::var& n)
                     {
                         if (n.isObject() && n.hasProperty ("trackId"))
                             nodes.emplace_back (static_cast<ObjectId> (n["trackId"]),
                                                 static_cast<ObjectId> (n["nodeId"]));
                     };

                     if (const auto* list = p["nodes"].getArray())
                         for (const auto& n : *list)
                             add (n);
                     else
                         add (p);

                     const auto window = p["window"].toString();
                     watchNodes (window.isEmpty() ? juce::String ("main") : window, std::move (nodes));
                     return juce::var();
                 });
}

void MainComponent::watchNodes (const juce::String& window, std::vector<WatchedNode> nodes)
{
    if (nodes.empty())
        m_watchesByWindow.erase (window);
    else
        m_watchesByWindow[window] = std::move (nodes);

    // The union, in the order the windows and their lists name the nodes.
    std::vector<WatchedNode> watched;

    for (const auto& [name, list] : m_watchesByWindow)
        for (const auto& node : list)
            if (std::ranges::find (watched, node) == watched.end())
                watched.push_back (node);

    // A node's tap stops only when no window watches it any more.
    for (const auto& previous : m_watchedNodes)
        if (std::ranges::find (watched, previous) == watched.end())
            if (std::ranges::find (nodes, previous) == nodes.end())
            {
                if (auto* node = m_compiler.findInstance (previous.first, previous.second))
                {
                    node->setTapEnabled (false);
                }
                else if (const auto state = m_spectra.find (previous);
                         state != m_spectra.end() && state->second.voice >= 0)
                {
                    // A node inside a Voice Instrument's voice graph: the tap is on the voice the panel showed.
                    if (auto* voiceNode =
                            m_compiler.findVoiceInstance (previous.first, previous.second, state->second.voice))
                        voiceNode->setTapEnabled (false);
                }

                m_spectra.erase (previous);
            }

    m_watchedNodes = std::move (watched);
}

void MainComponent::emitAnalysis()
{
    for (const auto& watched : m_watchedNodes)
        emitAnalysis (watched);
}

void MainComponent::emitAnalysis (const WatchedNode& watched)
{
    // A recompilation may have replaced the instance (a Faust script swap): tap whichever one runs now.
    auto* node = m_compiler.findInstance (watched.first, watched.second);
    auto& state = m_spectra[watched];

    if (node == nullptr)
    {
        // A node inside a Voice Instrument's voice graph has one instance per voice (phase12-voice-scope.md, V11).
        // Show the voice the panel showed last while it is modulated, else the first modulated voice, else the
        // first voice: a note's own filter is the one the player is looking at.
        Node* first = nullptr;
        int firstVoice = -1;
        Node* modulated = nullptr;
        int modulatedVoice = -1;
        Node* sticky = nullptr;

        for (int voice = 0; voice < static_cast<int> (VoiceAllocator::numVoices); ++voice)
        {
            auto* candidate = m_compiler.findVoiceInstance (watched.first, watched.second, voice);

            if (candidate == nullptr)
                continue;

            if (first == nullptr)
            {
                first = candidate;
                firstVoice = voice;
            }

            const auto meters = candidate->meters();

            if (meters.isObject() && static_cast<bool> (meters["isModulated"]))
            {
                if (voice == state.voice)
                {
                    sticky = candidate;
                    break;
                }

                if (modulated == nullptr)
                {
                    modulated = candidate;
                    modulatedVoice = voice;
                }
            }
        }

        int chosenVoice = -1;

        if (sticky != nullptr)
        {
            node = sticky;
            chosenVoice = state.voice;
        }
        else if (modulated != nullptr)
        {
            node = modulated;
            chosenVoice = modulatedVoice;
        }
        else
        {
            node = first;
            chosenVoice = firstVoice;
        }

        // Keep one tap recording: the voice shown last stops when the panel moves to another voice.
        if (state.voice >= 0 && state.voice != chosenVoice)
            if (auto* previous = m_compiler.findVoiceInstance (watched.first, watched.second, state.voice))
                previous->setTapEnabled (false);

        state.voice = chosenVoice;
    }
    else if (state.voice >= 0)
    {
        // A node moved from a voice graph to the track's graph (or a recompile): stop the old voice's tap.
        if (auto* previous = m_compiler.findVoiceInstance (watched.first, watched.second, state.voice))
            previous->setTapEnabled (false);

        state.voice = -1;
    }

    if (node == nullptr)
        return;

    if (node->activeTap() == nullptr)
        node->setTapEnabled (true);

    if (m_fft == nullptr)
        m_fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    m_analysisLeft.resize (fftSize);
    m_analysisRight.resize (fftSize);
    node->activeTap()->read (m_analysisLeft, m_analysisRight);

    // The time since this node's last frame, for the spectrum's averaging (the first frame starts from it).
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto seconds = state.updatedMs > 0.0 ? std::clamp ((now - state.updatedMs) / 1000.0, 0.0, 0.5) : 0.0;
    state.updatedMs = now;
    averageBinPowers (*m_fft, m_fftData, m_analysisLeft, m_analysisRight, false, state.powers, seconds);
    averageBinPowers (*m_fft, m_fftData, m_analysisLeft, m_analysisRight, true, state.sidePowers, seconds);

    const auto [correlation, width] = correlationAndWidth (m_analysisLeft, m_analysisRight);
    m_bridge.emit ("dawg_analysis",
                   makeObject ({ { "key", juce::String (watched.first) + ":" + juce::String (watched.second) },
                                 { "spectrum", spectrumBands (state.powers, currentSpec().sampleRate) },
                                 { "sideSpectrum", spectrumBands (state.sidePowers, currentSpec().sampleRate) },
                                 { "correlation", correlation },
                                 { "width", width },
                                 { "scope", scope (m_analysisLeft, m_analysisRight) },
                                 { "level", level (m_analysisLeft, m_analysisRight, currentSpec().sampleRate) },
                                 { "meters", node->panelMeters() } }));
}

} // namespace dawg
