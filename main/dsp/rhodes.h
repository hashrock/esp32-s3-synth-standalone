#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define RHODES_HOT IRAM_ATTR
#else
#define RHODES_HOT
#endif

namespace rhodes {
struct Settings {
    double stiffness = .43;
    double damping = .35;
    double cutoff = 3486.08;
    double drive = .1625;
    double release_seconds = .07;
    double pickup_mix = 0;
    double pickup_gap = .001;
    double pickup_offset = .00022;
    bool velocity_timbre = false;
    bool saturate_output = true;
};
inline constexpr Settings kBark{.39, .01, 10000, .25, .041, .60, .00132, -.00011, true, false};
inline constexpr Settings kVelvet{.28, .22, 1900, .145, .10};

// Three Newton refinements of reciprocal sqrt, then sqrt(x)=x*rsqrt(x).
// Positive normal contact compressions only. Avoid software sqrtf on Xtensa.
template <typename T> inline T contact_sqrt(T x) {
    if constexpr (std::is_same<T, float>::value) {
        uint32_t bits = __builtin_bit_cast(uint32_t, x);
        bits = 0x5f375a86u - (bits >> 1);
        float y = __builtin_bit_cast(float, bits);
        const float half = .5f*x;
        y *= 1.5f - half*y*y;
        y *= 1.5f - half*y*y;
        y *= 1.5f - half*y*y;
        return x*y;
    } else return std::sqrt(x);
}

// Mode 02 of rhodes-physics-lab.html, with its default knob settings.
// Shared bank is prepared before audio starts. No allocation or transcendental
// functions in the audio loop (sqrt is needed only during hammer contact).
template <typename T> struct Bank {
    static constexpr T pi = T(3.14159265358979323846);
    struct Mode { T w2, gamma, force, shape, a, b, c, d, inv_w, step_v, step_q, step_f; };
    struct Note { Mode modes[5]{}; T dt; int oversampling; int count = 0; } notes[128];
    T alpha, dc_alpha, release, stiffness, drive;
    struct Tone { T alpha, drive, pickup_position_scale; } tones[128];
    T saturation[1025];
    T pickup_table[1025];
    bool nonlinear_pickup = false;
    bool saturate_output = true;
    uint32_t max_frames;
    T sample_rate;

    void init(T sr = T(48000), int os = 4, bool adaptive = false, const Settings &settings = {}) {
        sample_rate = sr;
        drive = T(settings.drive);
        nonlinear_pickup = settings.pickup_mix != 0;
        saturate_output = settings.saturate_output;
        max_frames = static_cast<uint32_t>(sr * 16);
        alpha = 1 - std::exp(-2 * pi * T(settings.cutoff) / sr);
        for (int velocity=0; velocity<128; ++velocity) {
            const double v=velocity/127.0;
            const double strength=settings.velocity_timbre ? v*v : 1;
            const double cutoff=settings.cutoff*(.26+.74*strength);
            tones[velocity] = {
                1-std::exp(-2*pi*T(cutoff)/sr),
                T(settings.drive*(settings.saturate_output ? .45+.55*strength : 1)),
                T(64000*(.12+.88*strength))
            };
        }
        dc_alpha = 1 - std::pow(T(1) - T(.0006), T(44100) / sr);
        release = std::exp(-1 / (T(settings.release_seconds) * sr));
        stiffness = T(2.5e7) * std::exp(T((settings.stiffness - .5) * 2.3));
        for (int i = 0; i <= 1024; ++i) saturation[i] = std::tanh(T(i) * T(8.0 / 1024));
        // Same bounded dPhi/dx model as prototype mode 03, baked off the audio
        // thread. Position span +/-8mm; outside, clamp to the far-field endpoint response.
        const double nominal_t = -settings.pickup_offset/settings.pickup_gap;
        const double nominal = 2*nominal_t/std::pow(1+nominal_t*nominal_t,2);
        for (int i=0; i<=1024; ++i) {
            const double x = (i-512)*(.016/1024);
            const double t = (x-settings.pickup_offset)/settings.pickup_gap;
            const double derivative = 2*t/std::pow(1+t*t,2);
            const double factor = nominal == 0 ? 1 : std::max(-3.0,std::min(3.0,derivative/nominal));
            pickup_table[i] = T(1-settings.pickup_mix + settings.pickup_mix*factor);
        }
        constexpr double ratios[] = {1, 6.267, 17.548, 34.386, 56.84};
        constexpr double shapes[] = {1, -.43, .2, -.1, .05};
        for (int n = 0; n < 128; ++n) {
            auto &note = notes[n];
            note.count = 0;
            const T f = T(440) * std::pow(T(2), T(n - 69) / 12);
            note.oversampling = adaptive && f*T(6.267) <= std::min(T(11000), sr*T(.4)) ? 2 : os;
            note.dt = 1 / (sr * note.oversampling);
            for (int j = 0; j < 5; ++j) {
                const T hz = f * T(ratios[j]);
                // Keep the same mode set in 2x/4x/8x convergence experiments.
                // The reference drops fundamentals above 11 kHz (MIDI 125–127).
                if (hz > T(11000) || hz > sr * T(.4)) break;
                auto &m = note.modes[note.count++];
                const T w = 2 * pi * hz;
                m.w2 = w * w;
                m.inv_w = 1 / w;
                m.gamma = T(.42 + 2*settings.damping) * (j == 0 ? T(1) : std::pow(T(5), T(j)) * T(.25));
                m.shape = T(shapes[j]);
                // Store pickup-weighted displacement/velocity: Q=shape*q,
                // V=shape*v. Contact and pickup now sum Q/V without multiplies.
                m.force = m.shape * m.shape / (T(.004) * (1 + T(.85) * j));
                m.step_v = 1 - 2*m.gamma*note.dt;
                m.step_q = -m.w2*note.dt;
                m.step_f = m.force*note.dt;
                const T wd = std::sqrt(m.w2 - m.gamma * m.gamma);
                const T e = std::exp(-m.gamma / sr);
                const T s = std::sin(wd / sr) / wd;
                const T c = std::cos(wd / sr);
                m.a = e * (c + m.gamma * s);
                m.b = e * s;
                m.c = -e * m.w2 * s;
                m.d = e * (c - m.gamma * s);
            }
        }
    }
    T pickup_gain(T position, T position_scale = T(64000)) const {
        const T index = position*position_scale + T(512);
        if (index <= 0) return pickup_table[0];
        if (index >= 1024) return pickup_table[1024];
        const int i = static_cast<int>(index);
        return pickup_table[i] + (index-i)*(pickup_table[i+1]-pickup_table[i]);
    }
    T saturate(T x) const {
        const T pos = std::abs(x) * T(128);
        if (pos >= 1024) return x < 0 ? T(-1) : T(1);
        const int i = static_cast<int>(pos);
        const T y = saturation[i] + (pos - i) * (saturation[i + 1] - saturation[i]);
        return x < 0 ? -y : y;
    }
};

template <typename T> class BasicVoice {
public:
    void note_on(const Bank<T> &bank, uint8_t note, uint8_t velocity, bool optimize = true) {
        bank_ = &bank;
        const auto &tone=bank.tones[velocity & 127];
        alpha_=tone.alpha;
        drive_=tone.drive;
        pickup_position_scale_=tone.pickup_position_scale;
        note_ = &bank.notes[note & 127];
        for (int j = 0; j < 5; ++j) q_[j] = v_[j] = 0;
        hx_ = T(-.0007);
        hv_ = T(.35) + T(2.8) * T(velocity & 127) / 127;
        lp_ = dc_ = last_ = tip_x_ = tip_v_ = 0;
        gain_ = 1;
        released_ = free_ = false;
        optimize_ = optimize;
        active_ = velocity != 0 && note_->count != 0;
        age_ = handover_ = contacts_ = 0;
    }
    void note_off() { released_ = true; }
    bool idle() const { return !active_; }
    T level() const { return std::abs(last_); }
    uint32_t handover_frame() const { return handover_; }
    uint32_t contact_samples() const { return contacts_; }
    T process() {
        if (!active_) return 0;
        return bank_->nonlinear_pickup ? process_dispatch<true>() : process_dispatch<false>();
    }
    template<bool Pickup> T process_dispatch() {
        switch (note_->count) {
        case 1: return process_modes<1, Pickup>();
        case 2: return process_modes<2, Pickup>();
        case 3: return process_modes<3, Pickup>();
        case 4: return process_modes<4, Pickup>();
        case 5: return process_modes<5, Pickup>();
        default: return 0;
        }
    }
    template<int N, bool Pickup> __attribute__((always_inline)) inline T process_modes() {
        const auto &b = *bank_;
        T velocity = 0;
        T position = 0;
        if (free_) {
            _Pragma("GCC unroll 5")
            for (int j = 0; j < N; ++j) {
                const auto &m = note_->modes[j];
                const T q = q_[j], v = v_[j];
                q_[j] = m.a * q + m.b * v;
                v_[j] = m.c * q + m.d * v;
                velocity += v_[j];
                if constexpr (Pickup) position += q_[j];
            }
        } else {
            T x = tip_x_, v = tip_v_;
            for (int sub = 0; sub < note_->oversampling; ++sub) {
                const T compression = hx_ - x;
                T force = 0;
                const T damping = 1 + T(1.482) * (hv_ - v);
                if (compression > 0 && damping > 0) {
                    force = std::min(T(150), b.stiffness * compression * contact_sqrt(compression) * damping);
                }
                if (force > T(.005)) ++contacts_;
                hv_ -= force * T(100) * note_->dt;
                hx_ += hv_ * note_->dt;
                x = v = 0;
                _Pragma("GCC unroll 5")
                for (int j = 0; j < N; ++j) {
                    const auto &m = note_->modes[j];
                    v_[j] = m.step_v*v_[j] + m.step_q*q_[j] + m.step_f*force;
                    q_[j] += v_[j] * note_->dt;
                    x += q_[j];
                    v += v_[j];
                }
            }
            tip_x_ = x;
            if constexpr (Pickup) position = x;
            tip_v_ = velocity = v;
            // Under free continuous motion E=(v²+w²q²)/2 is nonincreasing.
            // Weighted Q/V states give the bound sqrt(Q²+V²/w²).
            // A receding hammer beyond this bound cannot ever hit the tine again.
            // Preserve both q and v; transition only at an output sample boundary.
            if (optimize_ && hv_ < 0 && hx_ < 0) {
                T bound = 0;
                _Pragma("GCC unroll 5")
                for (int j = 0; j < N; ++j) {
                    const auto &m = note_->modes[j];
                    // L1 bound is conservative: |Q|+|V|/w >= sqrt(Q²+V²/w²).
                    bound += std::abs(q_[j]) + std::abs(v_[j])*m.inv_w;
                }
                if (hx_ < -T(1.01) * bound) { free_ = true; handover_ = age_ + 1; }
            }
        }
        if constexpr (Pickup) velocity *= b.pickup_gain(position, pickup_position_scale_);
        const T voltage = velocity * (T(1) / (2 * Bank<T>::pi * T(220) * T(.0004)));
        lp_ += alpha_ * (voltage - lp_);
        dc_ += b.dc_alpha * (lp_ - dc_);
        if (released_) gain_ *= b.release;
        const T linear = (lp_ - dc_) * drive_;
        last_ = (b.saturate_output ? b.saturate(linear) : linear) * T(.62) * gain_;
        ++age_;
        // No abrupt 4.2-second buffer end: allow natural decay, or 70ms release.
        if (gain_ < T(1e-5) || age_ >= b.max_frames) active_ = false;
        return last_;
    }
    // Dispatch once per block, inline the state update inside the IRAM loop.
    // Mixing here also avoids per-sample calls across the flash/IRAM boundary.
    void render_mixed(T *mix, size_t frames, T gain, T &tail, T &last) {
        if (bank_->nonlinear_pickup) render_dispatch<true>(mix,frames,gain,tail,last);
        else render_dispatch<false>(mix,frames,gain,tail,last);
    }
    template<bool Pickup> void render_dispatch(T *mix, size_t frames, T gain, T &tail, T &last) {
        switch (note_->count) {
        case 1: render_modes<1, Pickup>(mix, frames, gain, tail, last); break;
        case 2: render_modes<2, Pickup>(mix, frames, gain, tail, last); break;
        case 3: render_modes<3, Pickup>(mix, frames, gain, tail, last); break;
        case 4: render_modes<4, Pickup>(mix, frames, gain, tail, last); break;
        case 5: render_modes<5, Pickup>(mix, frames, gain, tail, last); break;
        }
    }
    template<int N, bool Pickup> RHODES_HOT void render_modes(T *__restrict__ mix, size_t frames, T gain, T &tail, T &last) {
        T t = tail, y = 0;
        for (size_t i = 0; i < frames; ++i) {
            y = (active_ ? process_modes<N, Pickup>() : T(0)) + t;
            t *= T(.9862071);
            mix[i] += y * gain;
        }
        tail = t;
        last = y;
    }
    // Add mono samples to caller's mix buffer.
    void render(T *mix, size_t frames) { for (size_t i = 0; i < frames; ++i) mix[i] += process(); }
private:
    const Bank<T> *bank_ = nullptr;
    const typename Bank<T>::Note *note_ = nullptr;
    T alpha_ = 0, drive_ = 0, pickup_position_scale_ = T(64000);
    T tip_x_ = 0, tip_v_ = 0;
    T q_[5]{}, v_[5]{}, hx_ = 0, hv_ = 0, lp_ = 0, dc_ = 0, gain_ = 0, last_ = 0;
    uint32_t age_ = 0, handover_ = 0, contacts_ = 0;
    bool active_ = false, released_ = false, free_ = false, optimize_ = true;
};
using Voice = BasicVoice<float>;
} // namespace rhodes
#undef RHODES_HOT
