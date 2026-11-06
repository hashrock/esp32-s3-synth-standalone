#include "audio/audio_output.h"

#include <atomic>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace audio {
namespace {

constexpr const char *TAG = "audio";

// 暫定GPIO割り当て（PCM5102A）
constexpr gpio_num_t kPinBclk = GPIO_NUM_16;
constexpr gpio_num_t kPinLrck = GPIO_NUM_17;
constexpr gpio_num_t kPinDout = GPIO_NUM_18;

constexpr BaseType_t kAudioCore = 1;
constexpr UBaseType_t kAudioPriority = configMAX_PRIORITIES - 2;

i2s_chan_handle_t s_tx = nullptr;
RenderFn s_render = nullptr;
void *s_render_ctx = nullptr;

std::atomic<uint32_t> s_underruns{0};
std::atomic<uint32_t> s_blocks{0};
std::atomic<uint32_t> s_render_us_max{0};
std::atomic<uint32_t> s_render_us_sum{0};
std::atomic<uint32_t> s_render_count{0};
std::atomic<uint32_t> s_missed_deadlines{0};
std::atomic<uint32_t> s_cycles_max{0};
std::atomic<uint32_t> s_cycles_sum{0};

// ISR: 送信イベントキューの overflow。物理的なDAC underrunの直接計測ではない。
bool IRAM_ATTR on_send_q_ovf(i2s_chan_handle_t, i2s_event_data_t *, void *) {
    s_underruns.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// モノラル→ステレオ（L/Rに同じ値）
void mono_to_stereo(const int16_t *mono, int16_t *stereo, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        stereo[2 * i] = mono[i];
        stereo[2 * i + 1] = mono[i];
    }
}

void audio_task(void *) {
    // タスクスタック上の固定長バッファ（音声処理中の動的確保なし）
    int16_t mono[kBlockFrames];
    int16_t stereo[kBlockFrames * 2];

    for (;;) {
        const int64_t t0 = esp_timer_get_time();
        const uint32_t c0 = esp_cpu_get_cycle_count();
        s_render(mono, kBlockFrames, s_render_ctx);
        const uint32_t cycles = esp_cpu_get_cycle_count() - c0;
        s_cycles_sum.fetch_add(cycles, std::memory_order_relaxed);
        if (cycles > s_cycles_max.load(std::memory_order_relaxed))
            s_cycles_max.store(cycles, std::memory_order_relaxed);
        const uint32_t dt = static_cast<uint32_t>(esp_timer_get_time() - t0);

        if (dt > 1000000ULL * kBlockFrames / kSampleRate)
            s_missed_deadlines.fetch_add(1, std::memory_order_relaxed);
        uint32_t prev = s_render_us_max.load(std::memory_order_relaxed);
        if (dt > prev) s_render_us_max.store(dt, std::memory_order_relaxed);
        s_render_us_sum.fetch_add(dt, std::memory_order_relaxed);
        s_render_count.fetch_add(1, std::memory_order_relaxed);

        mono_to_stereo(mono, stereo, kBlockFrames);

        size_t written = 0;
        // DMAバッファに空きができるまでブロック → これがオーディオクロックに同期する
        i2s_channel_write(s_tx, stereo, sizeof(stereo), &written, portMAX_DELAY);
        s_blocks.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

esp_err_t start(RenderFn render, void *ctx) {
    s_render = render;
    s_render_ctx = ctx;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kDmaDescNum;
    chan_cfg.dma_frame_num = kBlockFrames;
    chan_cfg.auto_clear = true;  // アンダーラン時はゴミではなく無音を出す
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, nullptr), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,  // PCM5102Aは内部PLLでBCKからクロック生成（SCK=GND）
            .bclk = kPinBclk,
            .ws = kPinLrck,
            .dout = kPinDout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s_channel_init_std_mode");

    i2s_event_callbacks_t cbs = {};
    cbs.on_send_q_ovf = on_send_q_ovf;
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_tx, &cbs, nullptr), TAG, "register cb");

    // 開始直後のアンダーランを避けるため、DMAバッファを無音で埋めてから有効化
    static int16_t silence[kBlockFrames * 2] = {};
    for (uint32_t i = 0; i < kDmaDescNum; ++i) {
        size_t loaded = 0;
        i2s_channel_preload_data(s_tx, silence, sizeof(silence), &loaded);
        if (loaded < sizeof(silence)) break;
    }
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s_channel_enable");

    ESP_LOGI(TAG, "I2S started: %lu Hz, 16bit stereo, block=%u frames, dma_desc=%lu (buffer %.2f ms)",
             (unsigned long)kSampleRate, (unsigned)kBlockFrames, (unsigned long)kDmaDescNum,
             1000.0 * kBlockFrames * kDmaDescNum / kSampleRate);
    ESP_LOGI(TAG, "pins: BCLK=%d LRCK=%d DOUT=%d", kPinBclk, kPinLrck, kPinDout);

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, kAudioPriority,
                                            nullptr, kAudioCore);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

Stats take_stats() {
    Stats st{};
    st.missed_deadlines = s_missed_deadlines.load(std::memory_order_relaxed);
    st.render_cycles_max = s_cycles_max.exchange(0, std::memory_order_relaxed);
    const uint32_t cycles = s_cycles_sum.exchange(0, std::memory_order_relaxed);
    st.underruns = s_underruns.load(std::memory_order_relaxed);
    st.blocks = s_blocks.load(std::memory_order_relaxed);
    st.render_us_max = s_render_us_max.exchange(0, std::memory_order_relaxed);
    const uint32_t sum = s_render_us_sum.exchange(0, std::memory_order_relaxed);
    const uint32_t cnt = s_render_count.exchange(0, std::memory_order_relaxed);
    st.render_us_avg = cnt ? sum / cnt : 0;
    st.render_cycles_avg = cnt ? cycles / cnt : 0;
    return st;
}

}  // namespace audio
