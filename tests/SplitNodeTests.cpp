#include "TestUtils.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/Crossover.h"
#include "dawg/GraphCompiler.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <functional>
#include <numbers>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Split nodes (docs/design/split-nodes.md, SP10).

namespace
{
// ---------------------------------------------------------------------------------------------------------
// The crossover bank on its own

// Each band's impulse response, and the dry port's, for crossovers at `hz`.
struct Responses
{
    std::vector<std::vector<float>> bands;
    std::vector<float> dry;
};

[[nodiscard]] Responses impulseResponses (std::vector<float> hz, double rate, size_t length = 1u << 16)
{
    MinimumPhaseCrossovers bank;
    bank.prepare (rate, 1);
    bank.setFrequencies (hz);

    Responses result;
    result.bands.assign (hz.size() + 1, std::vector<float> (length));
    result.dry.assign (length, 0.0f);
    std::vector<float> input (length, 0.0f);
    input[0] = 1.0f;

    std::vector<float*> destinations;

    for (auto& band : result.bands)
        destinations.push_back (band.data());

    bank.process (0, input.data(), destinations, result.dry.data(), static_cast<int> (length));
    return result;
}

// A response's level in dB at a frequency (a direct DFT).
[[nodiscard]] double levelDb (const std::vector<float>& response, double hz, double rate)
{
    std::complex<double> sum;
    const auto w = 2.0 * std::numbers::pi * hz / rate;

    for (size_t n = 0; n < response.size(); ++n)
        sum += static_cast<double> (response[n]) * std::polar (1.0, -w * static_cast<double> (n));

    return 20.0 * std::log10 (std::max (std::abs (sum), 1.0e-12));
}

[[nodiscard]] std::vector<float> sumOf (const std::vector<std::vector<float>>& bands)
{
    std::vector<float> sum (bands[0].size(), 0.0f);

    for (const auto& band : bands)
        for (size_t i = 0; i < sum.size(); ++i)
            sum[i] += band[i];

    return sum;
}

// ---------------------------------------------------------------------------------------------------------
// The nodes in a compiled graph

// Stereo noise, a different sequence on each side, the same on every run.
constexpr std::array<PortInfo, 1> stereoOut { { { PortType::audio, "Audio" } } };
const NodeDescriptor stereoNoiseDescriptor { "test.noise", "Noise", {}, stereoOut, {} };

class NoiseNode final : public Node
{
public:
    NoiseNode()
        : Node (stereoNoiseDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (int s = 0; s < context.numSamples; ++s)
            for (size_t ch = 0; ch < 2; ++ch)
                context.audioOutputs[0][ch][s] = next() * (ch == 0 ? 0.5f : 0.3f);
    }

private:
    float next() noexcept
    {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;
        return static_cast<float> (m_state) / 4294967296.0f * 2.0f - 1.0f;
    }

    uint32_t m_state = 2463534242u;
};

[[nodiscard]] NodeRegistry splitRegistry()
{
    NodeRegistry registry;
    registerBuiltinNodes (registry);
    registry.add (stereoNoiseDescriptor, [] { return std::make_unique<NoiseNode>(); });
    return registry;
}

[[nodiscard]] GraphNode node (ObjectId id, const char* type)
{
    GraphNode result;
    result.id = id;
    result.type = type;
    return result;
}

[[nodiscard]] Project projectWithGraph (TrackGraph graph)
{
    Track track;
    track.id = 1;
    track.graph = std::move (graph);
    Project project;
    project.tracks.push_back (std::make_shared<const Track> (std::move (track)));
    project.nextId = 1000;
    return project;
}

constexpr int renderLength = 8 * blockSize;

[[nodiscard]] StereoBuffer renderGraph (const TrackGraph& graph)
{
    static const auto registry = splitRegistry();
    GraphCompiler compiler (registry);
    AudioEngine engine;
    engine.setEngine (compiler.compile (projectWithGraph (graph), spec));
    return render (engine, renderLength);
}

// The largest difference between two renders, from a sample on.
[[nodiscard]] float difference (const StereoBuffer& a, const StereoBuffer& b, size_t from = 0)
{
    auto worst = 0.0f;

    for (auto i = from; i < a.left().size(); ++i)
        worst = std::max ({ worst, std::abs (a.left()[i] - b.left()[i]), std::abs (a.right()[i] - b.right()[i]) });

    return worst;
}

// Noise into the track output, for reference.
[[nodiscard]] StereoBuffer plainNoise()
{
    TrackGraph graph;
    graph.nodes = { node (1, "test.noise"), node (9, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 9, 0 } };
    return renderGraph (graph);
}
} // namespace

TEST_CASE ("crossover bands add up to an all-pass, flat at every frequency", "[split][crossover]")
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const auto& hz :
             { std::vector { 1000.0f }, std::vector { 200.0f, 2500.0f }, std::vector { 120.0f, 1000.0f, 6000.0f } })
        {
            INFO ("rate " << rate << ", bands " << hz.size() + 1);
            const auto responses = impulseResponses (hz, rate);
            const auto sum = sumOf (responses.bands);

            for (auto f = 20.0; f <= 20000.0; f *= 1.25)
                CHECK (std::abs (levelDb (sum, f, rate)) < 0.01);

            // The Dry port is the same all-pass (SP3).
            for (size_t i = 0; i < sum.size(); ++i)
                REQUIRE_THAT (sum[i], WithinAbs (responses.dry[i], 1.0e-5));
        }
    }
}

TEST_CASE ("each band is -6 dB at its crossover and far down two octaves into its neighbour", "[split][crossover]")
{
    const auto rate = 48000.0;
    const auto responses = impulseResponses ({ 200.0f, 2500.0f }, rate);
    const auto& [low, mid, high] = std::tie (responses.bands[0], responses.bands[1], responses.bands[2]);

    CHECK_THAT (levelDb (low, 200.0, rate), WithinAbs (-6.02, 0.1));
    CHECK_THAT (levelDb (mid, 200.0, rate), WithinAbs (-6.02, 0.1));
    CHECK_THAT (levelDb (mid, 2500.0, rate), WithinAbs (-6.02, 0.1));
    CHECK_THAT (levelDb (high, 2500.0, rate), WithinAbs (-6.02, 0.1));

    CHECK (levelDb (low, 800.0, rate) < -40.0);
    CHECK (levelDb (mid, 50.0, rate) < -40.0);
    CHECK (levelDb (mid, 10000.0, rate) < -40.0);
    CHECK (levelDb (high, 625.0, rate) < -40.0);
}

TEST_CASE ("a crossover swept across a sine doesn't click", "[split][crossover]")
{
    const auto rate = 48000.0;
    constexpr int length = 48000;
    constexpr int chunk = 16;
    MinimumPhaseCrossovers bank;
    bank.prepare (rate, 1);

    std::vector<float> input (length);
    std::vector<float> low (length);
    std::vector<float> high (length);
    std::vector<float> dry (length);

    for (int i = 0; i < length; ++i)
        input[static_cast<size_t> (i)] =
            0.5f * static_cast<float> (std::sin (2.0 * std::numbers::pi * 1000.0 * i / rate));

    for (int start = 0; start < length; start += chunk)
    {
        const std::array frequency { static_cast<float> (200.0
                                                         * std::pow (25.0, static_cast<double> (start) / length)) };
        bank.setFrequencies (frequency);
        const std::array destinations { low.data() + start, high.data() + start };
        bank.process (0, input.data() + start, destinations, dry.data() + start, chunk);
    }

    const auto steepest = [] (const std::vector<float>& signal)
    {
        auto most = 0.0f;

        for (size_t i = 1; i < signal.size(); ++i)
            most = std::max (most, std::abs (signal[i] - signal[i - 1]));

        return most;
    };

    // A sine's own sample-to-sample change is at most 2 pi f / rate times its level.
    const auto sine = steepest (input);
    CHECK (steepest (low) < 1.5f * sine);
    CHECK (steepest (high) < 1.5f * sine);
}

TEST_CASE ("a Band Split's bands, summed by a Mix node, are its Dry port", "[split][band]")
{
    TrackGraph bands;
    bands.nodes = { node (1, "test.noise"), node (2, nodetype::bandSplit), node (3, nodetype::mix),
                    node (9, nodetype::trackOutput) };
    bands.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 2, 1, 3, 1 }, { 2, 2, 3, 2 }, { 3, 0, 9, 0 } };

    auto dry = bands;
    dry.nodes = { node (1, "test.noise"), node (2, nodetype::bandSplit), node (9, nodetype::trackOutput) };
    dry.connections = { { 1, 0, 2, 0 }, { 2, 4, 9, 0 } };

    const auto summed = renderGraph (bands);
    CHECK (difference (summed, renderGraph (dry)) < 1.0e-4f);
    CHECK (difference (summed, plainNoise()) > 1.0e-2f); // an all-pass, not the input itself

    SECTION ("Solo leaves one band")
    {
        auto solo = bands;
        solo.nodes[1].params["solo"] = 2;
        auto one = bands;
        one.connections = { { 1, 0, 2, 0 }, { 2, 1, 9, 0 } };
        CHECK (difference (renderGraph (solo), renderGraph (one)) < 1.0e-6f);
    }

    SECTION ("with two bands, the third port is silent and two add up")
    {
        bands.nodes[1].bands = 2;
        bands.nodes[1].params["crossover1"] = 1000.0;
        dry.nodes[1].bands = 2;
        dry.nodes[1].params["crossover1"] = 1000.0;
        CHECK (difference (renderGraph (bands), renderGraph (dry)) < 1.0e-4f);

        auto third = bands;
        third.connections = { { 1, 0, 2, 0 }, { 2, 2, 9, 0 } };
        CHECK (difference (renderGraph (third), StereoBuffer (renderLength)) == 0.0f);
    }
}

TEST_CASE ("a Stereo Split then Merge returns the input, in both modes", "[split][stereo]")
{
    for (const auto mode : { StereoMode::midSide, StereoMode::leftRight })
    {
        TrackGraph graph;
        graph.nodes = { node (1, "test.noise"), node (2, nodetype::stereoSplit), node (3, nodetype::stereoMerge),
                        node (9, nodetype::trackOutput) };
        graph.nodes[1].stereoMode = mode;
        graph.nodes[2].stereoMode = mode;
        graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 2, 1, 3, 1 }, { 3, 0, 9, 0 } };
        CHECK (difference (renderGraph (graph), plainNoise()) < 1.0e-6f);
    }
}

TEST_CASE ("a Stereo Split's parts and a Merge's gains", "[split][stereo]")
{
    const auto noise = plainNoise();
    TrackGraph graph;
    graph.nodes = { node (1, "test.noise"), node (2, nodetype::stereoSplit), node (3, nodetype::stereoMerge),
                    node (9, nodetype::trackOutput) };

    SECTION ("left/right: each channel on its own port, heard on both sides")
    {
        graph.nodes[1].stereoMode = StereoMode::leftRight;
        graph.connections = { { 1, 0, 2, 0 }, { 2, 1, 9, 0 } };
        const auto right = renderGraph (graph);

        for (size_t i = 0; i < noise.right().size(); i += 97)
        {
            REQUIRE_THAT (right.left()[i], WithinAbs (noise.right()[i], 1.0e-6));
            REQUIRE_THAT (right.right()[i], WithinAbs (noise.right()[i], 1.0e-6));
        }
    }

    SECTION ("mid/side gains make the merge a width control")
    {
        graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 2, 1, 3, 1 }, { 3, 0, 9, 0 } };
        graph.nodes[2].params["gain1"] = 20.0 * std::log10 (0.5);
        graph.nodes[2].params["gain2"] = 20.0 * std::log10 (2.0);
        const auto merged = renderGraph (graph);

        for (size_t i = 4 * blockSize; i < noise.left().size(); i += 97)
        {
            const auto mid = 0.5f * (noise.left()[i] + noise.right()[i]);
            const auto side = 0.5f * (noise.left()[i] - noise.right()[i]);
            REQUIRE_THAT (merged.left()[i], WithinAbs (0.5f * mid + 2.0f * side, 1.0e-5));
            REQUIRE_THAT (merged.right()[i], WithinAbs (0.5f * mid - 2.0f * side, 1.0e-5));
        }
    }

    SECTION ("left/right gains balance")
    {
        graph.nodes[1].stereoMode = StereoMode::leftRight;
        graph.nodes[2].stereoMode = StereoMode::leftRight;
        graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 2, 1, 3, 1 }, { 3, 0, 9, 0 } };
        graph.nodes[2].params["gain2"] = -96.0;
        const auto merged = renderGraph (graph);

        for (size_t i = 4 * blockSize; i < noise.left().size(); i += 97)
        {
            REQUIRE_THAT (merged.left()[i], WithinAbs (noise.left()[i], 1.0e-6));
            REQUIRE_THAT (merged.right()[i], WithinAbs (0.0, 1.0e-6));
        }
    }

    SECTION ("a stereo cable into a Merge's mono input is folded")
    {
        graph.connections = { { 1, 0, 3, 0 }, { 3, 0, 9, 0 } };
        const auto merged = renderGraph (graph);

        for (size_t i = 4 * blockSize; i < noise.left().size(); i += 97)
            REQUIRE_THAT (merged.left()[i], WithinAbs (0.5f * (noise.left()[i] + noise.right()[i]), 1.0e-6));
    }
}

TEST_CASE ("the band count and the stereo mode are settings with commands", "[split][commands]")
{
    TrackGraph graph;
    graph.nodes = { node (1, nodetype::gain), node (2, nodetype::bandSplit), node (3, nodetype::mix),
                    node (4, nodetype::stereoSplit), node (9, nodetype::trackOutput) };
    graph.connections = {
        { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 2, 1, 3, 1 }, { 2, 2, 3, 2 }, { 2, 4, 3, 3 }, { 3, 0, 9, 0 }
    };
    auto project = projectWithGraph (graph);

    project = apply (project, "setBandCount", { { "trackId", 1 }, { "nodeId", 2 }, { "bands", 2 } });
    const auto& split = project.tracks[0]->graph.nodes[1];
    CHECK (split.bands == 2);
    CHECK_THAT (split.params.at ("crossover1"), WithinAbs (1000.0, 1.0e-9)); // untouched: the new count's default
    CHECK (project.tracks[0]->graph.connections.size() == 5); // the third band's cable went, the Dry port's stayed
    CHECK_THROWS (apply (project, "setBandCount", { { "trackId", 1 }, { "nodeId", 2 }, { "bands", 5 } }));

    // Crossovers moved by hand stay, and those in use are put back in order.
    auto moved = apply (project, "setNodeParam",
                        { { "trackId", 1 }, { "nodeId", 2 }, { "param", "crossover1" }, { "value", 9000.0 } });
    moved = apply (moved, "setBandCount", { { "trackId", 1 }, { "nodeId", 2 }, { "bands", 4 } });
    const auto& params = moved.tracks[0]->graph.nodes[1].params;
    CHECK_THAT (params.at ("crossover1"), WithinAbs (9000.0, 1.0e-6));
    CHECK (params.at ("crossover2") >= 9000.0 * std::cbrt (2.0) - 1.0e-6);
    CHECK (params.at ("crossover3") >= params.at ("crossover2") * std::cbrt (2.0) - 1.0e-6);
    CHECK_THROWS (apply (project, "setBandCount", { { "trackId", 1 }, { "nodeId", 3 }, { "bands", 2 } }));

    project = apply (project, "setStereoMode", { { "trackId", 1 }, { "nodeId", 4 }, { "mode", "leftRight" } });
    CHECK (project.tracks[0]->graph.nodes[3].stereoMode == StereoMode::leftRight);
    CHECK_THROWS (apply (project, "setStereoMode", { { "trackId", 1 }, { "nodeId", 2 }, { "mode", "leftRight" } }));

    // Both are saved when not the default.
    const juce::TemporaryFile temporary (juce::String ("Splits") + projectfile::extension);
    REQUIRE (projectfile::write (project, { "id", "v" }, temporary.getFile(), false).isEmpty());
    const auto loaded = projectfile::read (temporary.getFile()).project;
    CHECK (loaded.tracks[0]->graph.nodes[1].bands == 2);
    CHECK (loaded.tracks[0]->graph.nodes[3].stereoMode == StereoMode::leftRight);
}

// ---------------------------------------------------------------------------------------------------------
// Linear phase (SP11)

namespace
{
// Runs a linear-phase bank over a signal in 16-sample pieces, as the node does: the bands and the Dry port.
Responses runLinear (LinearPhaseCrossovers& bank, const std::vector<float>& input, size_t bands,
                     const std::function<std::vector<float> (size_t)>& frequenciesAt)
{
    constexpr size_t chunk = 16;
    Responses result;
    result.bands.assign (bands, std::vector<float> (input.size()));
    result.dry.assign (input.size(), 0.0f);

    for (size_t start = 0; start < input.size(); start += chunk)
    {
        const auto hz = frequenciesAt (start);
        bank.setFrequencies (hz);
        std::vector<float*> destinations;

        for (auto& band : result.bands)
            destinations.push_back (band.data() + start);

        bank.process (0, input.data() + start, destinations, result.dry.data() + start,
                      static_cast<int> (std::min (chunk, input.size() - start)));
    }

    return result;
}

[[nodiscard]] std::vector<float> testNoise (size_t length)
{
    std::vector<float> result (length);
    uint32_t state = 2463534242u;

    for (auto& sample : result)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        sample = static_cast<float> (state) / 4294967296.0f - 0.5f;
    }

    return result;
}

[[nodiscard]] float peakOf (const std::vector<float>& signal, size_t from, size_t to)
{
    auto most = 0.0f;

    for (auto i = from; i < to; ++i)
        most = std::max (most, std::abs (signal[i]));

    return most;
}
} // namespace

TEST_CASE ("linear-phase bands add up to the input delayed by the latency", "[split][crossover][linear]")
{
    const auto rate = 48000.0;
    LinearPhaseCrossovers bank;
    bank.prepare (rate, 1);
    const std::vector<float> hz { 200.0f, 2500.0f, 6000.0f };
    bank.setInitial (hz);

    const auto latency = static_cast<size_t> (bank.latency());
    CHECK (latency == static_cast<size_t> (LinearPhaseCrossovers::latencyFor (rate)));
    CHECK (latency < static_cast<size_t> (0.15 * rate));

    const auto input = testNoise (latency + 20000);
    const auto result = runLinear (bank, input, 4, [&] (size_t) { return hz; });
    const auto sum = sumOf (result.bands);
    auto worstSum = 0.0f;
    auto worstDry = 0.0f;

    for (auto i = latency; i < input.size(); ++i)
    {
        worstSum = std::max (worstSum, std::abs (sum[i] - input[i - latency]));
        worstDry = std::max (worstDry, std::abs (result.dry[i] - input[i - latency]));
    }

    CHECK (worstSum < 1.0e-5f);
    CHECK (worstDry == 0.0f);
}

TEST_CASE ("linear-phase bands are symmetric band-passes with the Linkwitz-Riley slopes", "[split][crossover][linear]")
{
    const auto rate = 48000.0;
    LinearPhaseCrossovers bank;
    bank.prepare (rate, 1);
    const std::vector<float> hz { 200.0f, 2500.0f };
    bank.setInitial (hz);
    const auto latency = static_cast<size_t> (bank.latency());

    std::vector<float> impulse (2 * latency + 1024, 0.0f);
    impulse[0] = 1.0f;
    const auto result = runLinear (bank, impulse, 3, [&] (size_t) { return hz; });

    // Linear phase: each band's impulse response is symmetric around the latency.
    for (const auto& band : result.bands)
    {
        auto worst = 0.0f;

        for (size_t n = 1; n < latency - LinearPhaseCrossovers::blockSize; ++n)
            worst = std::max (worst, std::abs (band[latency - n] - band[latency + n]));

        CHECK (worst < 1.0e-6f);
    }

    const auto& [low, mid, high] = std::tie (result.bands[0], result.bands[1], result.bands[2]);
    CHECK_THAT (levelDb (low, 200.0, rate), WithinAbs (-6.02, 0.3));
    CHECK_THAT (levelDb (mid, 2500.0, rate), WithinAbs (-6.02, 0.3));
    CHECK_THAT (levelDb (high, 2500.0, rate), WithinAbs (-6.02, 0.3));
    CHECK_THAT (levelDb (low, 50.0, rate), WithinAbs (0.0, 0.1));
    CHECK (levelDb (low, 800.0, rate) < -40.0);
    CHECK (levelDb (mid, 10000.0, rate) < -40.0);
    CHECK (levelDb (high, 625.0, rate) < -40.0);
}

TEST_CASE ("a moved linear-phase crossover crossfades without a click", "[split][crossover][linear]")
{
    const auto rate = 48000.0;
    LinearPhaseCrossovers bank;
    bank.prepare (rate, 1);
    bank.setInitial (std::vector<float> { 200.0f });

    // A 400 Hz sine: above the crossover first, so the low band is quiet, then below it.
    const auto latency = static_cast<size_t> (bank.latency());
    const auto length = latency + 48000;
    std::vector<float> input (length);

    for (size_t i = 0; i < length; ++i)
        input[i] =
            0.5f * static_cast<float> (std::sin (2.0 * std::numbers::pi * 400.0 * static_cast<double> (i) / rate));

    const auto moveAt = latency + 8000;
    auto settled = false;
    const auto result = runLinear (bank, input, 2,
                                   [&] (size_t at)
                                   {
                                       const std::vector<float> moved { 2000.0f };

                                       if (at < moveAt)
                                           return std::vector<float> { 200.0f };

                                       if (! settled)
                                       {
                                           bank.setFrequencies (moved);
                                           settled = bank.settle();
                                       }

                                       return moved;
                                   });
    REQUIRE (settled);

    const auto& low = result.bands[0];
    auto steepest = 0.0f;

    for (auto i = latency + 1; i < length; ++i)
        steepest = std::max (steepest, std::abs (low[i] - low[i - 1]));

    // The sine's own steepest step is 2 pi 400 / 48000 times 0.5, about 0.026.
    CHECK (steepest < 0.03f);
    CHECK (peakOf (low, length - 4800, length) > 0.45f);
    CHECK (peakOf (low, moveAt - 4800, moveAt) < 0.1f);
}

TEST_CASE ("a linear-phase Band Split reports its latency and its bands add up to its Dry port",
           "[split][band][linear]")
{
    TrackGraph bands;
    bands.nodes = { node (1, "test.noise"), node (2, nodetype::bandSplit), node (3, nodetype::mix),
                    node (9, nodetype::trackOutput) };
    bands.nodes[1].params["phase"] = 1.0;
    bands.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 2, 1, 3, 1 }, { 2, 2, 3, 2 }, { 3, 0, 9, 0 } };

    auto dry = bands;
    dry.nodes = { node (1, "test.noise"), node (2, nodetype::bandSplit), node (9, nodetype::trackOutput) };
    dry.nodes[1].params["phase"] = 1.0;
    dry.connections = { { 1, 0, 2, 0 }, { 2, 4, 9, 0 } };

    static const auto registry = splitRegistry();
    const auto latency = static_cast<size_t> (LinearPhaseCrossovers::latencyFor (sampleRate));
    constexpr int length = 12000;

    {
        GraphCompiler compiler (registry);
        std::ignore = compiler.compile (projectWithGraph (bands), spec);
        const auto* split = compiler.findInstance (1, 2);
        REQUIRE (split != nullptr);
        CHECK (split->latencySamples() == static_cast<int> (latency));
    }

    const auto renderOf = [] (const TrackGraph& graph)
    {
        GraphCompiler own (registry);
        AudioEngine engine;
        engine.setEngine (own.compile (projectWithGraph (graph), spec));
        return render (engine, length);
    };

    const auto summed = renderOf (bands);
    const auto through = renderOf (dry);
    CHECK (difference (summed, through) < 1.0e-4f);

    // The Dry port is the noise, delayed by the latency.
    TrackGraph plainGraph;
    plainGraph.nodes = { node (1, "test.noise"), node (9, nodetype::trackOutput) };
    plainGraph.connections = { { 1, 0, 9, 0 } };
    const auto plain = renderOf (plainGraph);
    auto worst = 0.0f;

    for (auto i = latency; i < static_cast<size_t> (length); ++i)
        worst = std::max (worst, std::abs (through.left()[i] - plain.left()[i - latency]));

    CHECK (worst < 1.0e-6f);
}
