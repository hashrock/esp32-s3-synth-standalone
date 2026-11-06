#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace audio {

constexpr uint32_t kSampleRate = 48000;
// 1回のレンダリング/DMAバッファあたりのフレーム数（1フレーム = L+R）
constexpr size_t kBlockFrames = 64;
// DMAディスクリプタ数。総バッファ遅延 ≒ kBlockFrames * kDmaDescNum / kSampleRate
constexpr uint32_t kDmaDescNum = 4;

// モノラルのサンプル列を生成するコールバック。out に frames 個書き込む。
// オーディオタスク上で呼ばれるため、動的確保・ブロッキング処理・ログ出力を行わないこと。
using RenderFn = void (*)(int16_t *out, size_t frames, void *ctx);

struct Stats {
    uint32_t missed_deadlines; // render time exceeds one block (cumulative)
    uint32_t render_cycles_max;
    uint32_t render_cycles_avg;
    uint32_t underruns;       // I2S送信イベントキューのoverflow回数（累計、旧名称）
    uint32_t blocks;          // 出力したブロック数（累計）
    uint32_t render_us_max;   // 直近の統計区間でのレンダリング最大時間 [us]
    uint32_t render_us_avg;   // 直近の統計区間でのレンダリング平均時間 [us]
};

// I2Sを初期化し、オーディオタスクを起動する。
esp_err_t start(RenderFn render, void *ctx);

// 統計を取得し、区間統計（max/avg）をリセットする。
Stats take_stats();

}  // namespace audio
