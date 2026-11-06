// シンセ本体（16音ポリフォニー、切り替え可能なプリセット）
//
// プリセットの種類:
//   Fm     : FM エレピ / クラビネット
//   Square : 矩形波（PolyBLEP でエイリアシングを低減）
//   Drums  : TR-808 風ドラム（audio/drums.cpp）
//
// FM ボイス:  modulator ──(I(t))──┐
//                                 ├─▶ carrier ──▶ A(t) ──▶ out
//             tine      ──(T(t))──┘
//   out = A(t) * sin(carrier + I(t) * sin(modulator) + T(t) * sin(tine))
//
// tine は第2モジュレータ。打鍵直後だけ倍音を足す（T(t) は短い指数減衰）。
// tine_index_max = 0 のプリセットでは処理をスキップし、2オペレーターと同じ。

#include "audio/synth.h"

#include <atomic>
#include <cmath>
#include <cassert>
#include "sdkconfig.h"

#include "audio/audio_output.h"
#include "audio/drums.h"
#include "dsp/envelope.h"
#include "dsp/rhodes.h"
#include "dsp/limiter.h"
#include "dsp/fm_operator.h"
#include "dsp/oscillator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace synth {
namespace {

constexpr float kFs = static_cast<float>(audio::kSampleRate);

// ---- プリセット ------------------------------------------------------------

enum class Kind : uint8_t { Fm, Square, Drums, Rhodes };
rhodes::Bank<float> s_rhodes_banks[3];
uint32_t s_tremolo_phase = 0;
uint32_t s_tremolo_inc = 0;
dsp::LookaheadLimiter<audio::kBlockFrames> s_limiter;

struct Preset {
    Kind kind = Kind::Fm;
    float volume = 1.0f;  // プリセットごとの音量
    float car_ratio = 0.0f;
    float mod_ratio = 0.0f;
    float mod_detune_hz = 0.0f;  // モジュレータを僅かにずらしてうねりを作る
    dsp::AdsrParams amp = {};
    dsp::AdsrParams index = {};  // 変調指数エンベロープ（1.0 = index_peak）
    float index_min = 0.0f;        // ベロシティ 0 のときの変調指数ピーク [rad]
    float index_max = 0.0f;        // ベロシティ 127 のときの変調指数ピーク [rad]
    float index_vel_curve = 0.0f;  // ベロシティ→変調指数の指数（>1 で強打時に急に明るく）
    float amp_vel_curve = 0.0f;    // ベロシティ→音量の指数
    float amp_vel_min = 0.0f;      // ベロシティ 0 のときの音量
    float key_decay_octave = 0.0f; // 1オクターブ上がるごとにディケイ時間を何倍にするか
    float key_index_octave = 0.0f; // 1オクターブ上がるごとに変調指数を何倍にするか
    float tine_ratio = 0.0f;       // ティン（第2モジュレータ）の周波数比
    float tine_index_max = 0.0f;   // ベロシティ 127 のときのティン変調指数ピーク [rad]（0 で無効）
    float tine_vel_curve = 0.0f;   // ベロシティ→ティンの指数
    uint8_t rhodes_model = 0;
    bool rhodes_tremolo = false;
    float tine_decay_tau = 0.0f;   // ティンの減衰時定数 [s]
};

// 0: Classic — 1:1 で打鍵直後の「バーク」、変調指数を素早く減衰させてベル状の余韻へ
constexpr Preset kClassic = {
    .kind = Kind::Fm,
    .volume = 1.5f,
    .car_ratio = 1.0f,
    .mod_ratio = 1.0f,
    .mod_detune_hz = 0.3f,
    .amp = {.attack_sec = 0.002f, .decay_tau = 1.8f, .sustain = 0.0f, .release_tau = 0.08f},
    .index = {.attack_sec = 0.001f, .decay_tau = 0.18f, .sustain = 0.15f, .release_tau = 0.08f},
    .index_min = 0.4f,
    .index_max = 4.5f,
    .index_vel_curve = 1.6f,
    .amp_vel_curve = 1.3f,
    .amp_vel_min = 0.15f,
    .key_decay_octave = 0.7f,
    .key_index_octave = 0.75f,
    .tine_ratio = 14.0f,
    .tine_index_max = 0.0f,
    .tine_vel_curve = 1.5f,
    .tine_decay_tau = 0.04f,
};

// 1: Clavinet — 1:1 の深い変調で鋭く鼻にかかった音 + 1:3 の倍音。鍵盤を離すとすぐミュート
constexpr Preset kClavinet = {
    .kind = Kind::Fm,
    .car_ratio = 1.0f,
    .mod_ratio = 1.0f,
    .mod_detune_hz = 0.0f,
    .amp = {.attack_sec = 0.001f, .decay_tau = 1.2f, .sustain = 0.25f, .release_tau = 0.03f},
    .index = {.attack_sec = 0.0005f, .decay_tau = 0.45f, .sustain = 0.6f, .release_tau = 0.03f},
    .index_min = 1.8f,
    .index_max = 8.0f,
    .index_vel_curve = 1.2f,
    .amp_vel_curve = 1.0f,
    .amp_vel_min = 0.25f,
    .key_decay_octave = 0.8f,
    .key_index_octave = 0.8f,
    .tine_ratio = 3.0f,
    .tine_index_max = 2.5f,
    .tine_vel_curve = 1.2f,
    .tine_decay_tau = 0.25f,
};

// 2: 808 Drums — 鍵盤ごとに 808 風の打楽器（並びは drums.h）
constexpr Preset kDrums = {.kind = Kind::Drums, .volume = 2.08f};

// 3: Square — 押している間は一定音量（サステイン最大）、離すと即停止（クリック防止に 3ms だけフェード）
constexpr Preset kSquare = {
    .kind = Kind::Square,
    .volume = 0.6f,
    .car_ratio = 1.0f,
    .amp = {.attack_sec = 0.001f, .decay_tau = 1.0f, .sustain = 1.0f, .release_tau = 0.003f},
    .amp_vel_curve = 1.0f,
    .amp_vel_min = 0.5f,
};

constexpr Preset kRhodes = {.kind = Kind::Rhodes};
constexpr Preset kBark = {.kind = Kind::Rhodes, .volume = 1.0f, .rhodes_model = 1};
constexpr Preset kVelvet = {.kind = Kind::Rhodes, .volume = 1.35f, .rhodes_model = 2, .rhodes_tremolo = true};
constexpr const Preset *kPresets[kNumPresets] = {&kRhodes, &kClavinet, &kDrums, &kSquare, &kClassic, &kBark, &kVelvet};
constexpr const char *kPresetNames[kNumPresets] = {"Rhodes", "Clavinet", "808 Drums", "Square", "Classic FM", "Bark", "Velvet Trem"};
constexpr float kTineMaxHz = 18000.0f;  // これを超えるティンはエイリアシングするので鳴らさない
constexpr float kTineOffLevel = 1.0e-3f;
constexpr float kIndexLimit = 12.0f;  // radians_to_phase の有効範囲内に収める
constexpr float kRhodesGain = 2.0f;  // Rhodes-family mixer gain; final peaks are limited
constexpr float kMasterGain = 0.12f;  // 8音程度の強打ならソフトクリップに入りにくい程度
constexpr float kSquareGain = 1.0f;   // 矩形波の基準音量（プリセットの volume と掛ける）
constexpr float kDrumGain = 2.0f;     // ドラムは1ボイスで鳴ることが多いので上げる

// ---- ボイス ----------------------------------------------------------------

struct Voice {
    Kind kind = Kind::Fm;
    drums::DrumVoice drum;
    rhodes::Voice rhodes;
    float rhodes_last = 0.0f;
    float rhodes_tail = 0.0f;
    bool rhodes_tremolo = false;
    dsp::FmOperator car;
    dsp::FmOperator mod;
    dsp::FmOperator tine;
    dsp::Envelope amp_env;
    dsp::Envelope idx_env;
    float amp = 0.0f;         // ベロシティ由来の音量
    float index_peak = 0.0f;  // ベロシティ・キースケール済みの変調指数ピーク
    float tine_level = 0.0f;  // ティン変調指数（指数減衰する、[rad]）
    float tine_coef = 0.0f;   // ティンの1サンプルあたりの減衰係数
    uint32_t started = 0;     // ノートオン順序（古い順に奪う）
    uint8_t note = 0;
    bool gate = false;        // 鍵盤が押されているか
    bool sustained = false;   // 鍵盤は離したがサステインペダルで保持中

    bool idle() const { return kind == Kind::Rhodes ? rhodes.idle() : kind == Kind::Drums ? drum.idle() : amp_env.idle(); }
    float level() const { return kind == Kind::Rhodes ? rhodes.level() : kind == Kind::Drums ? drum.level() : amp_env.level(); }
};

Voice s_voices[kNumVoices];
uint32_t s_note_counter = 0;
float s_note_hz[128];

std::atomic<int> s_active{0};
std::atomic<uint32_t> s_steals{0};
std::atomic<uint32_t> s_clipped{0};       // リミッター前の上限超過サンプル数（累計）
std::atomic<uint32_t> s_peak_milli{0};    // 区間ピーク（クリップ前、1.0 = フルスケール）×1000

// ---- イベントキュー --------------------------------------------------------

enum class EvType : uint8_t { NoteOn, NoteOff, AllOff, Sustain, Preset };
struct Event {
    EvType type;
    uint8_t note;
    uint8_t velocity;
};

QueueHandle_t s_queue = nullptr;
bool s_sustain = false;  // 音声タスク内でのみ触る
int s_preset = kDefaultPreset;        // 音声タスク内でのみ書く（新しいノートから反映）
std::atomic<int> s_preset_shown{kDefaultPreset};

bool post(const Event &ev) {
    return s_queue && xQueueSend(s_queue, &ev, 0) == pdTRUE;
}

// ---- ボイス管理（音声タスク内でのみ呼ぶ） ----------------------------------

Voice *find_voice_for(uint8_t note) {
    // 1) 同じノートが鳴っていれば再利用（再打鍵）
    for (auto &v : s_voices)
        if (!v.idle() && v.note == note &&
            (kPresets[s_preset]->kind != Kind::Rhodes || v.kind == Kind::Rhodes)) return &v;

    // Physical polyphony is separately limited until target profiling passes.
#ifndef CONFIG_EPIANO_RHODES_STRESS
    if (kPresets[s_preset]->kind == Kind::Rhodes) {
        int count = 0;
        Voice *oldest = nullptr;
        for (auto &v : s_voices) {
            if (v.kind != Kind::Rhodes || v.idle()) continue;
            ++count;
            if (!oldest || v.started < oldest->started) oldest = &v;
        }
        if (count >= CONFIG_EPIANO_RHODES_VOICES && oldest) {
            s_steals.fetch_add(1, std::memory_order_relaxed);
            return oldest;
        }
    }
#endif
    // 2) 空きボイス
    for (auto &v : s_voices)
        if (v.idle()) return &v;

    s_steals.fetch_add(1, std::memory_order_relaxed);

    // 3) リリース中で最も小さいボイス
    Voice *best = nullptr;
    for (auto &v : s_voices)
        if (!v.gate && (!best || v.level() < best->level())) best = &v;
    if (best) return best;

    // 4) 最も古いボイス
    best = &s_voices[0];
    for (auto &v : s_voices)
        if (v.started < best->started) best = &v;
    return best;
}

void drum_note_on(Voice *v, uint8_t note, float vel, float volume) {
    const drums::Kind k = drums::kind_for_note(note);
    // クローズドハイハットでオープンハイハットを止める（808 と同じ）
    if (k == drums::Kind::ClosedHat)
        for (auto &o : s_voices)
            if (o.kind == Kind::Drums && !o.idle() && o.drum.kind == drums::Kind::OpenHat) o.drum.choke();
    v->kind = Kind::Drums;
    v->drum.trigger(k, vel, kFs);
    v->drum.gain *= volume;
    v->note = note;
    v->gate = false;  // 1ショット（ノートオフは無視）
    v->sustained = false;
    v->started = ++s_note_counter;
}

void voice_note_on(uint8_t note, uint8_t velocity) {
    const Preset &p = *kPresets[s_preset];
    Voice *v = find_voice_for(note);
    const float vel = velocity / 127.0f;

    if (p.kind == Kind::Drums) {
        drum_note_on(v, note, vel, p.volume);
        return;
    }
    if (p.kind == Kind::Rhodes) {
        const float previous = v->kind == Kind::Rhodes && !v->idle() ? v->rhodes_last * v->amp : 0.0f;
        v->amp = kRhodesGain * p.volume;
        v->rhodes_tail = previous / v->amp;
        v->rhodes_tremolo = p.rhodes_tremolo;
        v->kind = p.kind;
        v->rhodes.note_on(s_rhodes_banks[p.rhodes_model], note, velocity);
        v->note = note;
        v->gate = true;
        v->sustained = false;
        v->started = ++s_note_counter;
        return;
    }
    if (v->kind != p.kind) {
        // 別の種類のボイスを奪った場合は状態を初期化する
        v->amp_env.reset();
        v->idx_env.reset();
        v->kind = p.kind;
    }
    const bool was_idle = v->idle();

    const float octave = (note - 60) / 12.0f;  // C4 からのオクターブ差
    const float hz = s_note_hz[note];

    v->car.inc = dsp::hz_to_inc(hz * p.car_ratio, kFs);
    v->mod.inc = dsp::hz_to_inc(hz * p.mod_ratio + p.mod_detune_hz, kFs);
    if (was_idle) {
        // 空きボイスは位相を揃えて毎回同じアタックにする
        // （奪ったボイスは位相を連続させてクリックを避ける）
        v->car.phase = 0;
        v->mod.phase = 0;
        v->tine.phase = 0;
    }

    v->amp = (p.amp_vel_min + (1.0f - p.amp_vel_min) * std::pow(vel, p.amp_vel_curve)) * p.volume;

    if (p.kind == Kind::Square) {
        v->amp_env.configure(p.amp, kFs);
        v->note = note;
        v->gate = true;
        v->sustained = false;
        v->started = ++s_note_counter;
        v->amp_env.gate_on();
        return;
    }

    dsp::AdsrParams amp = p.amp;
    amp.decay_tau *= std::pow(p.key_decay_octave, octave);
    v->amp_env.configure(amp, kFs);
    v->idx_env.configure(p.index, kFs);

    float idx = p.index_min + (p.index_max - p.index_min) * std::pow(vel, p.index_vel_curve);
    idx *= std::pow(p.key_index_octave, octave);
    float tine = 0.0f;
    const float tine_hz = hz * p.tine_ratio;
    if (p.tine_index_max > 0.0f && tine_hz < kTineMaxHz) {
        tine = p.tine_index_max * std::pow(vel, p.tine_vel_curve) * std::pow(p.key_index_octave, octave);
        v->tine.inc = dsp::hz_to_inc(tine_hz, kFs);
        v->tine_coef = std::exp(-1.0f / (p.tine_decay_tau * kFs));
    }
    // 変調の合計が radians_to_phase の有効範囲に収まるように制限する
    if (idx > kIndexLimit) idx = kIndexLimit;
    if (idx + tine > kIndexLimit) tine = kIndexLimit - idx;
    v->index_peak = idx;
    v->tine_level = tine;

    v->note = note;
    v->gate = true;
    v->sustained = false;
    v->started = ++s_note_counter;
    v->amp_env.gate_on();
    v->idx_env.gate_on();
}

void voice_release(Voice &v) {
    v.gate = false;
    v.sustained = false;
    if (v.kind == Kind::Rhodes) v.rhodes.note_off();
    v.amp_env.gate_off();
    v.idx_env.gate_off();
}

void voice_note_off(uint8_t note) {
    for (auto &v : s_voices) {
        if (v.gate && v.note == note) {
            if (s_sustain) {
                v.gate = false;
                v.sustained = true;  // ペダルを離すまで鳴らし続ける
            } else {
                voice_release(v);
            }
        }
    }
}

void set_sustain(bool on) {
    s_sustain = on;
    if (on) return;
    for (auto &v : s_voices)
        if (v.sustained) voice_release(v);
}

void process_events() {
    Event ev;
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        switch (ev.type) {
        case EvType::NoteOn:
            if (ev.velocity == 0) voice_note_off(ev.note);  // MIDI: vel 0 の Note On は Note Off
            else voice_note_on(ev.note, ev.velocity);
            break;
        case EvType::NoteOff:
            voice_note_off(ev.note);
            break;
        case EvType::AllOff:
            s_sustain = false;
            for (auto &v : s_voices)
                if (!v.idle()) voice_release(v);
            break;
        case EvType::Sustain:
            set_sustain(ev.velocity != 0);
            break;
        case EvType::Preset:
            s_preset = ev.note % kNumPresets;
            s_preset_shown.store(s_preset, std::memory_order_relaxed);
            break;
        }
    }
}

// リミッター適用前の入力上限超過を数える。出力をクリップする閾値ではない。
constexpr float kClipThreshold = dsp::LookaheadLimiter<audio::kBlockFrames>::kCeiling;

// PolyBLEP: 矩形波の不連続点を滑らかにしてエイリアシングを減らす
inline float poly_blep(float t, float dt, float inv_dt) {
    if (t < dt) {
        const float x = t * inv_dt;
        return x + x - x * x - 1.0f;
    }
    if (t > 1.0f - dt) {
        const float x = (t - 1.0f) * inv_dt;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

void render_fm(Voice &v, float *mix, size_t frames) {
    if (v.tine_level > kTineOffLevel) {
        for (size_t i = 0; i < frames; ++i) {
            const float m = v.mod.process(0) * v.index_peak * v.idx_env.process() +
                            v.tine.process(0) * v.tine_level;
            v.tine_level *= v.tine_coef;
            const float c = v.car.process(dsp::radians_to_phase(m));
            mix[i] += c * v.amp_env.process() * v.amp;
        }
    } else {
        for (size_t i = 0; i < frames; ++i) {
            const float m = v.mod.process(0) * v.index_peak * v.idx_env.process();
            const float c = v.car.process(dsp::radians_to_phase(m));
            mix[i] += c * v.amp_env.process() * v.amp;
        }
    }
}

void render_square(Voice &v, float *mix, size_t frames) {
    constexpr float kPhaseScale = 1.0f / 4294967296.0f;
    if (v.car.inc == 0) return;  // 周波数 0 は無音（PolyBLEP の 0 除算も避ける）
    const float dt = v.car.inc * kPhaseScale;
    const float inv_dt = 1.0f / dt;
    const float gain = v.amp * kSquareGain;
    for (size_t i = 0; i < frames; ++i) {
        const float t = v.car.phase * kPhaseScale;
        float t2 = t + 0.5f;
        if (t2 >= 1.0f) t2 -= 1.0f;
        float y = t < 0.5f ? 1.0f : -1.0f;
        y += poly_blep(t, dt, inv_dt) - poly_blep(t2, dt, inv_dt);
        mix[i] += y * v.amp_env.process() * gain;
        v.car.phase += v.car.inc;
    }
}

}  // namespace

void init() {
    dsp::init_sine_table();
    s_limiter.init(kFs);
    // 2x for 2–5 modes, 4x for the highest one-mode notes.
    s_rhodes_banks[0].init(kFs, 4, true);
    s_rhodes_banks[1].init(kFs, 4, true, rhodes::kBark);
    s_rhodes_banks[2].init(kFs, 4, true, rhodes::kVelvet);
    s_tremolo_phase = 0;
    s_tremolo_inc = dsp::hz_to_inc(4.2f, kFs);
    for (int n = 0; n < 128; ++n) s_note_hz[n] = 440.0f * std::pow(2.0f, (n - 69) / 12.0f);
    s_queue = xQueueCreate(64, sizeof(Event));
}

bool note_on(uint8_t note, uint8_t velocity) {
    return post({EvType::NoteOn, static_cast<uint8_t>(note & 0x7f), static_cast<uint8_t>(velocity & 0x7f)});
}
bool note_off(uint8_t note) { return post({EvType::NoteOff, static_cast<uint8_t>(note & 0x7f), 0}); }
bool all_notes_off() { return post({EvType::AllOff, 0, 0}); }
bool set_preset(int index) { return post({EvType::Preset, static_cast<uint8_t>(index % kNumPresets), 0}); }
int preset() { return s_preset_shown.load(std::memory_order_relaxed); }
const char *preset_name(int index) { return kPresetNames[index % kNumPresets]; }
bool sustain(bool on) { return post({EvType::Sustain, 0, static_cast<uint8_t>(on ? 1 : 0)}); }

void render(int16_t *out, size_t frames, void *) {
    assert(frames == audio::kBlockFrames);
    process_events();

    float mix[audio::kBlockFrames];
    float drum_mix[audio::kBlockFrames];
    float trem_mix[audio::kBlockFrames];
    bool has_tremolo = false;
    for (size_t i = 0; i < frames; ++i) mix[i] = drum_mix[i] = trem_mix[i] = 0.0f;

    int active = 0;
    for (auto &v : s_voices) {
        if (v.idle()) continue;
        ++active;
        switch (v.kind) {
        case Kind::Rhodes:
            v.rhodes.render_mixed(v.rhodes_tremolo ? trem_mix : mix, frames, v.amp, v.rhodes_tail, v.rhodes_last);
            has_tremolo |= v.rhodes_tremolo;
            break;
        case Kind::Fm:
            render_fm(v, mix, frames);
            break;
        case Kind::Square:
            render_square(v, mix, frames);
            break;
        case Kind::Drums:
            v.drum.render(drum_mix, frames);
            break;
        }
    }
    s_active.store(active, std::memory_order_relaxed);
    const uint32_t next_phase = s_tremolo_phase + s_tremolo_inc * static_cast<uint32_t>(frames);
    if (has_tremolo && frames) {
        // Shared, free-running LFO; interpolate two sine-table reads per block.
        float gain = .7f + .3f * dsp::sine(s_tremolo_phase);
        const float step = (.7f + .3f * dsp::sine(next_phase) - gain) / frames;
        for (size_t i = 0; i < frames; ++i) {
            mix[i] += trem_mix[i] * gain;
            gain += step;
        }
    }
    s_tremolo_phase = next_phase;

    float peak = 0.0f;
    uint32_t clipped = 0;
    for (size_t i = 0; i < frames; ++i) {
        const float x = (mix[i] + drum_mix[i] * kDrumGain) * kMasterGain;
        const float ax = x < 0.0f ? -x : x;
        if (ax > peak) peak = ax;
        if (ax > kClipThreshold) ++clipped;
        mix[i] = x;
    }
    s_limiter.process(mix, peak);
    for (size_t i=0; i<frames; ++i) out[i] = static_cast<int16_t>(mix[i]*32767.0f);
    if (clipped) s_clipped.fetch_add(clipped, std::memory_order_relaxed);
    const uint32_t pm = static_cast<uint32_t>(peak * 1000.0f);
    if (pm > s_peak_milli.load(std::memory_order_relaxed)) s_peak_milli.store(pm, std::memory_order_relaxed);
}

int active_voices() { return s_active.load(std::memory_order_relaxed); }
uint32_t voice_steals() { return s_steals.load(std::memory_order_relaxed); }
uint32_t clipped_samples() { return s_clipped.load(std::memory_order_relaxed); }
float take_peak() { return s_peak_milli.exchange(0, std::memory_order_relaxed) / 1000.0f; }

}  // namespace synth
