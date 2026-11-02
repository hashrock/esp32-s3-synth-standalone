// Phase 3: USB MIDI 鍵盤で 2オペレーター FM エレピを演奏する
//
// USB MIDI タスク → synth::note_on/off（キュー経由）→ 音声タスク
// kRunTestSequence = true にすると Phase 2 の内部テストシーケンスを流す

#include <cmath>
#include <cstdint>

#include "audio/audio_output.h"
#include "audio/synth.h"
#include "midi/usb_midi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "main";

constexpr bool kRunTestSequence = false;
// true にすると起動時に全ボイス同時発音の負荷テストを流す（CPU 負荷の確認用）
constexpr bool kRunStressTest = false;
// 受信する MIDI チャンネル（0〜15）。-1 で全チャンネル（オムニ）
constexpr int kMidiChannel = -1;

// ---- MIDI → シンセ ----------------------------------------------------------

int s_preset = 0;  // USB MIDI タスク内でのみ触る

void set_preset(int index) {
    s_preset = index % synth::kNumPresets;
    synth::set_preset(s_preset);
    ESP_LOGI(TAG, "preset %d: %s", s_preset, synth::preset_name(s_preset));
}

void on_midi(uint8_t status, uint8_t d1, uint8_t d2) {
    const uint8_t type = status & 0xf0;
    const uint8_t ch = status & 0x0f;
    if (kMidiChannel >= 0 && ch != kMidiChannel) return;

    switch (type) {
    case 0x90:  // Note On（vel 0 は Note Off として synth 側で処理）
        synth::note_on(d1, d2);
        break;
    case 0x80:
        synth::note_off(d1);
        break;
    case 0xB0:  // Control Change
        if (d1 == 1) {
            // モジュレーションホイールの位置でプリセットを選ぶ（0〜127 を均等に分割）
            const int idx = d2 * synth::kNumPresets / 128;
            if (idx != s_preset) set_preset(idx);
        } else if (d1 == 64) {
            synth::sustain(d2 >= 64);
        } else if (d1 == 120 || d1 == 123) {
            synth::all_notes_off();  // All Sound Off / All Notes Off
        } else {
            ESP_LOGI(TAG, "CC ch%u #%u = %u", ch + 1, d1, d2);
        }
        break;
    case 0xC0:  // Program Change
        set_preset(d1);
        break;
    case 0xE0:  // Pitch Bend（未対応。連続で届くのでログも出さない）
        break;
    default:
        ESP_LOGI(TAG, "MIDI ch%u status 0x%02x %u %u", ch + 1, type, d1, d2);
        break;
    }
}

void on_midi_connect(bool connected) {
    if (!connected) synth::all_notes_off();  // 切断時に音が鳴りっぱなしにならないように
}

constexpr int kBpm = 84;
constexpr int kBeatMs = 60000 / kBpm;

void wait_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void chord_on(const uint8_t *notes, int n, uint8_t vel) {
    for (int i = 0; i < n; ++i) synth::note_on(notes[i], vel);
}
void chord_off(const uint8_t *notes, int n) {
    for (int i = 0; i < n; ++i) synth::note_off(notes[i]);
}

// 1) ベロシティによる音量・音色変化
void test_velocity() {
    ESP_LOGI(TAG, "[seq] velocity sweep (C4)");
    const uint8_t vels[] = {20, 45, 70, 95, 120, 127};
    for (uint8_t v : vels) {
        synth::note_on(60, v);
        wait_ms(700);
        synth::note_off(60);
        wait_ms(200);
    }
    wait_ms(600);
}

// 2) 音域ごとの響き（キースケーリング確認）
void test_range() {
    ESP_LOGI(TAG, "[seq] range C2..C6");
    for (uint8_t n = 36; n <= 84; n += 12) {
        synth::note_on(n, 90);
        wait_ms(900);
        synth::note_off(n);
        wait_ms(100);
    }
    wait_ms(600);
}

// 3) コード進行 + メロディー（ポリフォニー確認）
void test_progression() {
    ESP_LOGI(TAG, "[seq] chord progression");
    // Fmaj7 - Em7 - Dm7 - Cmaj7 (左手ベース + 右手ボイシング)
    const uint8_t chords[4][4] = {
        {41, 64, 69, 72},  // F2  E4 A4 C5
        {40, 62, 67, 71},  // E2  D4 G4 B4
        {38, 60, 65, 69},  // D2  C4 F4 A4
        {36, 59, 64, 67},  // C2  B3 E4 G4
    };
    // 各コード上のメロディー（8分音符×4、0 は休符）
    const uint8_t melody[4][4] = {
        {76, 0, 77, 79},
        {74, 76, 0, 74},
        {72, 0, 74, 72},
        {71, 0, 0, 0},
    };
    const uint8_t mel_vel[4] = {100, 70, 85, 65};

    for (int rep = 0; rep < 2; ++rep) {
        for (int c = 0; c < 4; ++c) {
            chord_on(chords[c], 4, rep == 0 ? 70 : 95);
            for (int s = 0; s < 4; ++s) {
                const uint8_t m = melody[c][s];
                if (m) synth::note_on(m, mel_vel[s]);
                wait_ms(kBeatMs / 2 - 20);
                if (m) synth::note_off(m);
                wait_ms(20);
            }
            chord_off(chords[c], 4);
        }
    }
    wait_ms(2000);
}

// 4) 9音以上を押さえて voice stealing を発生させる
void test_stealing() {
    ESP_LOGI(TAG, "[seq] voice stealing (12 notes held)");
    const uint8_t notes[] = {48, 52, 55, 59, 60, 64, 67, 71, 72, 76, 79, 83};
    for (uint8_t n : notes) {
        synth::note_on(n, 80);
        wait_ms(120);
    }
    wait_ms(1500);
    synth::all_notes_off();
    wait_ms(2500);
}

void sequencer_task(void *) {
    wait_ms(500);
    for (;;) {
        test_velocity();
        test_range();
        test_progression();
        test_stealing();
    }
}

}  // namespace

// 起動確認用の短い和音
// 各プリセットで全ボイスを同時に鳴らし、最悪ケースの負荷をログで確認する
void stress_test() {
    const int presets[] = {1, 2};  // Clavinet（ティンあり FM）、808 Drums（ハイハット）
    for (int p : presets) {
        synth::set_preset(p);
        ESP_LOGI(TAG, "[stress] %s x %d voices", synth::preset_name(p), synth::kNumVoices);
        for (int r = 0; r < 4; ++r) {
            for (int i = 0; i < synth::kNumVoices; ++i) {
                // ドラムはオープンハイハット（A#）を並べる。FM は 2 オクターブ半に広げる
                const uint8_t note = p == 2 ? static_cast<uint8_t>(34 + 12 * (i % 6) + 0) : static_cast<uint8_t>(48 + i * 2);
                synth::note_on(note, 127);
            }
            wait_ms(1000);
            synth::all_notes_off();
            wait_ms(200);
        }
    }
    synth::set_preset(0);
}

void startup_chime() {
    const uint8_t notes[] = {60, 64, 67, 72};
    for (uint8_t n : notes) {
        synth::note_on(n, 70);
        wait_ms(60);
    }
    wait_ms(400);
    for (uint8_t n : notes) synth::note_off(n);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "epiano Phase 3: USB MIDI + 2-op FM, %d voices", synth::kNumVoices);

    synth::init();
    ESP_ERROR_CHECK(audio::start(synth::render, nullptr));
    startup_chime();
    if (kRunStressTest) stress_test();

    if (kRunTestSequence) {
        // シーケンサーは音声タスクと別コア（Core 0）で動かす
        xTaskCreatePinnedToCore(sequencer_task, "seq", 4096, nullptr, 5, nullptr, 0);
    }
    ESP_ERROR_CHECK(usb_midi::start(on_midi, on_midi_connect));

    // 1秒ごとに統計を表示（音声タスクの外でログを出す）
    constexpr float kBlockUs = 1e6f * audio::kBlockFrames / audio::kSampleRate;
    uint32_t last_underruns = 0;
    for (;;) {
        wait_ms(1000);
        const audio::Stats st = audio::take_stats();
        const float peak = synth::take_peak();
        ESP_LOGI(TAG, "midi=%s pkts=%lu preset=%s voices=%d peak=%.2f (%.1f dBFS) clip=%lu steals=%lu underruns=%lu (+%lu) render avg=%lu us max=%lu us (load avg %.1f%% max %.1f%%)",
                 usb_midi::connected() ? "on" : "off", (unsigned long)usb_midi::packets_received(),
                 synth::preset_name(synth::preset()),
                 synth::active_voices(), peak, peak > 0.0f ? 20.0f * log10f(peak) : -99.0f,
                 (unsigned long)synth::clipped_samples(), (unsigned long)synth::voice_steals(),
                 (unsigned long)st.underruns, (unsigned long)(st.underruns - last_underruns),
                 (unsigned long)st.render_us_avg, (unsigned long)st.render_us_max,
                 100.0f * st.render_us_avg / kBlockUs, 100.0f * st.render_us_max / kBlockUs);
        last_underruns = st.underruns;
    }
}
