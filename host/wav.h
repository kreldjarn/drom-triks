#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Minimal 16-bit stereo PCM writer. Enough to drop the result into a DAW or
// play it with afplay; nothing here needs to be clever.

namespace drom::host {

inline bool WriteWav(const std::string        &path,
                     const std::vector<float> &interleaved_stereo,
                     uint32_t                  sample_rate)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if(!f)
        return false;

    const uint32_t frames      = static_cast<uint32_t>(interleaved_stereo.size() / 2);
    const uint16_t channels    = 2;
    const uint16_t bits        = 16;
    const uint32_t byte_rate   = sample_rate * channels * bits / 8;
    const uint16_t block_align = channels * bits / 8;
    const uint32_t data_bytes  = frames * block_align;

    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f);
    u32(36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1); // PCM
    u16(channels);
    u32(sample_rate);
    u32(byte_rate);
    u16(block_align);
    u16(bits);
    std::fwrite("data", 1, 4, f);
    u32(data_bytes);

    for(float s : interleaved_stereo)
    {
        // Clamp before conversion: a float overshoot would wrap and click.
        if(s > 1.f) s = 1.f;
        if(s < -1.f) s = -1.f;
        const int16_t v = static_cast<int16_t>(s * 32767.f);
        std::fwrite(&v, 2, 1, f);
    }

    std::fclose(f);
    return true;
}

} // namespace drom::host
