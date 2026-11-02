// USB MIDI ホスト（USB Audio Class 1.0 / MIDI Streaming の Bulk IN を受信する）
//
// - 1台の MIDI デバイスのみ扱う
// - 受信のみ（Bulk OUT は使わない）
// - 切断 → 再接続に対応

#include "midi/usb_midi.h"

#include <atomic>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

namespace usb_midi {
namespace {

constexpr const char *TAG = "usb_midi";

constexpr uint8_t kClassAudio = 0x01;
constexpr uint8_t kSubclassMidiStreaming = 0x03;

constexpr UBaseType_t kHostTaskPriority = 10;
constexpr UBaseType_t kClientTaskPriority = 9;
constexpr BaseType_t kUsbCore = 0;  // 音声タスク（Core 1）と分ける

// MIDI デバイスが無い状態がこの時間続いたら、ルートポートを入れ直して列挙をやり直す。
// 電源投入直後に鍵盤の応答が間に合わず列挙に失敗すると、抜き挿しするまで認識されないため。
// （ESP32-S3 のルートポート電源は USB コントローラ内部の状態で、VBUS の 5V は切れない）
constexpr TickType_t kRetryInterval = pdMS_TO_TICKS(3000);
constexpr TickType_t kPortOffTime = pdMS_TO_TICKS(100);

MessageFn s_on_message = nullptr;
ConnectFn s_on_connect = nullptr;

usb_host_client_handle_t s_client = nullptr;

// 接続中デバイスの状態（クライアントタスク内でのみ触る）
struct Device {
    uint8_t pending_addr = 0;  // NEW_DEV で受け取ったアドレス（0 = なし）
    bool gone = false;         // DEV_GONE を受け取った
    usb_device_handle_t handle = nullptr;
    int interface_num = -1;
    uint8_t ep_in = 0;
    uint16_t mps = 0;
    usb_transfer_t *xfer = nullptr;
    bool xfer_in_flight = false;
};
Device s_dev;

std::atomic<bool> s_connected{false};
std::atomic<uint32_t> s_packets{0};

// ---- USB-MIDI イベントパケット（4 バイト）の解析 ---------------------------
//   byte0: [Cable Number:4][Code Index Number:4]
//   byte1-3: MIDI メッセージ
void parse_packets(const uint8_t *buf, int len) {
    for (int i = 0; i + 4 <= len; i += 4) {
        const uint8_t cin = buf[i] & 0x0f;
        const uint8_t status = buf[i + 1];
        if (buf[i] == 0 && status == 0) continue;  // パディング
        s_packets.fetch_add(1, std::memory_order_relaxed);
        switch (cin) {
        case 0x8:  // Note Off
        case 0x9:  // Note On
        case 0xA:  // Poly Key Pressure
        case 0xB:  // Control Change
        case 0xC:  // Program Change
        case 0xD:  // Channel Pressure
        case 0xE:  // Pitch Bend
            if (s_on_message && (status & 0x80)) s_on_message(status, buf[i + 2] & 0x7f, buf[i + 3] & 0x7f);
            break;
        default:  // SysEx・システムメッセージ等は無視
            break;
        }
    }
}

void submit_in() {
    s_dev.xfer->num_bytes = s_dev.mps;  // Bulk IN は MPS の倍数で要求する
    if (usb_host_transfer_submit(s_dev.xfer) == ESP_OK) {
        s_dev.xfer_in_flight = true;
    } else {
        s_dev.xfer_in_flight = false;
        ESP_LOGW(TAG, "transfer submit failed");
    }
}

// usb_host_client_handle_events() の中（クライアントタスク）から呼ばれる
void in_transfer_cb(usb_transfer_t *xfer) {
    s_dev.xfer_in_flight = false;
    switch (xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        parse_packets(xfer->data_buffer, xfer->actual_num_bytes);
        if (!s_dev.gone) submit_in();
        break;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        break;  // 切断処理側で後始末する
    default:
        ESP_LOGW(TAG, "IN transfer status %d", xfer->status);
        if (!s_dev.gone) submit_in();
        break;
    }
}

// ---- デバイスのオープン / クローズ -----------------------------------------

// 構成ディスクリプタから MIDI Streaming インターフェースと Bulk IN エンドポイントを探す
bool find_midi_endpoint(const usb_config_desc_t *cfg, int *intf_num, int *alt, uint8_t *ep_addr, uint16_t *mps) {
    int offset = 0;
    const usb_standard_desc_t *d = reinterpret_cast<const usb_standard_desc_t *>(cfg);
    const usb_intf_desc_t *cur_intf = nullptr;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != nullptr) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(d);
            const bool is_midi = intf->bInterfaceClass == kClassAudio &&
                                 intf->bInterfaceSubClass == kSubclassMidiStreaming;
            cur_intf = is_midi ? intf : nullptr;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && cur_intf) {
            const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(d);
            const bool is_in = ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK;
            const bool is_bulk = USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_BULK;
            if (is_in && is_bulk) {
                *intf_num = cur_intf->bInterfaceNumber;
                *alt = cur_intf->bAlternateSetting;
                *ep_addr = ep->bEndpointAddress;
                *mps = USB_EP_DESC_GET_MPS(ep);
                return true;
            }
        }
    }
    return false;
}

void close_device() {
    if (!s_dev.handle) return;
    if (s_dev.xfer_in_flight && !s_dev.gone) {
        // 接続中にクローズする場合はエンドポイントを止めて転送を回収する
        usb_host_endpoint_halt(s_dev.handle, s_dev.ep_in);
        usb_host_endpoint_flush(s_dev.handle, s_dev.ep_in);
    }
    // 転送のコールバックが返るまで待つ（切断時は NO_DEVICE で返ってくる）
    for (int i = 0; i < 100 && s_dev.xfer_in_flight; ++i) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));

    if (s_dev.interface_num >= 0) usb_host_interface_release(s_client, s_dev.handle, s_dev.interface_num);
    if (s_dev.xfer && !s_dev.xfer_in_flight) usb_host_transfer_free(s_dev.xfer);
    usb_host_device_close(s_client, s_dev.handle);
    s_dev = Device{};

    s_connected.store(false);
    if (s_on_connect) s_on_connect(false);
    ESP_LOGI(TAG, "device closed");
}

void open_device(uint8_t addr) {
    if (s_dev.handle) {
        ESP_LOGW(TAG, "ignoring additional device at address %u (only one MIDI device supported)", addr);
        return;
    }
    usb_device_handle_t h = nullptr;
    if (usb_host_device_open(s_client, addr, &h) != ESP_OK) {
        ESP_LOGE(TAG, "device open failed (addr %u)", addr);
        return;
    }
    s_dev.handle = h;

    const usb_device_desc_t *dd = nullptr;
    usb_host_get_device_descriptor(h, &dd);
    ESP_LOGI(TAG, "device connected: VID=%04x PID=%04x", dd->idVendor, dd->idProduct);

    const usb_config_desc_t *cfg = nullptr;
    int intf = -1, alt = 0;
    uint8_t ep = 0;
    uint16_t mps = 0;
    if (usb_host_get_active_config_descriptor(h, &cfg) != ESP_OK ||
        !find_midi_endpoint(cfg, &intf, &alt, &ep, &mps)) {
        ESP_LOGW(TAG, "not a USB MIDI device");
        usb_host_device_close(s_client, h);
        s_dev = Device{};
        return;
    }
    ESP_LOGI(TAG, "MIDI streaming: interface %d alt %d, bulk IN ep 0x%02x, MPS %u", intf, alt, ep, mps);

    if (usb_host_interface_claim(s_client, h, intf, alt) != ESP_OK) {
        ESP_LOGE(TAG, "interface claim failed");
        usb_host_device_close(s_client, h);
        s_dev = Device{};
        return;
    }
    s_dev.interface_num = intf;
    s_dev.ep_in = ep;
    s_dev.mps = mps;

    // 転送バッファの確保は接続時のみ（音声処理とは無関係のタスク）
    if (usb_host_transfer_alloc(mps, 0, &s_dev.xfer) != ESP_OK) {
        ESP_LOGE(TAG, "transfer alloc failed");
        close_device();
        return;
    }
    s_dev.xfer->device_handle = h;
    s_dev.xfer->bEndpointAddress = ep;
    s_dev.xfer->callback = in_transfer_cb;
    s_dev.xfer->context = nullptr;

    s_connected.store(true);
    if (s_on_connect) s_on_connect(true);
    submit_in();
}

// ---- タスク ----------------------------------------------------------------

void client_event_cb(const usb_host_client_event_msg_t *msg, void *) {
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        s_dev.pending_addr = msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (msg->dev_gone.dev_hdl == s_dev.handle) s_dev.gone = true;
        break;
    default:
        break;
    }
}

void client_task(void *) {
    usb_host_client_config_t cfg = {};
    cfg.is_synchronous = false;
    cfg.max_num_event_msg = 5;
    cfg.async.client_event_callback = client_event_cb;
    cfg.async.callback_arg = nullptr;
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &s_client));

    TickType_t no_device_since = xTaskGetTickCount();
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(500));
        if (s_dev.gone) {
            ESP_LOGI(TAG, "device disconnected");
            close_device();
        }
        if (s_dev.pending_addr) {
            const uint8_t addr = s_dev.pending_addr;
            s_dev.pending_addr = 0;
            open_device(addr);
        }

        // 列挙失敗からの復帰
        usb_host_lib_info_t info = {};
        const bool any_device = usb_host_lib_info(&info) == ESP_OK && info.num_devices > 0;
        if (s_dev.handle || any_device) {
            no_device_since = xTaskGetTickCount();
        } else if (xTaskGetTickCount() - no_device_since >= kRetryInterval) {
            ESP_LOGI(TAG, "no MIDI device, restarting root port");
            usb_host_lib_set_root_port_power(false);
            vTaskDelay(kPortOffTime);
            usb_host_lib_set_root_port_power(true);
            no_device_since = xTaskGetTickCount();
        }
    }
}

void host_task(void *arg) {
    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.intr_flags = ESP_INTR_FLAG_LOWMED;
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskNotifyGive(static_cast<TaskHandle_t>(arg));

    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

}  // namespace

esp_err_t start(MessageFn on_message, ConnectFn on_connect) {
    s_on_message = on_message;
    s_on_connect = on_connect;

    if (xTaskCreatePinnedToCore(host_task, "usb_host", 4096, xTaskGetCurrentTaskHandle(), kHostTaskPriority,
                                nullptr, kUsbCore) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) return ESP_ERR_TIMEOUT;

    if (xTaskCreatePinnedToCore(client_task, "usb_midi", 4096, nullptr, kClientTaskPriority, nullptr, kUsbCore) !=
        pdPASS)
        return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "USB host started, waiting for MIDI device");
    return ESP_OK;
}

bool connected() { return s_connected.load(); }
uint32_t packets_received() { return s_packets.load(std::memory_order_relaxed); }

}  // namespace usb_midi
