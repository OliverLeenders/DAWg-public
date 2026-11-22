#include "TestUtils.h"
#include "dawg/DrumSynth.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Sequence.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <vector>

// What a busy Drum Synth pattern costs (docs/design/drum-synth.md, step 7). Hidden: run with [.drum-bench], in a Release
// build. The default kit with its cymbals and toms long, playing sixteenth hats with an open hat each beat, kick and
// snare, eighth rides, a crash each bar and a tom fill at each bar's end, for 20 seconds in 10 ms blocks.

using namespace dawg;
using namespace dawg::test;

TEST_CASE ("Drum Synth: a busy pattern's cost per block", "[.drum-bench]")
{
    constexpr int block = 480;      // 10 ms at 48 kHz, as WASAPI's shared mode
    constexpr int sixteenth = 6000; // at 120 BPM
    constexpr int bar = 16 * sixteenth;
    constexpr int seconds = 20;
    constexpr int total = seconds * 48000;

    auto node = NodeRegistry::builtin().create (nodetype::drumSynth);
    GraphNode model;
    model.type = nodetype::drumSynth;
    model.id = 3;
    model.drumPads = defaultDrumPads();
    node->configure (model);
    node->prepare ({ sampleRate, block });

    // Long cymbals and toms, so as many voices ring as the pool and the pads allow.
    for (const auto* id :
         { "p14.decay", "p16.decay", "p6.decay", "p8.decay", "p10.decay", "p12.decay", "p13.decay", "p15.decay" })
        node->params().set (static_cast<size_t> (node->descriptor().indexOfParam (id)), 400.0f);

    // The pattern, by sample.
    std::vector<std::pair<int, int>> hits; // sample, key

    for (int start = 0; start < total; start += bar)
    {
        for (int step = 0; step < 16; ++step)
        {
            const auto at = start + step * sixteenth;
            hits.emplace_back (at, step % 4 == 2 ? 46 : 42); // hats, an open one off each beat
            if (step % 2 == 0)
                hits.emplace_back (at, 51); // ride on eighths
            if (step % 4 == 0)
                hits.emplace_back (at, 36); // kick on beats
            if (step == 4 || step == 12)
                hits.emplace_back (at, 38); // snare on 2 and 4
            if (step >= 10)
                hits.emplace_back (at,
                                   std::array { 50, 48, 47, 45, 43, 41 }[static_cast<size_t> (step - 10)]); // tom fill
        }

        hits.emplace_back (start, 49); // a crash each bar
    }

    std::ranges::sort (hits);
    std::deque<CompiledNote> notes;
    std::vector<float> left (block);
    std::vector<float> right (block);
    std::vector<std::vector<float>> spare (static_cast<size_t> (maxDrumOutputs), std::vector<float> (block * 2));
    size_t next = 0;
    double worst = 0.0;
    double sum = 0.0;
    int blocks = 0;
    int most = 0;
    double voiceSum = 0.0;

    for (int offset = 0; offset < total; offset += block)
    {
        EventBuffer events;

        for (; next < hits.size() && hits[next].first < offset + block; ++next)
        {
            auto& note = notes.emplace_back();
            note.id = static_cast<ObjectId> (notes.size());
            note.pitch = hits[next].second;
            note.velocity = 0.9f;
            NoteEvent event;
            event.sampleOffset = hits[next].first - offset;
            event.note = &note;
            event.noteId = note.id;
            REQUIRE (events.add (event));
        }

        std::vector<AudioOutput> outputs { { left.data(), right.data() } };

        for (int port = 1; port < maxDrumOutputs; ++port)
            outputs.push_back (
                { spare[static_cast<size_t> (port)].data(), spare[static_cast<size_t> (port)].data() + block });

        ProcessContext context;
        context.numSamples = block;
        context.eventsIn = &events;
        context.audioOutputs = outputs;

        const auto start = std::chrono::steady_clock::now();
        node->process (context);
        const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();

        worst = std::max (worst, ms);
        sum += ms;
        ++blocks;
        const auto voices = static_cast<int> (node->meters()["voices"]);
        most = std::max (most, voices);
        voiceSum += voices;
    }

    const auto mean = sum / blocks;
    std::printf ("Drum Synth bench: %d blocks of %d samples, %zu hits\n", blocks, block, hits.size());
    std::printf ("  mean %.4f ms per block (%.2f %% of a 10 ms block), worst %.4f ms\n", mean, mean * 10.0, worst);
    std::printf ("  voices: mean %.1f, most %d\n", voiceSum / blocks, most);
    CHECK (most >= 8); // the ride and the crash restrike one voice each (section 3.5)
    // Measured 2026-11-19 at about 0.89 ms (16-thread desktop, Release): the six long cymbals are most of it, at about
    // 200 ns a sample each. The bound catches a regression, not a target.
    CHECK (mean < 1.5);
}
