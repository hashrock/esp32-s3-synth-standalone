#pragma once

#include <cstdint>

#include "esp_err.h"

namespace usb_midi {

// 受信した MIDI チャンネルメッセージ（status は 0x80〜0xEF）
// USB Host クライアントタスク上で呼ばれる。ブロックしないこと。
using MessageFn = void (*)(uint8_t status, uint8_t data1, uint8_t data2);

// デバイスの接続・切断通知（切断時は鳴っている音を止めるのに使う）
using ConnectFn = void (*)(bool connected);

// USB Host を起動し、USB MIDI デバイス（Audio class / MIDI Streaming）を待ち受ける
esp_err_t start(MessageFn on_message, ConnectFn on_connect);

bool connected();
uint32_t packets_received();

}  // namespace usb_midi
