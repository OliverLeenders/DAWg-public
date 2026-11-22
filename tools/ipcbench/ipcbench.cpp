// What it costs to hand an audio block to another process and back (docs/design/out-of-process-plugins.md, O2, step 1).
//
// No plugins and no JUCE: real child processes with one thread per instance, shared memory for the audio, and the engine's
// way of working, where the audio thread and a pool of workers take instances from a shared counter once per block. The
// blocks are paced as a device would pace them. Windows only for now.
//
//   ipcbench <instances> <processes> <blockSamples> <seconds> <spinMicros> <workMicros> <workers> <priority 0|1>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr int maxBlock = 512;
constexpr double sampleRate = 48000.0;

struct Slot
{
    alignas (64) std::atomic<uint32_t> request { 0 };
    alignas (64) std::atomic<uint32_t> done { 0 };
    std::atomic<uint32_t> waiting { 0 };
    std::atomic<uint32_t> stop { 0 };
    int numSamples = 0;
    int workMicros = 0;
    float input[2][maxBlock] {};
    float output[2][maxBlock] {};
};

using Clock = std::chrono::steady_clock;

[[nodiscard]] double microsSince (Clock::time_point start)
{
    return std::chrono::duration<double, std::micro> (Clock::now() - start).count();
}

void burn (int micros)
{
    const auto start = Clock::now();

    while (microsSince (start) < micros)
        YieldProcessor();
}

void raisePriority (bool wanted)
{
    if (! wanted)
        return;

    DWORD index = 0;
    AvSetMmThreadCharacteristicsW (L"Pro Audio", &index);
}

[[nodiscard]] std::wstring wide (const std::string& text)
{
    return { text.begin(), text.end() };
}

[[nodiscard]] std::string nameOf (const std::string& base, const char* kind, int slot)
{
    return "Local\\" + base + "_" + kind + "_" + std::to_string (slot);
}

// The helper's side: one thread per instance, asleep until its block arrives.
int runChild (const std::string& base, int firstSlot, int count, bool priority)
{
    std::vector<std::thread> threads;

    for (int i = 0; i < count; ++i)
    {
        threads.emplace_back (
            [=]
            {
                const auto slotIndex = firstSlot + i;
                auto* mapping = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, wide (nameOf (base, "map", slotIndex)).c_str());
                auto* slot = static_cast<Slot*> (MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (Slot)));
                auto* requested = OpenEventW (EVENT_ALL_ACCESS, FALSE, wide (nameOf (base, "req", slotIndex)).c_str());
                auto* finished = OpenEventW (EVENT_ALL_ACCESS, FALSE, wide (nameOf (base, "don", slotIndex)).c_str());
                raisePriority (priority);
                uint32_t seen = 0;

                while (slot->stop.load() == 0)
                {
                    WaitForSingleObject (requested, 1000);
                    const auto request = slot->request.load (std::memory_order_acquire);

                    if (request == seen)
                        continue;

                    seen = request;

                    for (int ch = 0; ch < 2; ++ch)
                        std::memcpy (slot->output[ch], slot->input[ch], sizeof (float) * static_cast<size_t> (slot->numSamples));

                    if (slot->workMicros > 0)
                        burn (slot->workMicros);

                    slot->done.store (seen, std::memory_order_release);

                    if (slot->waiting.exchange (0) != 0)
                        SetEvent (finished);
                }
            });
    }

    for (auto& thread : threads)
        thread.join();

    return 0;
}

struct Instance
{
    Slot* slot = nullptr;
    HANDLE requested = nullptr;
    HANDLE finished = nullptr;
    uint32_t sequence = 0;
};

// One round trip on the engine's side: write the input, wake the helper, spin for a while and then wait.
void roundTrip (Instance& instance, int numSamples, int spinMicros)
{
    auto& slot = *instance.slot;

    for (int ch = 0; ch < 2; ++ch)
        std::memset (slot.input[ch], 0, sizeof (float) * static_cast<size_t> (numSamples));

    slot.numSamples = numSamples;
    const auto sequence = ++instance.sequence;
    slot.request.store (sequence, std::memory_order_release);
    SetEvent (instance.requested);

    const auto start = Clock::now();

    while (slot.done.load (std::memory_order_acquire) != sequence && microsSince (start) < spinMicros)
        YieldProcessor();

    if (slot.done.load (std::memory_order_acquire) == sequence)
        return;

    slot.waiting.store (1);

    while (slot.done.load (std::memory_order_acquire) != sequence)
        WaitForSingleObject (instance.finished, 1);
}

[[nodiscard]] double percentile (std::vector<float>& values, double fraction)
{
    if (values.empty())
        return 0.0;

    const auto index = std::min (values.size() - 1, static_cast<size_t> (fraction * static_cast<double> (values.size())));
    std::nth_element (values.begin(), values.begin() + static_cast<std::ptrdiff_t> (index), values.end());
    return static_cast<double> (values[index]);
}
} // namespace

int main (int argc, char** argv)
{
    if (argc >= 6 && std::strcmp (argv[1], "--child") == 0)
        return runChild (argv[2], std::atoi (argv[3]), std::atoi (argv[4]), std::atoi (argv[5]) != 0);

    if (argc < 9)
    {
        std::puts ("ipcbench <instances> <processes> <blockSamples> <seconds> <spinMicros> <workMicros> <workers> <priority 0|1>");
        return 2;
    }

    const auto instances = std::atoi (argv[1]);
    const auto processes = std::max (1, std::atoi (argv[2]));
    const auto block = std::min (maxBlock, std::atoi (argv[3]));
    const auto seconds = std::atof (argv[4]);
    const auto spinMicros = std::atoi (argv[5]);
    const auto workMicros = std::atoi (argv[6]);
    const auto workers = std::atoi (argv[7]);
    const auto priority = std::atoi (argv[8]) != 0;
    const auto base = "dawgipc" + std::to_string (GetCurrentProcessId());

    std::vector<Instance> all (static_cast<size_t> (instances));

    for (int i = 0; i < instances; ++i)
    {
        auto* mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof (Slot), wide (nameOf (base, "map", i)).c_str());
        auto* memory = MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (Slot));
        all[static_cast<size_t> (i)].slot = new (memory) Slot();
        all[static_cast<size_t> (i)].slot->workMicros = workMicros;
        all[static_cast<size_t> (i)].requested = CreateEventW (nullptr, FALSE, FALSE, wide (nameOf (base, "req", i)).c_str());
        all[static_cast<size_t> (i)].finished = CreateEventW (nullptr, FALSE, FALSE, wide (nameOf (base, "don", i)).c_str());
    }

    wchar_t exe[MAX_PATH] {};
    GetModuleFileNameW (nullptr, exe, MAX_PATH);
    std::vector<PROCESS_INFORMATION> children;

    for (int p = 0; p < processes; ++p)
    {
        const auto first = p * instances / processes;
        const auto count = (p + 1) * instances / processes - first;
        auto command = L"\"" + std::wstring (exe) + L"\" --child " + wide (base) + L" " + std::to_wstring (first) + L" " + std::to_wstring (count) + L" "
                       + (priority ? L"1" : L"0");
        STARTUPINFOW startup {};
        startup.cb = sizeof (startup);
        PROCESS_INFORMATION info {};

        if (! CreateProcessW (nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
            return 3;

        children.push_back (info);
    }

    Sleep (300);

    const auto periodMicros = block / sampleRate * 1.0e6;
    const auto blocks = static_cast<size_t> (seconds * 1.0e6 / periodMicros);
    std::atomic<int> next { 0 };
    std::atomic<int> finished { 0 };
    std::atomic<uint32_t> generation { 0 };
    std::atomic<bool> quit { false };
    const auto threads = workers + 1;
    std::vector<std::vector<float>> trips (static_cast<size_t> (threads));

    for (auto& samples : trips)
        samples.reserve (blocks * static_cast<size_t> (instances) + 16);

    const auto takeWork = [&] (size_t thread)
    {
        for (auto index = next.fetch_add (1); index < instances; index = next.fetch_add (1))
        {
            const auto start = Clock::now();
            roundTrip (all[static_cast<size_t> (index)], block, spinMicros);
            trips[thread].push_back (static_cast<float> (microsSince (start)));
            finished.fetch_add (1);
        }
    };

    std::vector<std::thread> pool;

    for (int w = 0; w < workers; ++w)
    {
        pool.emplace_back (
            [&, w]
            {
                raisePriority (priority);
                uint32_t seen = 0;

                while (true)
                {
                    generation.wait (seen);
                    seen = generation.load();

                    if (quit.load())
                        return;

                    takeWork (static_cast<size_t> (w) + 1);
                }
            });
    }

    raisePriority (priority);
    auto* timer = CreateWaitableTimerExW (nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    std::vector<float> blockTimes;
    blockTimes.reserve (blocks);
    std::vector<float> wakeLateness;
    wakeLateness.reserve (blocks);
    const auto begin = Clock::now();

    for (size_t b = 0; b < blocks; ++b)
    {
        // Paced as a device would call: sleep to near the block's time, then spin the rest.
        const auto dueMicros = static_cast<double> (b) * periodMicros;
        const auto ahead = dueMicros - microsSince (begin);

        if (ahead > 1500.0)
        {
            LARGE_INTEGER due {};
            due.QuadPart = -static_cast<LONGLONG> ((ahead - 1000.0) * 10.0);
            SetWaitableTimer (timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject (timer, INFINITE);
        }

        while (microsSince (begin) < dueMicros)
            YieldProcessor();

        wakeLateness.push_back (static_cast<float> (microsSince (begin) - dueMicros));
        const auto start = Clock::now();
        finished.store (0);
        next.store (0);
        generation.fetch_add (1);
        generation.notify_all();
        takeWork (0);

        while (finished.load() < instances)
            YieldProcessor();

        blockTimes.push_back (static_cast<float> (microsSince (start)));
    }

    quit.store (true);
    generation.fetch_add (1);
    generation.notify_all();

    for (auto& thread : pool)
        thread.join();

    for (auto& instance : all)
    {
        instance.slot->stop.store (1);
        SetEvent (instance.requested);
    }

    for (const auto& child : children)
        WaitForSingleObject (child.hProcess, 3000);

    std::vector<float> merged;

    for (const auto& samples : trips)
        merged.insert (merged.end(), samples.begin(), samples.end());

    size_t overruns = 0;

    for (const auto value : blockTimes)
        if (static_cast<double> (value) > periodMicros)
            ++overruns;

    auto top = *std::max_element (merged.begin(), merged.end());
    const auto p50 = percentile (merged, 0.5);
    const auto p99 = percentile (merged, 0.99);
    const auto p999 = percentile (merged, 0.999);
    const auto block50 = percentile (blockTimes, 0.5);
    const auto block999 = percentile (blockTimes, 0.999);

    std::printf ("%3d inst %3d proc %3d smp %3d w spin %3d work %3d prio %d | trip us p50 %7.1f p99 %7.1f p99.9 %8.1f max %8.1f | block us p50 %8.1f p99.9 %8.1f of %6.0f | overruns %zu/%zu | wake p99.9 %6.1f\n",
                 instances, processes, block, workers, spinMicros, workMicros, priority ? 1 : 0, p50, p99, p999,
                 static_cast<double> (top), block50, block999, periodMicros, overruns, blockTimes.size(), percentile (wakeLateness, 0.999));
    return 0;
}
