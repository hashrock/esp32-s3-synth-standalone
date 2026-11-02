#include "dsp/envelope.h"

#include <cmath>

namespace dsp {

namespace {
float tau_to_coef(float tau, float sample_rate) {
    if (tau <= 0.0f) return 0.0f;
    return std::exp(-1.0f / (tau * sample_rate));
}
}  // namespace

void Envelope::configure(const AdsrParams &p, float sample_rate) {
    const float attack_samples = p.attack_sec * sample_rate;
    attack_inc_ = attack_samples >= 1.0f ? 1.0f / attack_samples : 1.0f;
    decay_coef_ = tau_to_coef(p.decay_tau, sample_rate);
    sustain_ = p.sustain;
    release_coef_ = tau_to_coef(p.release_tau, sample_rate);
}

}  // namespace dsp
