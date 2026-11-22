#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/GraphCompiler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

// Output protection (docs/design/output-protection.md, S1-S4).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
// Audio tracks that monitor the device inputs (one track per input channel), so a test decides what they play.
struct ProtectionRig
{
    explicit ProtectionRig (int numTracks = 1)
    {
        for (int i = 0; i < numTracks; ++i)
        {
            project = apply (project, "addTrack", { { "kind", "audio" } });
            const auto id = project.tracks.back()->id;
            trackIds.push_back (id);
            project = apply (project, "setTrackSwitches", { { "id", id }, { "inputChannel", i }, { "monitor", 2 } });
        }

        engine.setEngine (compiler.compile (project, spec));
        std::ignore = renderInputs ({ 0.0f }); // offline once: the engine learns the sample rate
    }

    // One block rendered offline, each input channel holding one value (or the given samples).
    [[nodiscard]] StereoBuffer renderInputs (std::vector<float> values)
    {
        inputs.resize (values.size());

        for (size_t ch = 0; ch < values.size(); ++ch)
            inputs[ch].assign (blockSize, values[ch]);

        return renderBlock (false);
    }

    // One block through the device callback, which applies the device's protection (S2, S3).
    [[nodiscard]] StereoBuffer deviceBlock (float value)
    {
        inputs.assign (1, std::vector<float> (blockSize, value));
        return renderBlock (true);
    }

    [[nodiscard]] StereoBuffer renderBlock (bool device)
    {
        std::vector<const float*> pointers;
        pointers.reserve (inputs.size());

        for (const auto& channel : inputs)
            pointers.push_back (channel.data());

        StereoBuffer out (blockSize);

        if (device)
            engine.audioDeviceIOCallbackWithContext (pointers.data(), static_cast<int> (pointers.size()),
                                                     out.channels(), 2, blockSize, {});
        else
            engine.renderOffline (out.channels(), 2, blockSize, sampleRate, pointers.data(),
                                  static_cast<int> (pointers.size()));

        return out;
    }

    [[nodiscard]] int invalidBlocks (size_t track)
    {
        return compiler.trackState (trackIds[track])->invalidBlocks.exchange (0);
    }

    Project project;
    std::vector<ObjectId> trackIds;
    GraphCompiler compiler;
    AudioEngine engine;
    std::vector<std::vector<float>> inputs;
};

[[nodiscard]] bool allEqual (const std::vector<float>& samples, float value)
{
    return std::ranges::all_of (samples, [value] (float x) { return x == value; });
}
} // namespace

TEST_CASE ("a track that outputs NaN is silenced and reset, the others play on", "[protection]")
{
    ProtectionRig rig (2);
    std::ignore = rig.renderInputs ({ 0.25f, 0.5f });
    std::ignore = rig.invalidBlocks (0);

    // The first track's input turns NaN for a block: it plays nothing, the second track is heard alone.
    auto poisoned = rig.renderInputs ({ std::numeric_limits<float>::quiet_NaN(), 0.5f });
    CHECK (std::ranges::all_of (poisoned.left(), [] (float x) { return std::isfinite (x); }));
    CHECK_THAT (poisoned.left().back(), WithinAbs (0.5, 1e-4));
    CHECK (rig.invalidBlocks (0) == 1);
    CHECK (rig.invalidBlocks (1) == 0);

    // Its nodes were reset, so it recovers as soon as its input is valid again.
    const auto recovered = rig.renderInputs ({ 0.25f, 0.5f });
    CHECK_THAT (recovered.left().back(), WithinAbs (0.75, 1e-4));
    CHECK (rig.invalidBlocks (0) == 0);
}

TEST_CASE ("offline rendering is neither clipped nor muted", "[protection]")
{
    ProtectionRig rig;

    for (int block = 0; block < 40; ++block)
        std::ignore = rig.renderInputs ({ 20.0f });

    const auto out = rig.renderInputs ({ 20.0f });
    CHECK_THAT (out.left().back(), WithinAbs (20.0, 1e-3));
    CHECK (rig.engine.getTelemetry().outputMute == OutputMute::none);
}

TEST_CASE ("a sample beyond +24 dBFS mutes the device output until it is unmuted", "[protection]")
{
    ProtectionRig rig;
    std::ignore = rig.deviceBlock (0.5f);

    // Muted at once: the block fades out over 10 ms (480 samples), then silence.
    const auto first = rig.deviceBlock (20.0f);
    CHECK (rig.engine.getTelemetry().outputMute == OutputMute::extreme);
    CHECK (first.left()[0] <= 1.0f);
    CHECK (first.left().back() == 0.0f);
    CHECK (allEqual (rig.deviceBlock (0.5f).left(), 0.0f)); // stays muted though the signal is sane again

    // Unmuted: the output fades back in over 10 ms.
    rig.engine.unmuteOutput();
    const auto back = rig.deviceBlock (0.5f);
    CHECK (rig.engine.getTelemetry().outputMute == OutputMute::none);
    CHECK (back.left()[0] < 0.01f);
    CHECK_THAT (back.left()[240], WithinAbs (0.25, 0.01));
    CHECK_THAT (back.left().back(), WithinAbs (0.5, 1e-4));

    // Unmuting while the cause is still there mutes again at once.
    std::ignore = rig.deviceBlock (20.0f);
    rig.engine.unmuteOutput();
    CHECK (allEqual (rig.deviceBlock (20.0f).left(), 0.0f));
    CHECK (rig.engine.getTelemetry().outputMute == OutputMute::extreme);
}

TEST_CASE ("a sustained overload mutes the device output after 200 ms; the ceiling clips before", "[protection]")
{
    ProtectionRig rig;

    // 1.5 is above full scale: clipped to 1 at the device ...
    for (int block = 0; block < 18; ++block) // 18 * 512 samples = 192 ms
    {
        const auto out = rig.deviceBlock (1.5f);
        REQUIRE (rig.engine.getTelemetry().outputMute == OutputMute::none);
        CHECK (out.left().back() == 1.0f);
    }

    // ... and muted once it has stayed there for 200 ms.
    std::ignore = rig.deviceBlock (1.5f);
    CHECK (rig.engine.getTelemetry().outputMute == OutputMute::overload);
    CHECK (allEqual (rig.deviceBlock (1.5f).left(), 0.0f));
}

TEST_CASE ("a loud but sane signal never mutes and is not changed", "[protection]")
{
    ProtectionRig rig;
    rig.inputs.assign (1, std::vector<float> (blockSize));

    // A full-scale sine for two seconds, with a short peak above full scale now and then.
    for (int block = 0; block < 2 * static_cast<int> (sampleRate) / blockSize; ++block)
    {
        for (int s = 0; s < blockSize; ++s)
        {
            const auto t = static_cast<double> (block * blockSize + s) / sampleRate;
            rig.inputs[0][static_cast<size_t> (s)] =
                static_cast<float> (std::sin (2.0 * std::numbers::pi * 440.0 * t)) * (s == 100 ? 1.4f : 1.0f);
        }

        const auto out = rig.renderBlock (true);
        REQUIRE (rig.engine.getTelemetry().outputMute == OutputMute::none);
        CHECK_THAT (out.left()[200], WithinAbs (rig.inputs[0][200], 1e-4));
    }
}

TEST_CASE ("output protection does not allocate", "[protection][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    ProtectionRig rig;
    rig.inputs.assign (1, std::vector<float> (blockSize, std::numeric_limits<float>::quiet_NaN()));
    std::vector<const float*> pointers { rig.inputs[0].data() };
    StereoBuffer out (blockSize);
    const auto before = allocationViolationCount();

    {
        const ScopedNoAllocation guard;

        for (int block = 0; block < 4; ++block)
            rig.engine.audioDeviceIOCallbackWithContext (pointers.data(), 1, out.channels(), 2, blockSize, {});
    }

    CHECK (allocationViolationCount() == before);
}
