#include "dawg/PitchModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <onnxruntime_cxx_api.h>
#include <string>

namespace dawg
{
namespace
{
constexpr double modelRate = 16000.0;
constexpr int hop = 160; // 10 ms at 16 kHz, both models
constexpr size_t bins = 360;

// CREPE: 1024 samples per frame, centred on the frame.
constexpr int crepeWindow = 1024;
constexpr size_t crepeBatch = 256;

// FCPE reads the whole signal; long audio goes in chunks of 30 s, each with 0.5 s of the audio around it, so the
// model's context at a chunk's edge is the real audio and memory stays small.
constexpr int fcpeChunk = 30 * 16000;
constexpr int fcpeContext = 8000;

// Peak saliences that are voiced as often as not: the thresholds with the lowest frame error on PTDB-TUG's
// development speakers, from huggingface.co/Scicom-intl/fcpe-onnx (pitch_onnx.py).
constexpr float fcpeThreshold = 0.33f;
constexpr float crepeTinyThreshold = 0.58f;
constexpr float crepeThreshold = 0.61f;

// MIDI semitones of a pitch in cents above 10 Hz.
[[nodiscard]] float midiOfCents (double cents) noexcept
{
    return static_cast<float> (69.0 + (cents - 1200.0 * std::log2 (44.0)) / 100.0);
}

[[nodiscard]] Ort::Env& environment()
{
    static Ort::Env env (ORT_LOGGING_LEVEL_WARNING, "dawg");
    return env;
}

// FNV-1a over the model file, so the identity changes with the weights.
[[nodiscard]] juce::String hashOf (const juce::File& file)
{
    juce::MemoryBlock data;

    if (! file.loadFileAsData (data))
        return "0";

    auto hash = std::uint64_t { 14695981039346656037ull };

    for (size_t i = 0; i < data.getSize(); ++i)
    {
        hash ^= static_cast<std::uint8_t> (data[i]);
        hash *= 1099511628211ull;
    }

    return juce::String::toHexString (static_cast<juce::int64> (hash)).paddedLeft ('0', 16);
}

enum class Kind
{
    crepe,
    fcpe,
};

class OnnxPitchModel final : public PitchModel
{
public:
    OnnxPitchModel (const juce::File& file, Kind kind, Ort::Session session, std::string input, std::string output)
        : m_kind (kind),
          m_identity (file.getFileNameWithoutExtension() + ":" + hashOf (file)),
          m_threshold (kind == Kind::fcpe                               ? fcpeThreshold
                       : file.getFileName().containsIgnoreCase ("tiny") ? crepeTinyThreshold
                                                                        : crepeThreshold),
          m_session (std::move (session)),
          m_input (std::move (input)),
          m_output (std::move (output))
    {
        m_binPitches.resize (bins);

        for (size_t k = 0; k < bins; ++k)
        {
            if (kind == Kind::crepe)
            {
                // 20 cents per bin from 1997.38 cents above 10 Hz (32.70 Hz).
                m_binPitches[k] = midiOfCents (1997.3794084376191 + 20.0 * static_cast<double> (k));
            }
            else
            {
                // Evenly spaced in cents from 32.7 to 1975.5 Hz.
                const auto low = 1200.0 * std::log2 (32.7 / 10.0);
                const auto high = 1200.0 * std::log2 (1975.5 / 10.0);
                m_binPitches[k] = midiOfCents (low + (high - low) * static_cast<double> (k) / (bins - 1));
            }
        }
    }

    [[nodiscard]] juce::String identity() const override { return m_identity; }

    [[nodiscard]] double sampleRate() const noexcept override { return modelRate; }

    [[nodiscard]] float voicingThreshold() const noexcept override { return m_threshold; }

    [[nodiscard]] PitchSalience salience (const std::vector<float>& audio,
                                          const std::function<bool()>& cancelled) const override
    {
        PitchSalience result;
        result.hopSeconds = hop / modelRate;
        // FCPE's STFT isn't centred: its frame i covers 432 samples before i * hop to 592 after, so its middle is
        // half a hop later than CREPE's.
        result.startSeconds = m_kind == Kind::fcpe ? 0.5 * hop / modelRate : 0.0;
        result.binPitches = m_binPitches;

        const auto frames = audio.size() / hop + 1;
        result.values.assign (frames * bins, 0.0f);

        auto ok = false;

        try
        {
            ok = m_kind == Kind::crepe ? runCrepe (audio, frames, result.values, cancelled)
                                       : runFcpe (audio, frames, result.values, cancelled);
        }
        catch (const Ort::Exception& e)
        {
            juce::ignoreUnused (e);
            DBG ("Pitch model failed: " << e.what());
        }

        if (! ok)
            result.values.clear();

        return result;
    }

private:
    [[nodiscard]] static bool isCancelled (const std::function<bool()>& cancelled)
    {
        return cancelled != nullptr && cancelled();
    }

    // Runs the model on one input tensor and copies `count` frames of its output, from frame `skip`, to `out`.
    void run (std::vector<float>& input, const std::vector<std::int64_t>& shape, size_t skip, size_t count,
              float* out) const
    {
        const auto memory = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float> (memory, input.data(), input.size(), shape.data(), shape.size());
        const std::array inputNames { m_input.c_str() };
        const std::array outputNames { m_output.c_str() };
        auto outputs =
            m_session.Run (Ort::RunOptions { nullptr }, inputNames.data(), &tensor, 1, outputNames.data(), 1);
        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const auto available = info.GetElementCount() / bins;
        const auto* data = outputs[0].GetTensorData<float>();

        if (skip < available)
            std::copy_n (data + skip * bins, std::min (count, available - skip) * bins, out);
    }

    // CREPE: 1024 samples around each frame (zeros past the ends), in batches.
    bool runCrepe (const std::vector<float>& audio, size_t frames, std::vector<float>& out,
                   const std::function<bool()>& cancelled) const
    {
        std::vector<float> batch;

        for (size_t first = 0; first < frames; first += crepeBatch)
        {
            if (isCancelled (cancelled))
                return false;

            const auto count = std::min (crepeBatch, frames - first);
            batch.assign (count * crepeWindow, 0.0f);

            for (size_t f = 0; f < count; ++f)
            {
                const auto centre = static_cast<std::int64_t> ((first + f) * hop);

                for (int i = 0; i < crepeWindow; ++i)
                {
                    const auto at = centre - crepeWindow / 2 + i;

                    if (at >= 0 && at < static_cast<std::int64_t> (audio.size()))
                        batch[f * crepeWindow + static_cast<size_t> (i)] = audio[static_cast<size_t> (at)];
                }
            }

            run (batch, { static_cast<std::int64_t> (count), crepeWindow }, 0, count, out.data() + first * bins);
        }

        return true;
    }

    // FCPE: the signal in chunks, each with context on both sides; a chunk starts on a frame, so its frames line up
    // with the whole signal's.
    bool runFcpe (const std::vector<float>& audio, size_t frames, std::vector<float>& out,
                  const std::function<bool()>& cancelled) const
    {
        const auto length = static_cast<std::int64_t> (audio.size());
        std::vector<float> chunk;

        for (std::int64_t start = 0; start == 0 || start < length; start += fcpeChunk)
        {
            if (isCancelled (cancelled))
                return false;

            const auto from = std::max<std::int64_t> (0, start - fcpeContext);
            const auto to = std::min (length, start + fcpeChunk + fcpeContext);
            chunk.assign (audio.begin() + from, audio.begin() + to);

            if (chunk.empty())
                chunk.push_back (0.0f);

            const auto firstFrame = static_cast<size_t> (start / hop);
            const auto lastFrame =
                start + fcpeChunk >= length ? frames : static_cast<size_t> ((start + fcpeChunk) / hop);
            run (chunk, { 1, static_cast<std::int64_t> (chunk.size()) }, static_cast<size_t> ((start - from) / hop),
                 lastFrame - firstFrame, out.data() + firstFrame * bins);
        }

        return true;
    }

    Kind m_kind;
    juce::String m_identity;
    float m_threshold;
    std::vector<float> m_binPitches;
    mutable Ort::Session m_session;
    std::string m_input;
    std::string m_output;
};
} // namespace

std::unique_ptr<PitchModel> loadPitchModel (const juce::File& file, juce::String& error, int threads)
{
    if (! file.existsAsFile())
    {
        error = "No pitch model at " + file.getFullPathName();
        return nullptr;
    }

    try
    {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads (std::max (1, threads));
        options.SetInterOpNumThreads (1);
        options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);

#if JUCE_WINDOWS
        const std::wstring path = file.getFullPathName().toWideCharPointer();
#else
        const std::string path = file.getFullPathName().toStdString();
#endif
        Ort::Session session (environment(), path.c_str(), options);
        Ort::AllocatorWithDefaultOptions allocator;
        const std::string input = session.GetInputNameAllocated (0, allocator).get();
        const std::string output = session.GetOutputNameAllocated (0, allocator).get();
        Kind kind;

        if (input == "frames")
            kind = Kind::crepe;
        else if (input == "audio")
            kind = Kind::fcpe;
        else
        {
            error = "Unknown pitch model (input \"" + juce::String (input) + "\"): " + file.getFileName();
            return nullptr;
        }

        return std::make_unique<OnnxPitchModel> (file, kind, std::move (session), input, output);
    }
    catch (const Ort::Exception& e)
    {
        error = "Couldn't load the pitch model " + file.getFileName() + ": " + e.what();
        return nullptr;
    }
}

juce::File pitchModelsFolder()
{
    const auto beside = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                            .getParentDirectory()
                            .getChildFile ("models")
                            .getChildFile ("pitch");

    if (beside.isDirectory())
        return beside;

    return juce::File (DAWG_PITCH_MODELS_DIR);
}

} // namespace dawg
