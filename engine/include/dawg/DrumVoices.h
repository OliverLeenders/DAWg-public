#pragma once

#include "dawg/DrumEnvelopes.h"
#include "dawg/Node.h"
#include "dawg/Svf.h"

#include <array>
#include <cstdint>
#include <span>

/**
    The Drum Synth's voice models (docs/design/drum-synth.md, DR5-DR7): analogue-style kick, snare, tom, hat, cymbal,
    clap and rim, each a handful of oscillators, noise, filters and envelopes. One DrumVoice plays any model; a node
    keeps a pool of them, allocated up front, so nothing on the audio thread allocates.
*/
namespace dawg
{

/**
    The number of models, off included.
*/
inline constexpr size_t drumModelCount = 8;

/**
    The most parameters a model has of its own.
*/
inline constexpr size_t maxDrumModelParams = 8;

/**
    The parameters every model has, in this order: Level (dB), Pan (%), Tune (st), Decay (% of the model's), Vel. amount
    (%, how much velocity changes the level).
*/
enum DrumCommonParam : size_t
{
    drumLevel,
    drumPan,
    drumTune,
    drumDecay,
    drumVelocity,
    drumCommonCount,
};

/**
    Returns the common parameters' descriptions (DR3), ids without the pad's prefix.
*/
[[nodiscard]] std::span<const ParamInfo> drumCommonParams() noexcept;

/**
    Returns a model's own parameters (DR3), ids without the pad's prefix; empty for off.

    @param model  The model.
*/
[[nodiscard]] std::span<const ParamInfo> drumModelParams (DrumModel model) noexcept;

/**
    The range a parameter is randomised within (docs/design/drum-synth.md, DR14): narrower than its full range, so a
    random kit stays playable. A range whose low is above its high is left alone by a randomise (a tom's pitch, which
    sets its place among the toms; a cymbal's bell, which makes it a ride or a crash).
*/
struct DrumRange
{
    float low = 0.0f;  ///< The lowest value a randomise picks.
    float high = 0.0f; ///< The highest.

    /**
        Returns true if a randomise leaves the parameter alone.
    */
    [[nodiscard]] bool kept() const noexcept { return low > high; }
};

/**
    Returns a model's randomise ranges, in drumModelParams() order; empty for off.

    @param model  The model.
*/
[[nodiscard]] std::span<const DrumRange> drumModelRandomRanges (DrumModel model) noexcept;

/**
    Returns a model's display name ("Kick"), and its identifier for the project file ("kick").

    @param model  The model.
*/
[[nodiscard]] const char* drumModelName (DrumModel model) noexcept;

/**
    See drumModelName().
*/
[[nodiscard]] const char* drumModelId (DrumModel model) noexcept;

/**
    One hit: the model, its parameter values and the note's velocity.
*/
struct DrumHit
{
    DrumModel model = DrumModel::off;                ///< What plays.
    std::array<float, drumCommonCount> common {};    ///< Values in DrumCommonParam order.
    std::array<float, maxDrumModelParams> values {}; ///< The model's own, in drumModelParams() order.
    float velocity = 127.0f;                         ///< The note-on velocity, 1..127.
    std::uint32_t seed = 1;                          ///< The noise's seed: equal seeds sound alike (DR11).

    /**
        A hit of a model with every parameter at its default, at full velocity.

        @param model  The model.
    */
    [[nodiscard]] static DrumHit defaults (DrumModel model) noexcept;

    /**
        Sets a parameter by its id, common or the model's own; ignores an unknown id.

        @param id     The id, as in drumCommonParams() or drumModelParams().
        @param value  The value.
        @return       This hit.
    */
    DrumHit& set (const char* id, float value) noexcept;
};

/**
    The hats' metal (section 3.4): six squares at the 808's unrelated frequencies, each band-limited by
    polyBLEP, summed.
*/
class DrumMetal final
{
public:
    /**
        The squares' frequencies in Hz at a ratio of 1, as usually cited from the 808's schematic.
    */
    static constexpr std::array<double, 6> frequencies { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };

    /**
        Puts every square back at phase 0, at a pitch.

        @param ratio       The squares' frequencies are frequencies times this.
        @param sampleRate  The sample rate.
    */
    void start (double ratio, double sampleRate) noexcept;

    /**
        Returns the next sample, -1..1, and advances.
    */
    [[nodiscard]] double next() noexcept;

private:
    std::array<double, 6> m_phases {};
    std::array<double, 6> m_steps {};
};

/**
    One sounding hit of any model. prepare() once, then trigger() and render() as notes come; it allocates nothing
    after prepare().
*/
class DrumVoice final
{
public:
    /**
        Sets the sample rate and silences the voice.

        @param sampleRate  The sample rate.
    */
    void prepare (double sampleRate) noexcept;

    /**
        Starts a hit, cutting whatever the voice was playing.

        @param hit  The hit.
    */
    void trigger (const DrumHit& hit) noexcept;

    /**
        Strikes a sounding cymbal again (section 3.5): its modes ring on and the new hit adds to them, as on a real
        cymbal, where a second voice would only ring a second, identical cymbal. The hit's parameters replace the
        voice's. Does nothing for any other model, or a voice that is silent or choking.

        @param hit  The hit, a cymbal's.
        @return     True if the voice took the hit; false if it needs a voice of its own.
    */
    bool restrike (const DrumHit& hit) noexcept;

    /**
        Fades the voice out linearly, as a choke or a stolen voice does (DR8). A silent voice stays silent.

        @param seconds  The fade's length.
    */
    void choke (double seconds = 0.005) noexcept;

    /**
        Returns true while the voice sounds.
    */
    [[nodiscard]] bool active() const noexcept;

    /**
        Returns true while a choke fades the voice out.
    */
    [[nodiscard]] bool choking() const noexcept { return m_fadeStep > 0.0 && active(); }

    /**
        Adds the voice's next samples to two channels.

        @param left   The left channel.
        @param right  The right channel.
        @param count  The number of samples.
    */
    void render (float* left, float* right, int count) noexcept;

private:
    static constexpr size_t maxEnvelopes = 6;
    static constexpr size_t maxModes = 4;
    static constexpr size_t maxFilters = 7;
    static constexpr size_t maxCymbalModes = 128;

    // The cymbal's resonators, four at a time: y = a1 y1 + a2 y2 + x, panned by two gains.
    struct CymbalModes
    {
        alignas (16) std::array<float, maxCymbalModes> a1 {};
        alignas (16) std::array<float, maxCymbalModes> a2 {};
        alignas (16) std::array<float, maxCymbalModes> gainLeft {};
        alignas (16) std::array<float, maxCymbalModes> gainRight {};
        alignas (16) std::array<float, maxCymbalModes> y1 {};
        alignas (16) std::array<float, maxCymbalModes> y2 {};
    };

    // A level that hits add to: a linear rise, then an exponential fall.
    struct Bloom
    {
        float value = 0.0f;
        float fall = 0.0f;
        float step = 0.0f;
        int rising = 0;

        void add (float amount, double riseSeconds, double fallSeconds, double sampleRate) noexcept;
        [[nodiscard]] float next() noexcept;
    };

    void setUp (const DrumHit& hit) noexcept;
    void setUpCymbal (double tune, double scale, float strength) noexcept;

    [[nodiscard]] float noise() noexcept;
    [[nodiscard]] float noiseRight() noexcept;
    [[nodiscard]] float filter (size_t index, size_t channel, float x) noexcept;
    void setFilter (size_t index, SvfMode mode, double cutoff, double k) noexcept;
    [[nodiscard]] float sines (size_t from, size_t to, double sweepRatio) noexcept;
    [[nodiscard]] float drive (float x) noexcept;
    [[nodiscard]] float burst (size_t filterIndex, size_t envelope, float gain) noexcept;

    [[nodiscard]] float kick() noexcept;
    [[nodiscard]] float snare() noexcept;
    [[nodiscard]] float tom() noexcept;
    [[nodiscard]] float hat() noexcept;
    [[nodiscard]] std::array<float, 2> cymbal() noexcept;
    [[nodiscard]] float clap() noexcept;
    [[nodiscard]] float rim() noexcept;

    double m_sampleRate = 48000.0;
    DrumModel m_model = DrumModel::off;
    std::array<float, maxDrumModelParams> m_values {};
    float m_level = 0.0f;
    float m_accent = 0.0f;
    std::array<float, 2> m_pan { 1.0f, 1.0f };
    double m_pitch = 0.0; // Hz, tune applied

    std::array<DrumEnvelope, maxEnvelopes> m_envelopes {};
    PitchSweep m_sweep;
    std::array<double, maxModes> m_modeFrequencies {};
    std::array<double, maxModes> m_modePhases {};
    std::array<float, maxModes> m_modeGains {};
    DrumMetal m_metalLeft;
    std::array<SvfCoefficients, maxFilters> m_filters {};
    std::array<std::array<SvfState, 2>, maxFilters> m_filterStates {};
    std::uint32_t m_noise = 1;
    std::uint32_t m_noiseRight = 2;
    std::uint32_t m_noiseWide = 3; // the cymbal's wash, right
    float m_noiseGain = 0.0f;      // the noise's (or the kick's click's) level
    float m_bandScale = 1.0f;      // a band-pass's peak gain undone: its k
    float m_trim = 1.0f;           // the model's loudness at its defaults

    float m_driveGain = 1.0f;
    float m_driveNorm = 1.0f;

    int m_burstsLeft = 0;
    int m_burstSpacing = 0;
    int m_untilBurst = 0;
    double m_tailDecay = 0.0; // the clap's tail, in seconds

    double m_fade = 1.0;
    double m_fadeStep = 0.0;

    CymbalModes m_cymbal;
    size_t m_cymbalModes = 0; // a multiple of 4
    float m_strike = 0.0f;
    float m_impulse = 0.0f;  // the stick's click, played on the next sample
    float m_coupling = 0.0f; // how much of the modes' clipped sum comes back in
    float m_feedback =
        0.0f; // and what comes back on the next sample    // the hit's strength, into the stick's burst, the wash and the sizzle
    Bloom m_washEarly;
    Bloom m_washTail;
    Bloom m_sizzle;
    float m_sizzleGain = 0.0f;
    double m_washCutoff = 0.0; // Hz: the wash's low-pass, closing as the cymbal rings
    double m_washEnd = 0.0;
    double m_washClose = 0.0; // the closing per 32 samples
    int m_washUntil = 0;      // samples until the low-pass's next step
};

} // namespace dawg
