#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Minimal WAV reading and writing for the render tool and tests: PCM 16/24/32-bit and 32-bit float, mono or stereo
// in, 32-bit float stereo out.

namespace harness
{

/**
    Stereo audio at a sample rate.
*/
struct Audio
{
    /**
        Sample rate in Hz.
    */
    double sampleRate = 48000.0;

    /**
        Left channel (the same length as the right).
    */
    std::vector<float> left;

    /**
        Right channel.
    */
    std::vector<float> right;
};

/**
    Reads a WAV file. A mono file gives the same samples on both channels.

    @param path   The file.
    @param error  Receives a message on failure.
    @return       The audio, or nothing.
*/
[[nodiscard]] std::optional<Audio> readWav (const std::filesystem::path& path, std::string& error);

/**
    Writes 32-bit float stereo WAV.

    @param path   The file.
    @param audio  The audio.
    @return       True on success.
*/
[[nodiscard]] bool writeWav (const std::filesystem::path& path, const Audio& audio);

} // namespace harness
