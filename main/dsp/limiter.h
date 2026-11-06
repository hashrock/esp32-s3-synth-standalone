#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace dsp {
// One-block lookahead: ramp the pending block's gain in anticipation of the
// incoming block. Both ends stay below the pending block's safe gain, so every
// sample is bounded without waveshaping. Fixed-size audio blocks only.
template<size_t Frames> class LookaheadLimiter {
public:
    static constexpr float kCeiling = .95f;
    void init(float sample_rate) {
        release_step_ = 1-std::exp(-float(Frames)/(sample_rate*.080f));
        gain_ = 1;
        pending_peak_ = 0;
        for (float &x : pending_) x = 0;
    }
    void process(float *samples, float input_peak) {
        const float pending_safe = pending_peak_ > kCeiling ? kCeiling/pending_peak_ : 1;
        const float incoming_safe = input_peak > kCeiling ? kCeiling/input_peak : 1;
        const float end_gain = std::min(std::min(pending_safe,incoming_safe),
                                       gain_+(1-gain_)*release_step_);
        const float step = (end_gain-gain_)/Frames;
        for (size_t i=0; i<Frames; ++i) {
            const float incoming = samples[i];
            const float gain = std::min(pending_safe,gain_+float(i+1)*step);
            samples[i] = pending_[i]*gain;
            pending_[i] = incoming;
        }
        gain_ = end_gain;
        pending_peak_ = input_peak;
    }
private:
    float pending_[Frames]{};
    float pending_peak_ = 0, gain_ = 1, release_step_ = 0;
};
} // namespace dsp
