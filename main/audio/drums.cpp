// TR-808 風ドラム（アナログ回路の考え方を簡略化してデジタルで再現）
//
//  Kick/Tom : サイン波 + ピッチエンベロープ（打撃で音程が急降下）
//  Snare    : 2つのサイン波（胴鳴り）+ ハイパスしたノイズ（スナッピー）
//  Clap     : バンドパスしたノイズを短く3回鳴らし、残響を足す
//  Hi-hat   : 6つの矩形波（非整数比）を合成してハイパス → 金属音
//  Cowbell  : 2つの矩形波（540Hz/800Hz）をバンドパス
//  Rim      : 高いサイン波の短いクリック

#include "audio/drums.h"

#include <cmath>

#include "dsp/oscillator.h"

namespace drums {
namespace {

constexpr float kIdle = 1.0e-4f;
constexpr float kMetalHz[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};  // 808 のハイハット/シンバル発振器

// 808 クラップ: 10ms 間隔で 3 回のバースト、その後に残響
constexpr float kClapBurstSec = 0.010f;
constexpr int kClapBursts = 3;

float coef(float tau, float fs) { return std::exp(-1.0f / (tau * fs)); }
uint32_t inc(float hz, float fs) { return dsp::hz_to_inc(hz, fs); }

inline float square(uint32_t phase) { return (phase & 0x80000000u) ? -1.0f : 1.0f; }

}  // namespace

Kind kind_for_note(uint8_t note) {
    static constexpr Kind kMap[12] = {
        Kind::Kick,    Kind::Rim,       Kind::Snare,   Kind::Clap,      Kind::Snare2,  Kind::TomLow,
        Kind::ClosedHat, Kind::TomMid,  Kind::ClosedHat, Kind::TomHigh, Kind::OpenHat, Kind::Cowbell,
    };
    return kMap[note % 12];
}

void DrumVoice::trigger(Kind k, float velocity, float fs) {
    kind = k;
    t = 0;
    ph1 = ph2 = 0;
    inc1 = inc2 = sweep_inc = 0;
    pitch_env = pitch_coef = 0.0f;
    tone1 = tone1_coef = tone2 = tone2_coef = 0.0f;
    noise = noise_coef = 0.0f;
    filt.reset();
    rng.state ^= 0x9e3779b9u + (static_cast<uint32_t>(velocity * 1000.0f) << 8);
    if (rng.state == 0) rng.state = 1;

    float g = 1.0f;
    switch (k) {
    case Kind::Kick:
        inc1 = inc(48.0f, fs);
        sweep_inc = inc(110.0f, fs);
        pitch_env = 1.0f;
        pitch_coef = coef(0.035f, fs);
        tone1 = 1.0f;
        tone1_coef = coef(0.45f, fs);
        g = 1.0f;
        break;
    case Kind::TomLow:
    case Kind::TomMid:
    case Kind::TomHigh: {
        const float hz = k == Kind::TomLow ? 85.0f : (k == Kind::TomMid ? 120.0f : 165.0f);
        inc1 = inc(hz, fs);
        sweep_inc = inc(hz * 0.6f, fs);
        pitch_env = 1.0f;
        pitch_coef = coef(0.06f, fs);
        tone1 = 1.0f;
        tone1_coef = coef(0.11f, fs);
        g = 0.8f;
        break;
    }
    case Kind::Snare:
    case Kind::Snare2: {
        const bool snappy = k == Kind::Snare2;
        inc1 = inc(185.0f, fs);
        inc2 = inc(330.0f, fs);
        tone1 = snappy ? 0.35f : 0.6f;
        tone1_coef = coef(0.015f, fs);
        tone2 = snappy ? 0.25f : 0.4f;
        tone2_coef = coef(0.0105f, fs);
        noise = snappy ? 1.0f : 0.8f;
        noise_coef = coef(snappy ? 0.036f : 0.024f, fs);
        filt.configure(snappy ? 2500.0f : 1800.0f, 0.7f, fs);
        g = 0.7f;
        break;
    }
    case Kind::Rim:
        inc1 = inc(1700.0f, fs);
        inc2 = inc(500.0f, fs);
        tone1 = 1.0f;
        tone1_coef = coef(0.006f, fs);
        tone2 = 0.5f;
        tone2_coef = coef(0.01f, fs);
        g = 0.6f;
        break;
    case Kind::Clap:
        filt.configure(1100.0f, 2.0f, fs);
        tone2 = 1.0f;                     // バースト
        tone2_coef = coef(0.0025f, fs);
        noise = 0.0f;                     // 残響（バースト後に立ち上げる）
        noise_coef = coef(0.045f, fs);
        g = 2.2f;                         // 狭いバンドパスで下がる分を補う
        break;
    case Kind::ClosedHat:
    case Kind::OpenHat:
        for (int i = 0; i < 6; ++i) {
            metal_ph[i] = static_cast<uint32_t>(i) * 0x2468ace1u;
            metal_inc[i] = inc(kMetalHz[i], fs);
        }
        filt.configure(7000.0f, 0.8f, fs);
        noise = 1.0f;
        noise_coef = coef(k == Kind::ClosedHat ? 0.02f : 0.22f, fs);
        g = 0.5f;
        break;
    case Kind::Cowbell:
        inc1 = inc(540.0f, fs);
        inc2 = inc(800.0f, fs);
        filt.configure(1800.0f, 1.2f, fs);
        tone1 = 0.7f;                     // 立ち上がりの速い減衰
        tone1_coef = coef(0.015f, fs);
        tone2 = 0.3f;                     // 残り
        tone2_coef = coef(0.18f, fs);
        g = 0.9f;
        break;
    }
    gain = g * (0.2f + 0.8f * velocity);
    sample_rate_ = fs;
}

void DrumVoice::choke() { noise_coef = coef(0.005f, sample_rate_); }

bool DrumVoice::idle() const {
    if (kind == Kind::Clap && t < static_cast<uint32_t>(kClapBurstSec * kClapBursts * sample_rate_)) return false;
    return tone1 < kIdle && tone2 < kIdle && noise < kIdle;
}

float DrumVoice::level() const {
    float l = tone1 > tone2 ? tone1 : tone2;
    return noise > l ? noise : l;
}

void DrumVoice::render(float *mix, size_t frames) {
    switch (kind) {
    case Kind::Kick:
    case Kind::TomLow:
    case Kind::TomMid:
    case Kind::TomHigh:
        for (size_t i = 0; i < frames; ++i) {
            mix[i] += dsp::sine(ph1) * tone1 * gain;
            ph1 += inc1 + static_cast<uint32_t>(sweep_inc * pitch_env);
            pitch_env *= pitch_coef;
            tone1 *= tone1_coef;
        }
        break;

    case Kind::Snare:
    case Kind::Snare2:
        for (size_t i = 0; i < frames; ++i) {
            filt.process(rng.next());
            const float body = dsp::sine(ph1) * tone1 + dsp::sine(ph2) * tone2;
            mix[i] += (body + filt.hp * noise) * gain;
            ph1 += inc1;
            ph2 += inc2;
            tone1 *= tone1_coef;
            tone2 *= tone2_coef;
            noise *= noise_coef;
        }
        break;

    case Kind::Rim:
        for (size_t i = 0; i < frames; ++i) {
            mix[i] += (dsp::sine(ph1) * tone1 + dsp::sine(ph2) * tone2) * gain;
            ph1 += inc1;
            ph2 += inc2;
            tone1 *= tone1_coef;
            tone2 *= tone2_coef;
        }
        break;

    case Kind::Clap: {
        const uint32_t burst_len = static_cast<uint32_t>(kClapBurstSec * sample_rate_);
        for (size_t i = 0; i < frames; ++i) {
            if (t < burst_len * kClapBursts && t % burst_len == 0) tone2 = 1.0f;  // バースト再トリガ
            if (t == burst_len * (kClapBursts - 1)) noise = 0.8f;                 // 残響の立ち上げ
            filt.process(rng.next());
            const float env = tone2 > noise ? tone2 : noise;
            mix[i] += filt.bp * filt.k * env * gain;
            tone2 *= tone2_coef;
            noise *= noise_coef;
            ++t;
        }
        break;
    }

    case Kind::ClosedHat:
    case Kind::OpenHat:
        for (size_t i = 0; i < frames; ++i) {
            float m = 0.0f;
            for (int j = 0; j < 6; ++j) {
                m += square(metal_ph[j]);
                metal_ph[j] += metal_inc[j];
            }
            filt.process(m * (1.0f / 6.0f) + 0.3f * rng.next());
            mix[i] += filt.hp * noise * gain;
            noise *= noise_coef;
        }
        break;

    case Kind::Cowbell:
        for (size_t i = 0; i < frames; ++i) {
            filt.process(0.5f * (square(ph1) + square(ph2)));
            mix[i] += filt.bp * filt.k * (tone1 + tone2) * gain;
            ph1 += inc1;
            ph2 += inc2;
            tone1 *= tone1_coef;
            tone2 *= tone2_coef;
        }
        break;
    }
}

}  // namespace drums
