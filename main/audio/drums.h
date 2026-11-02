#pragma once

#include <cstddef>
#include <cstdint>

#include "dsp/svf.h"

namespace drums {

enum class Kind : uint8_t { Kick, Rim, Snare, Snare2, Clap, TomLow, TomMid, TomHigh, ClosedHat, OpenHat, Cowbell };

// 鍵盤（どのオクターブでも同じ並び）→ 楽器。GM ドラム配置を 1 オクターブに畳んだもの
//   C: Kick  C#: Rim  D: Snare  D#: Clap  E: Snare2  F: Low Tom  F#: Closed Hat
//   G: Mid Tom  G#: Closed Hat  A: High Tom  A#: Open Hat  B: Cowbell
Kind kind_for_note(uint8_t note);

// TR-808 風の1ショット音源ボイス（ノートオフは無視、自然減衰で終わる）
struct DrumVoice {
    Kind kind = Kind::Kick;
    float gain = 0.0f;

    // 音程成分（キック・タム・スネアの胴鳴り・リム）
    uint32_t ph1 = 0, ph2 = 0;
    uint32_t inc1 = 0, inc2 = 0;
    uint32_t sweep_inc = 0;    // ピッチエンベロープ 1.0 のときに inc1 に足す量
    float pitch_env = 0.0f, pitch_coef = 0.0f;
    float tone1 = 0.0f, tone1_coef = 0.0f;
    float tone2 = 0.0f, tone2_coef = 0.0f;

    // ノイズ / メタリック成分
    float noise = 0.0f, noise_coef = 0.0f;
    uint32_t metal_ph[6] = {};
    uint32_t metal_inc[6] = {};
    dsp::Svf filt;
    dsp::Noise rng;
    uint32_t t = 0;  // 経過サンプル数（クラップのバースト用）
    float sample_rate_ = 48000.0f;

    void trigger(Kind k, float velocity, float sample_rate);
    void choke();  // オープンハイハットをクローズドで止める
    bool idle() const;
    float level() const;
    void render(float *mix, size_t frames);
};

}  // namespace drums
