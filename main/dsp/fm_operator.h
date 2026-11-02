#pragma once

#include <cstdint>

#include "dsp/oscillator.h"

namespace dsp {

// 変調量 [rad] → 位相オフセット（32bit 固定小数点）
// |rad| < 約12.5 の範囲で有効（呼び出し側で変調指数を制限する）
inline uint32_t radians_to_phase(float rad) {
    constexpr float kRadToQuarterCycles = 1073741824.0f / 6.283185307f;  // 2^30 / 2π
    return static_cast<uint32_t>(static_cast<int32_t>(rad * kRadToQuarterCycles)) << 2;
}

// サイン波オペレーター（位相変調入力付き）
struct FmOperator {
    uint32_t phase = 0;
    uint32_t inc = 0;

    inline float process(uint32_t phase_mod) {
        const float s = sine(phase + phase_mod);
        phase += inc;
        return s;
    }
};

}  // namespace dsp
