#include "Wav.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace harness
{

namespace
{
[[nodiscard]] std::uint32_t read32 (const unsigned char* p) noexcept
{
    return static_cast<std::uint32_t> (p[0]) | (static_cast<std::uint32_t> (p[1]) << 8)
           | (static_cast<std::uint32_t> (p[2]) << 16) | (static_cast<std::uint32_t> (p[3]) << 24);
}

[[nodiscard]] std::uint16_t read16 (const unsigned char* p) noexcept
{
    return static_cast<std::uint16_t> (p[0] | (p[1] << 8));
}

[[nodiscard]] float sampleAt (const unsigned char* p, int format, int bits) noexcept
{
    if (format == 3 && bits == 32)
    {
        float value = 0.0f;
        std::memcpy (&value, p, sizeof value);
        return value;
    }

    if (bits == 16)
        return static_cast<float> (static_cast<std::int16_t> (read16 (p))) / 32768.0f;

    if (bits == 24)
    {
        auto value = static_cast<std::int32_t> (p[0] | (p[1] << 8) | (p[2] << 16));
        if ((value & 0x800000) != 0)
            value -= 0x1000000;
        return static_cast<float> (value) / 8388608.0f;
    }

    return static_cast<float> (static_cast<double> (static_cast<std::int32_t> (read32 (p))) / 2147483648.0);
}

void put32 (std::vector<unsigned char>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back (static_cast<unsigned char> ((v >> (8 * i)) & 0xff));
}

void put16 (std::vector<unsigned char>& out, std::uint16_t v)
{
    out.push_back (static_cast<unsigned char> (v & 0xff));
    out.push_back (static_cast<unsigned char> (v >> 8));
}
} // namespace

std::optional<Audio> readWav (const std::filesystem::path& path, std::string& error)
{
    std::ifstream file (path, std::ios::binary);
    const std::vector<unsigned char> data ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char>());

    if (data.size() < 12 || std::memcmp (data.data(), "RIFF", 4) != 0 || std::memcmp (data.data() + 8, "WAVE", 4) != 0)
    {
        error = "not a WAV file: " + path.string();
        return std::nullopt;
    }

    int format = 0;
    int channels = 0;
    int bits = 0;
    double rate = 0.0;
    size_t pos = 12;

    while (pos + 8 <= data.size())
    {
        const auto* chunk = data.data() + pos;
        const auto size = static_cast<size_t> (read32 (chunk + 4));
        const auto* body = chunk + 8;

        if (std::memcmp (chunk, "fmt ", 4) == 0 && size >= 16)
        {
            format = read16 (body);
            channels = read16 (body + 2);
            rate = read32 (body + 4);
            bits = read16 (body + 14);

            if (format == 0xFFFE && size >= 26) // WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes
                format = read16 (body + 24);
        }
        else if (std::memcmp (chunk, "data", 4) == 0)
        {
            if (channels < 1 || channels > 2 || (format != 1 && format != 3) || bits % 8 != 0)
            {
                error = "unsupported WAV format: " + path.string();
                return std::nullopt;
            }

            const auto frameBytes = static_cast<size_t> (channels * bits / 8);
            const auto frames = std::min (size, data.size() - pos - 8) / frameBytes;
            Audio audio;
            audio.sampleRate = rate;
            audio.left.resize (frames);
            audio.right.resize (frames);

            for (size_t i = 0; i < frames; ++i)
            {
                const auto* frame = body + i * frameBytes;
                audio.left[i] = sampleAt (frame, format, bits);
                audio.right[i] = channels == 2 ? sampleAt (frame + bits / 8, format, bits) : audio.left[i];
            }

            return audio;
        }

        pos += 8 + size + (size & 1);
    }

    error = "no audio data in " + path.string();
    return std::nullopt;
}

bool writeWav (const std::filesystem::path& path, const Audio& audio)
{
    const auto frames = static_cast<std::uint32_t> (audio.left.size());
    const auto dataBytes = frames * 2 * 4;
    std::vector<unsigned char> out;
    out.reserve (44 + dataBytes);

    out.insert (out.end(), { 'R', 'I', 'F', 'F' });
    put32 (out, 36 + dataBytes);
    out.insert (out.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
    put32 (out, 16);
    put16 (out, 3); // IEEE float
    put16 (out, 2);
    put32 (out, static_cast<std::uint32_t> (audio.sampleRate));
    put32 (out, static_cast<std::uint32_t> (audio.sampleRate) * 8);
    put16 (out, 8);
    put16 (out, 32);
    out.insert (out.end(), { 'd', 'a', 't', 'a' });
    put32 (out, dataBytes);

    for (std::uint32_t i = 0; i < frames; ++i)
        for (const auto* channel : { &audio.left, &audio.right })
        {
            std::uint32_t bitsOf = 0;
            std::memcpy (&bitsOf, &(*channel)[i], sizeof bitsOf);
            put32 (out, bitsOf);
        }

    std::ofstream file (path, std::ios::binary);
    file.write (reinterpret_cast<const char*> (out.data()), static_cast<std::streamsize> (out.size())); // NOLINT: bytes
    return static_cast<bool> (file);
}

} // namespace harness
