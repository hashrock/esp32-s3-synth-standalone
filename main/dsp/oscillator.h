#pragma once

#include <cstdint>

namespace dsp {

// 位相は 32bit 固定小数点（2^32 = 1周期）
constexpr int kSineBits = 11;
constexpr int kSineSize = 1 << kSineBits;
extern float g_sine[kSineSize + 1];  // 線形補間用に1要素多い

void init_sine_table();

inline float sine(uint32_t phase) {
    constexpr int kFracBits = 32 - kSineBits;
    constexpr float kFracScale = 1.0f / (1u << kFracBits);
    const uint32_t idx = phase >> kFracBits;
    const float frac = (phase & ((1u << kFracBits) - 1)) * kFracScale;
    const float a = g_sine[idx];
    return a + (g_sine[idx + 1] - a) * frac;
}

inline uint32_t hz_to_inc(float hz, float sample_rate) {
    return static_cast<uint32_t>(hz / sample_rate * 4294967296.0f);
}

}  // namespace dsp
