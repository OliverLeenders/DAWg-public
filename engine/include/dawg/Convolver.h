#pragma once

#include "dawg/AudioSource.h"
#include "dawg/Realtime.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

/**
    The Convolution Reverb's engine (docs/design/convolution-reverb.md): a zero-latency, non-uniformly partitioned
    convolver (I2), the IR's preparation (I4, I5, I8) and the background thread that builds IRs (I4, I6).
*/
namespace dawg::convolution
{

/**
    Taps of the direct head.
*/
inline constexpr int headLength = 64;

/**
    The partition size of each FFT level.
*/
inline constexpr std::array<int, 3> partitionSizes { 64, 512, 4096 };

/**
    The IR sample each FFT level starts at. Levels 1 and 2 start one partition in and run when their input block is
    complete; level 3 starts two partitions in, so its work can be spread over the following partition (I2).
*/
inline constexpr std::array<int, 3> levelStarts { 64, 512, 8192 };

/**
    The cycle after which every level is at a boundary together: where a new IR starts and how long it fades in (I6).
*/
inline constexpr int cycleLength = 4096;

/**
    How many steps level 3's work is spread over in a cycle: one per head-sized chunk.
*/
inline constexpr int spreadSteps = cycleLength / headLength;

/**
    The longest IR, after Stretch (I3).
*/
inline constexpr double maxSeconds = 20.0;

/**
    How an IR's channels map inputs to outputs (I3).
*/
enum class Layout
{
    mono,       ///< One channel, used for both inputs.
    stereo,     ///< Left to left, right to right.
    trueStereo, ///< Four channels: left to left, left to right, right to left, right to right.
};

/**
    One path through the convolver: an IR channel from an input to an output.
*/
struct Route
{
    int channel = 0; ///< The IR channel.
    int input = 0;   ///< The input channel it reads.
    int output = 0;  ///< The output channel it adds to.
};

/**
    Returns the layout for a file with this many channels: 1 mono, 4 true stereo, any other stereo (its first two).

    @param channels  The file's channels.
*/
[[nodiscard]] Layout layoutFor (int channels) noexcept;

/**
    Returns how many IR channels a layout has.

    @param layout  The layout.
*/
[[nodiscard]] int channelsOf (Layout layout) noexcept;

/**
    Returns a layout's routes.

    @param layout  The layout.
*/
[[nodiscard]] std::span<const Route> routesOf (Layout layout) noexcept;

/**
    An IR made ready for the convolver: the head's taps and every level's partition spectra. Immutable once built.
*/
struct Kernel
{
    /**
        The IR's layout.
    */
    Layout layout = Layout::stereo;

    /**
        The rate it was built for.
    */
    double sampleRate = 48000.0;

    /**
        Its length in samples.
    */
    int length = 0;

    /**
        The head's taps per IR channel, reversed (the last tap first), so the head is a dot product with the input
        history from oldest to newest.
    */
    std::array<std::array<float, headLength>, 4> head {};

    /**
        One FFT level's partitions.
    */
    struct Level
    {
        int partitions = 0;         ///< How many.
        std::vector<float> spectra; ///< Per IR channel and partition, partitionSize + 1 interleaved complex bins.
    };

    /**
        The levels, in the order of partitionSizes.
    */
    std::array<Level, 3> levels;
};

/**
    The input history the levels convolve: each level's input window and the spectra of its last input blocks, per
    input channel. The audio thread alone writes it once it is published (I6).
*/
struct History
{
    /**
        Creates an empty history.

        @param level3Capacity  How many level-3 input spectra it keeps.
    */
    explicit History (int level3Capacity);

    /**
        Silences it.
    */
    void clear() noexcept;

    /**
        The head's input history per input channel: headLength samples written twice, so the last headLength are
        always contiguous.
    */
    std::array<std::array<float, 2 * static_cast<size_t> (headLength)>, 2> headRing {};

    /**
        Where the newest sample is in headRing (and headLength after it).
    */
    int headPosition = 0;

    /**
        One level's history.
    */
    struct Level
    {
        std::array<std::vector<float>, 2> window; ///< The previous and current input block, per input channel.
        std::array<std::vector<float>, 2> ring;   ///< Input spectra, capacity × (partitionSize + 1) complex bins.
        int capacity = 0;                         ///< How many spectra the ring holds.
        int newest = 0;                           ///< The newest spectrum's slot.
    };

    /**
        The levels, in the order of partitionSizes.
    */
    std::array<Level, 3> levels;

    /**
        True until the audio thread first runs it: a new, larger history takes the old one's newest state then (I6).
    */
    bool fresh = true;
};

/**
    What the convolver runs: a kernel (null for silence) and the history it runs on.
*/
struct Program
{
    std::shared_ptr<const Kernel> kernel; ///< The IR, or null for a silent wet path.
    std::shared_ptr<History> history;     ///< Never null.
};

/**
    Hands programs from the builder thread to the audio thread. Unlike SnapshotExchange, the audio thread keeps a
    program for as long as it fades out and retires it itself (I6).

    @note  publish() and collectGarbage(): one thread (the builder). take() and retire(): the audio thread.
*/
class ProgramExchange final
{
public:
    /**
        Makes a program available to the audio thread.

        @param program  The program.
    */
    void publish (std::shared_ptr<const Program> program);

    /**
        Frees programs the audio thread retired.
    */
    void collectGarbage();

    /**
        Returns a program published since the last call, or nullptr.

        @note  Audio thread. Never blocks or allocates.
    */
    [[nodiscard]] const Program* take() noexcept { return m_latest.exchange (nullptr); }

    /**
        Hands a program back to be freed.

        @param program  A program take() returned.
        @note  Audio thread. Never blocks or allocates.
    */
    void retire (const Program* program) noexcept
    {
        if (program != nullptr)
            std::ignore = m_retired.push (program); // when full, the program stays owned: a leak, never a free here
    }

private:
    void release (const Program* program);

    std::atomic<const Program*> m_latest { nullptr };
    SpscQueue<const Program*, 64> m_retired;
    std::vector<std::shared_ptr<const Program>> m_owned;
};

/**
    The zero-latency convolver (I2, I6): a direct head and three uniformly partitioned FFT levels, with a crossfade
    from one program to the next at a cycle boundary. The output depends only on the input and the programs, not on
    the block sizes.
*/
class Convolver final
{
public:
    Convolver();

    /**
        Allocates the output buffers and silences them (the history is left to clear()).

        @note  Message thread, while the audio thread doesn't use the convolver.
    */
    void prepare();

    /**
        Clears the history and forgets every program (they go back through the exchange).

        @note  Audio thread, or the message thread while the audio thread doesn't use the convolver.
    */
    void reset (ProgramExchange& exchange) noexcept;

    /**
        Silences the history, keeping the newest program; one fading out is dropped.

        @param exchange  Where a dropped program goes.
        @note  Audio thread, or the message thread while the audio thread doesn't use the convolver.
    */
    void clear (ProgramExchange& exchange) noexcept;

    /**
        Convolves one block.

        @param exchange  Where new programs come from and old ones go.
        @param input     Two input channels.
        @param output    Two output channels for the wet signal (may not alias the input).
        @param numSamples  How many samples.
        @param sampleRate  The rate the kernels must have been built for; others are treated as silence.
        @note  Audio thread. Never allocates.
    */
    void process (ProgramExchange& exchange, std::array<const float*, 2> input, std::array<float*, 2> output,
                  int numSamples, double sampleRate) noexcept;

    /**
        Returns whether a program is running (a kernel may still be null).
    */
    [[nodiscard]] bool running() const noexcept { return m_current >= 0; }

private:
    // A program in one of three roles: current, fading out (previous), or preparing its first level-3 block before
    // it fades in (I6).
    struct Slot
    {
        const Program* program = nullptr;
        std::array<std::array<std::vector<float>, 2>, 3> output; // per level and output channel, partitionSize samples
        std::array<std::vector<float>, 2> spread; // level 3's sum over the cycle so far, per output (interleaved bins)
    };

    [[nodiscard]] const Kernel* kernelOf (int slot) const noexcept;
    [[nodiscard]] int freeSlot() const noexcept;
    void install (const Program* program, int slot) noexcept;
    void adoptHistory (const Program* from, const Program* to) noexcept;
    void transform (size_t level) noexcept;
    void addPartitions (int slot, size_t level, int first, int last, std::array<float*, 2> sums) noexcept;
    void inverse (size_t level, const std::array<float*, 2>& sums, std::array<std::vector<float>, 2>& outputs) noexcept;
    void takeProgram (ProgramExchange& exchange) noexcept;
    void addOutput (int slot, int position, std::array<float, 2>& sum, float gain) const noexcept;
    static void pushHeads (const std::array<History*, 3>& targets, int count, float left, float right) noexcept;
    void runLevel (size_t level) noexcept;
    void spreadStep (int step) noexcept;
    void cycleBoundary (ProgramExchange& exchange) noexcept;
    [[nodiscard]] int histories (std::array<History*, 3>& result) const noexcept;

    std::array<std::unique_ptr<juce::dsp::FFT>, 3> m_ffts;
    std::array<Slot, 3> m_slots;
    int m_current = -1;
    int m_previous = -1;  // fading out
    int m_preparing = -1; // fades in at the next cycle
    int m_fadePosition = 0;
    const Program* m_pending = nullptr; // waits for the next cycle to start preparing
    int m_position = 0;                 // samples into the cycle
    double m_rate = 0.0;
    std::vector<float> m_scratch;                    // 2 × the largest FFT size
    std::array<std::vector<float>, 2> m_accumulator; // per output, the largest level's bins
};

/**
    How the controls of section I8 shape an IR.
*/
struct Shape
{
    double startMs = 0.0;          ///< Start, ms.
    double lengthPercent = 100.0;  ///< Length, % of what follows Start.
    double decayPercent = 100.0;   ///< Decay, % of the IR's decay time.
    double stretchPercent = 100.0; ///< Stretch, %.
    bool reverse = false;          ///< Reverse.
    bool normalise = true;         ///< Normalise (I5).

    /// Whether two are the same, member by member.
    // Exact comparison of the floating-point members is intended (an edit changed them or not).
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    [[nodiscard]] bool operator== (const Shape&) const = default;
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
};

/**
    What the panel shows of an IR file (I12).
*/
struct Analysis
{
    std::vector<float> envelope; ///< The whole file's envelope in dB, at most 256 points.
    double seconds = 0.0;        ///< The file's length.
    double decaySeconds = 0.0;   ///< Its estimated decay time to −60 dB, or 0.
};

/**
    Generates the default IR (I10) at a rate: stereo noise decaying by 60 dB in 2 s, darkening as it decays.

    @param sampleRate  The rate.
    @return            The IR.
*/
[[nodiscard]] juce::AudioBuffer<float> defaultImpulse (double sampleRate);

/**
    Estimates an IR's decay time from its Schroeder backward integral (T30, or T20 for IRs that never fall 35 dB).

    @param ir          The IR, any number of channels.
    @param sampleRate  Its rate.
    @return            The time to fall 60 dB in seconds, or 0 if it can't be estimated.
*/
[[nodiscard]] double estimateDecay (const juce::AudioBuffer<float>& ir, double sampleRate);

/**
    Analyses an IR for the panel.

    @param ir          The IR.
    @param sampleRate  Its rate.
    @return            The envelope, length and decay time.
*/
[[nodiscard]] Analysis analyse (const juce::AudioBuffer<float>& ir, double sampleRate);

/**
    Resamples by a ratio with a windowed-sinc low-pass (I4, step 5).

    @param input  The audio.
    @param ratio  Output samples per input sample.
    @return       The resampled audio, round (length × ratio) samples long.
*/
[[nodiscard]] juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& input, double ratio);

/**
    An IR shaped and brought to the device rate.
*/
struct Shaped
{
    juce::AudioBuffer<float> ir; ///< One channel per IR channel of the layout.
    bool truncated = false;      ///< Whether it was cut at maxSeconds.
};

/**
    Shapes an IR: Start and Length, Decay and the fades, Reverse, resampling with Stretch, and the level (I4, I5, I8).

    @param file          The IR at its own rate, with the layout's channels (or more).
    @param fileRate      Its rate.
    @param layout        Its layout.
    @param deviceRate    The rate to build for.
    @param shape         The controls.
    @param decaySeconds  The file's estimated decay time (estimateDecay()), for Decay.
    @return              The IR at the device rate.
*/
[[nodiscard]] Shaped shapeImpulse (const juce::AudioBuffer<float>& file, double fileRate, Layout layout,
                                   double deviceRate, const Shape& shape, double decaySeconds);

/**
    Splits an IR into the head and partitions and transforms them.

    @param ir          The IR at the device rate, one channel per IR channel of the layout.
    @param layout      Its layout.
    @param sampleRate  The device rate.
    @return            The kernel.
*/
[[nodiscard]] std::shared_ptr<const Kernel> makeKernel (const juce::AudioBuffer<float>& ir, Layout layout,
                                                        double sampleRate);

/**
    Returns how many level-3 partitions an IR of this length needs.

    @param length  Samples.
*/
[[nodiscard]] int level3Partitions (int length) noexcept;

/**
    What a node asks the builder for.
*/
struct Request
{
    std::shared_ptr<const AudioSource> source; ///< The IR file, or null.
    bool useDefault = true;                    ///< No file: the default IR. Otherwise a null source is a missing file.
    double sampleRate = 0.0;                   ///< The device rate.
    Shape shape;                               ///< The controls.
};

/**
    What the panel shows (I12).
*/
struct PanelInfo
{
    Analysis analysis;      ///< The IR's envelope, length and decay time.
    bool missing = false;   ///< The file is missing.
    bool truncated = false; ///< The IR was cut at maxSeconds.
    bool ready = false;     ///< A build has finished.
};

/**
    A node's connection to the builder: its requests, the programs built for it and what the panel shows.
*/
class Inbox final
{
public:
    /**
        The programs for the audio thread.
    */
    ProgramExchange exchange;

    /**
        Returns what the panel shows.

        @note  Any thread.
    */
    [[nodiscard]] PanelInfo panel() const;

    /**
        Returns whether a build is waiting or running.
    */
    [[nodiscard]] bool pending() const noexcept { return m_pending.load(); }

private:
    friend class Builder;

    // Builder thread only.
    std::shared_ptr<History> m_history;
    const AudioSource* m_loadedSource = nullptr;
    bool m_loadedDefault = false;
    double m_loadedRate = 0.0;
    juce::AudioBuffer<float> m_file;
    double m_fileRate = 0.0;
    Layout m_layout = Layout::stereo;
    Analysis m_analysis;

    // Under the builder's lock.
    std::optional<Request> m_request;

    std::atomic<bool> m_pending { false };
    mutable std::mutex m_panelLock;
    PanelInfo m_panel;
};

/**
    The shared background thread that builds IRs (I4): newest request per node, one at a time.
*/
class Builder final
{
public:
    /**
        Returns the builder.
    */
    [[nodiscard]] static Builder& shared();

    ~Builder();

    Builder (const Builder&) = delete;
    Builder& operator= (const Builder&) = delete;
    Builder (Builder&&) = delete;
    Builder& operator= (Builder&&) = delete;

    /**
        Asks for a program; replaces a request of the same inbox that hasn't started.

        @param inbox    The node's inbox.
        @param request  What to build.
        @note  Any thread but the audio thread.
    */
    void request (const std::shared_ptr<Inbox>& inbox, Request request);

    /**
        Waits until no request is waiting or running (I15).
    */
    void waitIdle();

private:
    Builder();
    void run();
    void build (Inbox& inbox, const Request& request);

    std::mutex m_lock;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::vector<std::weak_ptr<Inbox>> m_queue;
    std::vector<std::weak_ptr<Inbox>> m_known;
    bool m_busy = false;
    bool m_stop = false;
    std::thread m_thread;
};

} // namespace dawg::convolution
