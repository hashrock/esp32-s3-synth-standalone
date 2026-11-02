# ESP32-S3 FM Electric Piano

Freenove ESP32-S3（16MB Flash）+ PCM5102A によるスタンドアロンFMエレピ音源。

**現在の状態：Phase 4（完成度向上）** — KORG microKEY Air 37 を USB-OTG 端子につなぎ、16音ポリフォニーで演奏できる。モジュレーションホイールで音色を切り替える。

| ホイール位置 | 音色 | 内容 |
|---|---|---|
| 0〜31 | Classic | FM エレピ（1:1、変調指数が素早く減衰） |
| 32〜63 | Clavinet | 深い FM + 1:3 の倍音。離すとすぐミュート |
| 64〜95 | 808 Drums | TR-808 風ドラム（下表） |
| 96〜127 | Square | 矩形波（PolyBLEP）。サステイン最大・リリースなし |

808 Drums の鍵盤割り当て（どのオクターブでも同じ）：

| C | C# | D | D# | E | F | F# | G | G# | A | A# | B |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Kick | Rim | Snare | Clap | Snare2 | Low Tom | Closed Hat | Mid Tom | Closed Hat | High Tom | Open Hat | Cowbell |

クローズドハットでオープンハットが止まる（チョーク）。音量は各プリセットの `.volume` で調整する。

Phase 1〜3 は実機で動作確認済み（2026-11-03）。

## 接続

- **UART 側 USB-C**：PC（書き込み・ログ）またはモバイルバッテリー（給電）
- **USB-OTG 側 USB-C**：microKEY Air 37（VID 0944 / PID 0136）。UART 側から給電した状態で、OTG 側から microKEY へ給電されることを実機で確認済み
- MIDI は全チャンネル受信。Note On/Off、CC1（音色切り替え）、CC64（サステイン）、CC120/123（全音停止）、Program Change 0〜3（音色切り替え）に対応
- 抜き挿し（切断・再接続）に対応。切断時は鳴っている音を止める

## 開発環境

- ESP-IDF v6.1-dev（`~/esp/esp-idf`、新I2Sドライバ `driver/i2s_std.h` / コンポーネント `esp_driver_i2s` を使用）
- ターゲット：`esp32s3`

ESP-IDF v5.x 系でも同じAPIのためビルドできる見込み（未確認）。

## 配線（暫定）

| PCM5102A | ESP32-S3 | 備考 |
|---|---|---|
| BCK | GPIO16 | |
| LCK (LRCK/WS) | GPIO17 | |
| DIN | GPIO18 | |
| SCK | GND | 内部PLLを使用。多くのモジュールは裏面のはんだジャンパでGNDに落とす |
| GND | GND | |
| VIN | モジュール仕様に従う | 3.3V/5V どちらか、要確認 |

**PCM5102A 設定端子（モジュールにより要確認）**

- `XSMT`：High でミュート解除（Lowのままだと無音）
- `FMT`：Low で I2S フォーマット
- `FLT`：Low で通常レイテンシフィルタ
- `DEMP`：Low でディエンファシス OFF

**実機で判明したこと（2026-11-03）**

- **SCK は必ず GND に落とす。** 浮いていると音程が低く・音痴・ノイズ混じりになった
- **XSMT 未接続では無音。** High にして初めて鳴った
- **手元のモジュールは裏面ジャンパ（H1L〜H4L）の H/L シルクが実際と逆だった。** はんだ付け後はテスターで各端子の電圧を確認すること（XSMT=3.3V、FLT/DEMP/FMT=0V）
- ジャンパを L 側にしたまま XSMT を 3V3 にワイヤ接続すると 3V3–GND 短絡になる（実際に USB 給電が落ちた）。ジャンパとワイヤを併用しない

I2S設定：Philips標準、16bit スロット、ステレオ → BCK = 32fs = 1.536MHz。MCLKは出力しない。

## ビルド・書き込み・モニター

```sh
# ESP-IDF 環境を読み込む（ターミナルごとに1回）
. ~/esp/esp-idf/export.sh

cd ~/code/hardware/epiano

# 初回のみ：ターゲット設定（sdkconfig.defaults も読み込まれる）
idf.py set-target esp32s3

# ビルド
idf.py build

# 書き込み + シリアルモニター（ポートは環境に合わせる）
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

- ポートは `ls /dev/cu.*` で確認（Freenove の USB-UART 側。CH343 なら `/dev/cu.wchusbserial*` 等）
- モニター終了は `Ctrl+]`
- 書き込みに失敗する場合は BOOT ボタンを押しながら RST

## シリアルログ

1秒ごとに次のような統計を出力する。

```
I (1234) main: blocks=750 underruns=0 (+0) render avg=12 us max=20 us (budget 1333 us, load 0.9%)
```

| 項目 | 意味 |
|---|---|
| `blocks` | 出力した 64 フレームブロック数（累計、毎秒 750 増えれば正常） |
| `underruns` | DMA 送信キューが空になった回数（累計）。`(+N)` は直近1秒の増分 |
| `render avg/max` | 1ブロックの生成時間 |
| `budget` | 1ブロックの時間（64 / 48000 = 1333us） |
| `load` | 平均生成時間 / budget |

## Phase 1 完了条件の確認手順

1. 書き込み後、ログに `I2S started` が出ることを確認
2. `blocks` が毎秒約 750 ずつ増え、`underruns` の増分が 0 であること
3. PCM5102A のライン出力をアンプ（またはヘッドホン/ライン入力機器）に接続し、440Hz の連続音が途切れず聞こえること
4. オシロスコープがあれば BCK=1.536MHz、LCK=48kHz を確認

音が出ない場合：XSMT が High か、SCK が GND に落ちているか、GPIO 番号、VIN 電源を確認。

## 構成

```text
main/
├── main.cpp                 # MIDI → シンセの接続、テストシーケンス（kRunTestSequence）、負荷テスト（kRunStressTest）、統計ログ
├── audio/
│   ├── audio_output.{h,cpp} # I2S 初期化、オーディオタスク（Core 1）、アンダーラン計測
│   ├── synth.{h,cpp}        # ボイス管理・voice stealing・プリセット（FM/矩形波/ドラム）、イベントキュー
│   └── drums.{h,cpp}        # TR-808 風ドラム（キック・スネア・クラップ・タム・ハット・カウベル・リム）
├── midi/
│   └── usb_midi.{h,cpp}     # USB Host、MIDI Streaming の Bulk IN 受信、抜き挿し対応
└── dsp/
    ├── oscillator.{h,cpp}   # サインテーブル（2048点・線形補間）
    ├── fm_operator.h        # 位相変調オペレーター
    ├── envelope.{h,cpp}     # ADSR（線形アタック・指数ディケイ/リリース）
    └── svf.h                # ステートバリアブルフィルタ、ノイズ
```

- 音声：`audio_output` が 64 フレームごとに `synth::render` を呼ぶ → モノラル → L/R 複製 → `i2s_channel_write`
- イベント：`synth::note_on/note_off/all_notes_off` は FreeRTOS キューに積むだけ。どのタスクからでも呼べ、render の先頭で処理される（Phase 3 の USB MIDI も同じ API を使う）
- FM：`out = A(t) * sin(carrier + I(t) * sin(modulator) + T(t) * sin(tine))`。変調指数 I(t) は専用エンベロープで打鍵直後に大きく、素早く減衰。tine は第2モジュレータ（Clavinet で使用）
- Voice stealing：同じノート → 空き → リリース中で最も小さい → 最も古い、の順。奪ったボイスは位相・レベルを連続させてクリックを防ぐ
- 音色パラメータは `synth.cpp` の `kClassic` / `kClavinet` / `kDrums` / `kSquare`、ドラムの各音は `drums.cpp` の `trigger()`
- サチュレーションは試したが不自然だったため不採用

### テストシーケンス（ループ）

1. ベロシティスイープ（C4、vel 20〜127）
2. 音域（C2〜C6）
3. コード進行 Fmaj7–Em7–Dm7–Cmaj7 + メロディー
4. 12音を押さえて voice stealing

### 負荷

`kRunStressTest = true` で起動時に Clavinet と 808 Drums を 16 音同時に鳴らす。実測（2026-11-03）：最大 65.7%、アンダーラン 0。
16 音すべてを最大ベロシティで鳴らすとピーク 1.28 になりソフトクリップがかかる。

### ログ

```
I (14306) main: midi=on pkts=242 preset=Classic voices=8 peak=0.41 (-7.7 dBFS) clip=0 steals=4 underruns=0 (+0) render avg=190 us max=315 us (load avg 14.2% max 23.6%)
```

`voices` は発音中ボイス数、`peak` は出力ピーク、`clip` はソフトクリップ域に入ったサンプル数（累計）、`steals` は奪取回数（累計）、`load` は 1 ブロック（1333us）に対する生成時間の割合。

## 未確認事項

- GPIO16/17/18 が Freenove ESP32-S3 で他機能（PSRAM・カメラ等）と競合しないか（公式ピン配置で要確認）
- PCM5102A モジュールの VIN 電圧・設定端子のデフォルト状態
