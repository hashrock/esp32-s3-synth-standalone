#pragma once

#include <cstddef>
#include <cstdint>

namespace synth {

constexpr int kNumVoices = 16;
constexpr int kNumPresets = 4;

// シンセを初期化する（audio::start より前に呼ぶ）
void init();

// ノートイベントを送る。どのタスクからでも呼べる（キューが満杯なら false）
bool note_on(uint8_t note, uint8_t velocity);
bool note_off(uint8_t note);
bool all_notes_off();
bool sustain(bool on);  // サステインペダル（CC64）

// 音色プリセットの切り替え（次に弾いたノートから反映）
bool set_preset(int index);
int preset();
const char *preset_name(int index);

// audio::RenderFn として渡すレンダリング関数
void render(int16_t *out, size_t frames, void *ctx);

// 統計
int active_voices();
uint32_t voice_steals();
uint32_t clipped_samples();  // ソフトクリップ域に入ったサンプル数（累計）
float take_peak();           // 前回呼び出し以降のピーク（クリップ前、1.0 = フルスケール）

}  // namespace synth
