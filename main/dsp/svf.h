#pragma once

#include <cmath>
#include <cstdint>

namespace dsp {

// ステートバリアブルフィルタ（TPT 型、Zavalishin）。LP/BP/HP を同時に得る
struct Svf {
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;
    float ic1 = 0.0f, ic2 = 0.0f;
    float lp = 0.0f, bp = 0.0f, hp = 0.0f;

    // ノートオン時に呼ぶ（tan を使うので音声サンプルごとには呼ばない）
    void configure(float cutoff_hz, float q, float sample_rate) {
        const float g = std::tan(3.14159265f * cutoff_hz / sample_rate);
        k = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() { ic1 = ic2 = 0.0f; }

    inline void process(float x) {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = x - k * v1 - v2;
    }
};

// xorshift32 ホワイトノイズ（-1〜1）
struct Noise {
    uint32_t state = 0x12345678u;
    inline float next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<int32_t>(state) * (1.0f / 2147483648.0f);
    }
};

}  // namespace dsp
