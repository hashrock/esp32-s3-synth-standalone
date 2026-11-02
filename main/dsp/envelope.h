#pragma once

#include <cstdint>

namespace dsp {

struct AdsrParams {
    float attack_sec;   // 線形アタック時間
    float decay_tau;    // サステインレベルへ向かう指数減衰の時定数 [s]
    float sustain;      // 0.0〜1.0
    float release_tau;  // 0 へ向かう指数減衰の時定数 [s]
};

// ADSR エンベロープ（アタックは線形、ディケイ/リリースは指数）
class Envelope {
public:
    enum class Stage : uint8_t { Idle, Attack, Decay, Release };

    // パラメータから1サンプルあたりの係数を計算する（音声タスク外/ノートオン時に呼ぶ）
    void configure(const AdsrParams &p, float sample_rate);

    // 現在のレベルから再アタック（ボイス奪取・再打鍵でクリックを出さないため 0 に戻さない）
    void gate_on() { stage_ = Stage::Attack; }
    void reset() {
        stage_ = Stage::Idle;
        level_ = 0.0f;
    }
    void gate_off() {
        if (stage_ != Stage::Idle) stage_ = Stage::Release;
    }
    bool idle() const { return stage_ == Stage::Idle; }
    bool releasing() const { return stage_ == Stage::Release; }
    float level() const { return level_; }

    inline float process() {
        switch (stage_) {
        case Stage::Attack:
            level_ += attack_inc_;
            if (level_ >= 1.0f) {
                level_ = 1.0f;
                stage_ = Stage::Decay;
            }
            break;
        case Stage::Decay:
            level_ = sustain_ + (level_ - sustain_) * decay_coef_;
            if (level_ < kIdleLevel && sustain_ <= 0.0f) stage_ = Stage::Idle;
            break;
        case Stage::Release:
            level_ *= release_coef_;
            if (level_ < kIdleLevel) stage_ = Stage::Idle;
            break;
        case Stage::Idle:
            level_ = 0.0f;
            break;
        }
        return level_;
    }

private:
    static constexpr float kIdleLevel = 1.0e-4f;  // -80dB

    Stage stage_ = Stage::Idle;
    float level_ = 0.0f;
    float attack_inc_ = 1.0f;
    float decay_coef_ = 0.0f;
    float sustain_ = 0.0f;
    float release_coef_ = 0.0f;
};

}  // namespace dsp
