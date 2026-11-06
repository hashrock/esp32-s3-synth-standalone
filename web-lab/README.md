# Bark Tone Lab

物理モデリング・エレピの音色調整用Web版。Barkが初期値、8音ポリ、64サンプル先読みリミッター。Bark / Rhodes / Velvet Trem、画面鍵盤、PC鍵盤、Web MIDI、A/B比較、ベロシティ試奏、JSON保存・読込、C++設定出力を備えます。

## ローカル起動

```sh
cd web-lab
npm run preview
```

http://127.0.0.1:4173 で「音を有効にする」を押します。依存パッケージは不要。AudioWorkletにはHTTPSまたはlocalhostが必要です。MIDIは対応ブラウザで接続ボタンを押してください。

音色変更は次の打鍵から適用。モニター音量は試聴専用で書き出されません。設定はlocalStorageに保存します。JSONでバックアップできます。

## 実機への反映

C++出力の `tone_lab::kTone` は `rhodes::Bank::init` のSettings引数に対応します。`kVolume` はプリセット音量、`kTremolo` は4.2 Hzトレモロの有効状態です。自動書き込みは行いません。

## 検証

`npm test` で設定、8音制限、サステイン、リミッターを検証。`tests/reference.cpp` は親リポジトリの `main/dsp/rhodes.h` を参照する比較用プログラムです。Bark C4 velocity 78の4096サンプルをdouble実装と比較し最大誤差6.72e-15未満を確認しました。実機float・高速平方根・DACとの差は残ります。トレモロはブラウザ側でMath.sinを使用します。

ブラウザ操作用ホストが利用できず、実ブラウザでの操作・試聴は未確認です。HTTP応答、JavaScript構文、音源単体テストは確認済みです。
