/*
 * c5_bridge.cc - S3 侧 C5 WiFi 网桥驱动实现
 */
#include "c5_bridge.h"

#include <cstring>

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/task.h>

#define TAG "C5Bridge"

#define EV_C5_READY        BIT0
#define EV_WIFI_CONNECTED  BIT1
#define EV_OTA_URL         BIT2
#define EV_PONG            BIT3
#define EV_WIFI_RESET_DONE BIT4
#define EV_SCAN_RESULT     BIT5
#define EV_CONFIG_RESULT   BIT6
#define EV_NEED_PROVISION  BIT7

/* 查表法 CRC16-CCITT, 复用 bridge_protocol.h 中的实现 (与 C5 端一致) */
static inline uint16_t crc16_ccitt(const uint8_t* data, size_t len, uint16_t crc)
{
    return bridge_crc16_update(crc, data, len);
}

C5Bridge::C5Bridge(gpio_num_t tx_pin, gpio_num_t rx_pin, size_t rx_buffer_size)
    : tx_pin_(tx_pin), rx_pin_(rx_pin), rx_buffer_size_(rx_buffer_size), uart_port_(UART_NUM_1) {
    tx_lock_ = xSemaphoreCreateMutex();
    events_ = xEventGroupCreate();
}

C5Bridge::~C5Bridge() {
    if (rx_task_) vTaskDelete(rx_task_);
    if (tx_lock_) vSemaphoreDelete(tx_lock_);
    if (events_) vEventGroupDelete(events_);
    uart_driver_delete(uart_port_);
}

void C5Bridge::Start(int baud_rate) {
    bool flow_ctrl = (rts_pin_ != GPIO_NUM_NC && cts_pin_ != GPIO_NUM_NC);

    uart_config_t cfg = {};
    cfg.baud_rate = baud_rate;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = flow_ctrl ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE;
    cfg.rx_flow_ctrl_thresh = 122;   // 剩余空间阈值, 触发 RTS 反压
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(uart_port_, rx_buffer_size_, rx_buffer_size_, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_port_, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_port_, tx_pin_, rx_pin_,
                                 flow_ctrl ? rts_pin_ : UART_PIN_NO_CHANGE,
                                 flow_ctrl ? cts_pin_ : UART_PIN_NO_CHANGE));

    xTaskCreate(RxTaskEntry, "c5_rx", 4096, this, 12, &rx_task_);
    ESP_LOGI(TAG, "C5Bridge started: tx=%d rx=%d baud=%d flow_ctrl=%d",
             tx_pin_, rx_pin_, baud_rate, flow_ctrl ? 1 : 0);
}

bool C5Bridge::SendFrame(uint8_t type, uint8_t link_id, const uint8_t* payload, uint16_t len) {
    if (len > BRIDGE_MAX_PAYLOAD) return false;

    xSemaphoreTake(tx_lock_, portMAX_DELAY);

    // 一次组装整帧再单次写出 (原来 3 次 uart_write_bytes -> 1 次)
    uint8_t* f = tx_frame_;
    f[0] = BRIDGE_MAGIC0;
    f[1] = BRIDGE_MAGIC1;
    f[2] = type;
    f[3] = link_id;
    f[4] = (uint8_t)(len & 0xFF);
    f[5] = (uint8_t)(len >> 8);
    if (payload && len) memcpy(&f[BRIDGE_HEADER_LEN], payload, len);

    uint16_t crc = crc16_ccitt(&f[2], BRIDGE_HEADER_LEN - 2, 0xFFFF);
    if (payload && len) crc = crc16_ccitt(&f[BRIDGE_HEADER_LEN], len, crc);
    f[BRIDGE_HEADER_LEN + len]     = (uint8_t)(crc & 0xFF);
    f[BRIDGE_HEADER_LEN + len + 1] = (uint8_t)(crc >> 8);

    int total = BRIDGE_HEADER_LEN + len + BRIDGE_CRC_LEN;
    uart_write_bytes(uart_port_, (const char*)f, total);

    xSemaphoreGive(tx_lock_);
    return true;
}

void C5Bridge::RxTaskEntry(void* arg) {
    static_cast<C5Bridge*>(arg)->RxTask();
}

void C5Bridge::RxTask() {
    enum { ST_M0, ST_M1, ST_TYPE, ST_LINK, ST_L0, ST_L1, ST_PL, ST_C0, ST_C1 } st = ST_M0;
    static uint8_t payload[BRIDGE_MAX_PAYLOAD];
    static uint8_t rxbuf[512];
    uint8_t type = 0, link = 0;
    uint16_t len = 0, idx = 0, rx_crc = 0;

    while (true) {
        // 批量读取: 先阻塞等 1 字节 (避免凑不满整块而永久阻塞),
        // 再用 0 超时把 ring buffer 里已到的其余字节一次取走。
        int got = uart_read_bytes(uart_port_, rxbuf, 1, portMAX_DELAY);
        if (got <= 0) continue;
        int more = uart_read_bytes(uart_port_, rxbuf + 1, sizeof(rxbuf) - 1, 0);
        if (more > 0) got += more;

        for (int i = 0; i < got; i++) {
            uint8_t byte = rxbuf[i];
            switch (st) {
            case ST_M0: st = (byte == BRIDGE_MAGIC0) ? ST_M1 : ST_M0; break;
            case ST_M1: st = (byte == BRIDGE_MAGIC1) ? ST_TYPE : ST_M0; break;
            case ST_TYPE: type = byte; st = ST_LINK; break;
            case ST_LINK: link = byte; st = ST_L0; break;
            case ST_L0: len = byte; st = ST_L1; break;
            case ST_L1:
                len |= (uint16_t)byte << 8;
                if (len > BRIDGE_MAX_PAYLOAD) { st = ST_M0; break; }
                idx = 0; st = (len == 0) ? ST_C0 : ST_PL; break;
            case ST_PL: {
                // 批量拷贝当前 buffer 中属于本 payload 的连续字节
                uint16_t need = len - idx;
                int avail = got - i;
                int n = (avail < need) ? avail : need;
                memcpy(&payload[idx], &rxbuf[i], n);
                idx += n;
                i += n - 1;   // for 循环还会 +1
                if (idx >= len) st = ST_C0;
                break;
            }
            case ST_C0: rx_crc = byte; st = ST_C1; break;
            case ST_C1: {
                rx_crc |= (uint16_t)byte << 8;
                uint8_t meta[4] = { type, link, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
                uint16_t crc = crc16_ccitt(meta, 4, 0xFFFF);
                crc = crc16_ccitt(payload, len, crc);
                if (crc == rx_crc) {
                    DispatchFrame(type, link, payload, len);
                } else {
                    ESP_LOGW(TAG, "CRC mismatch type=0x%02x len=%u", type, len);
                }
                st = ST_M0;
                break;
            }
            }
        }
    }
}

void C5Bridge::DispatchFrame(uint8_t type, uint8_t link_id, const uint8_t* payload, uint16_t len) {
    switch (type) {
    case BRIDGE_EVT_READY:
        c5_ready_ = true;
        xEventGroupSetBits(events_, EV_C5_READY);
        ESP_LOGI(TAG, "C5 ready");
        break;

    case BRIDGE_EVT_PONG:
        xEventGroupSetBits(events_, EV_PONG);
        break;

    case BRIDGE_EVT_WIFI_STATUS: {
        if (len < sizeof(bridge_wifi_status_t)) break;
        auto* s = (const bridge_wifi_status_t*)payload;
        {
            std::lock_guard<std::mutex> lk(status_mutex_);
            wifi_connected_ = s->connected != 0;
            wifi_rssi_ = s->rssi;
            wifi_band_ = s->band;
            memcpy(wifi_ip_, s->ip, 4);
        }
        if (s->connected) {
            xEventGroupSetBits(events_, EV_WIFI_CONNECTED);
            ESP_LOGI(TAG, "WiFi connected rssi=%d band=%dG ip=%u.%u.%u.%u",
                     s->rssi, s->band == 2 ? 5 : 2, s->ip[0], s->ip[1], s->ip[2], s->ip[3]);
        } else {
            xEventGroupClearBits(events_, EV_WIFI_CONNECTED);
        }
        break;
    }

    case BRIDGE_EVT_SOCK_OPENED: {
        if (link_id >= BRIDGE_MAX_LINKS || len < sizeof(bridge_sock_result_t)) break;
        auto* r = (const bridge_sock_result_t*)payload;
        std::function<void(bool, int)> cb;
        {
            std::lock_guard<std::mutex> lk(links_mutex_);
            if (links_[link_id].in_use) cb = links_[link_id].on_opened;
        }
        if (cb) cb(r->ok != 0, r->err);
        break;
    }

    case BRIDGE_EVT_SOCK_DATA: {
        if (link_id >= BRIDGE_MAX_LINKS) break;
        std::function<void(const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lk(links_mutex_);
            if (links_[link_id].in_use) cb = links_[link_id].on_data;
        }
        if (cb) cb(std::string((const char*)payload, len));
        break;
    }

    case BRIDGE_EVT_SOCK_CLOSED: {
        if (link_id >= BRIDGE_MAX_LINKS) break;
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lk(links_mutex_);
            if (links_[link_id].in_use) cb = links_[link_id].on_closed;
        }
        if (cb) cb();
        break;
    }

    case BRIDGE_EVT_OTA_URL: {
        {
            std::lock_guard<std::mutex> lk(ota_url_mutex_);
            ota_url_.assign((const char*)payload, len);
        }
        xEventGroupSetBits(events_, EV_OTA_URL);
        ESP_LOGI(TAG, "received ota_url from C5 (len=%u)", len);
        break;
    }

    case BRIDGE_EVT_REBOOT_REQUEST:
        // C5 配网完成, 它即将带着新凭据重启。S3 现在的网络状态(socket/IP)全部
        // 失效, 且 StartNetwork 已经跑完不会再等, 所以只能重启重新走一遍。
        ESP_LOGW(TAG, "C5 requested reboot (provisioning done), restarting S3");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        break;

    case BRIDGE_EVT_WIFI_RESET_DONE:
        xEventGroupSetBits(events_, EV_WIFI_RESET_DONE);
        ESP_LOGW(TAG, "C5 wifi credentials cleared, C5 is rebooting to config mode");
        break;

    case BRIDGE_EVT_NEED_PROVISION:
        // C5 每 5 秒重发一次 (防止开机时序竞态丢帧), 这里只在状态翻转时打日志,
        // 否则会把串口刷满。
        if (!needs_provision_) {
            ESP_LOGW(TAG, "C5 has no usable credentials, S3 should start provisioning AP");
        }
        needs_provision_ = true;
        xEventGroupSetBits(events_, EV_NEED_PROVISION);
        break;

    case BRIDGE_EVT_WIFI_SCAN_RESULT: {
        // payload: count(1) + count×{ rssi(i8), authmode(1), ssid_len(1), ssid[] }
        std::vector<C5ApInfo> list;
        if (len >= 1) {
            uint8_t count = payload[0];
            size_t off = 1;
            for (uint8_t i = 0; i < count; i++) {
                if (off + 3 > len) break;
                C5ApInfo ap;
                ap.rssi = (int8_t)payload[off];
                ap.authmode = payload[off + 1];
                uint8_t sl = payload[off + 2];
                off += 3;
                if (off + sl > len) break;
                ap.ssid.assign((const char*)payload + off, sl);
                off += sl;
                list.push_back(std::move(ap));
            }
        }
        {
            std::lock_guard<std::mutex> lk(scan_mutex_);
            scan_results_ = std::move(list);
        }
        xEventGroupSetBits(events_, EV_SCAN_RESULT);
        break;
    }

    case BRIDGE_EVT_WIFI_CONFIG_RESULT: {
        // payload: ok(1) + 其余字节为错误文案
        std::lock_guard<std::mutex> lk(config_mutex_);
        config_ok_ = (len >= 1 && payload[0] == 1);
        config_error_.clear();
        if (!config_ok_ && len > 1) {
            config_error_.assign((const char*)payload + 1, len - 1);
        }
        xEventGroupSetBits(events_, EV_CONFIG_RESULT);
        ESP_LOGI(TAG, "wifi config result: ok=%d err=%s",
                 config_ok_ ? 1 : 0, config_error_.c_str());
        break;
    }

    case BRIDGE_EVT_LOG:
        ESP_LOGI("C5", "%.*s", (int)len, (const char*)payload);
        break;

    default:
        ESP_LOGW(TAG, "unknown frame 0x%02x", type);
        break;
    }
}

bool C5Bridge::WaitForNetworkReady(int timeout_ms) {
    // 先等 C5 就绪 (不清除标志位, 允许多次查询)。
    // 同 WaitForNetworkOrProvision: READY 只发一次且早于 S3 起 UART, 收不到时
    // 别在这里干等满 10 秒 —— 已经联网的信号同样能证明 C5 活着。
    if (!c5_ready_) {
        xEventGroupWaitBits(events_, EV_C5_READY | EV_WIFI_CONNECTED,
                            pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    }
    // 再等 WiFi 连接 (C5 自主配网/连接)
    EventBits_t bits = xEventGroupWaitBits(events_, EV_WIFI_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & EV_WIFI_CONNECTED) != 0;
}

std::string C5Bridge::GetIpAddress() const {
    std::lock_guard<std::mutex> lk(status_mutex_);
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", wifi_ip_[0], wifi_ip_[1], wifi_ip_[2], wifi_ip_[3]);
    return std::string(buf);
}

int C5Bridge::AllocLink() {
    std::lock_guard<std::mutex> lk(links_mutex_);
    for (int i = 0; i < BRIDGE_MAX_LINKS; i++) {
        if (!links_[i].in_use) {
            links_[i] = C5Link{};
            links_[i].in_use = true;
            return i;
        }
    }
    return -1;
}

void C5Bridge::FreeLink(int link_id) {
    if (link_id < 0 || link_id >= BRIDGE_MAX_LINKS) return;
    std::lock_guard<std::mutex> lk(links_mutex_);
    links_[link_id] = C5Link{};
}

void C5Bridge::SetLinkCallbacks(int link_id,
                                std::function<void(bool, int)> on_opened,
                                std::function<void(const std::string&)> on_data,
                                std::function<void()> on_closed) {
    if (link_id < 0 || link_id >= BRIDGE_MAX_LINKS) return;
    std::lock_guard<std::mutex> lk(links_mutex_);
    links_[link_id].on_opened = std::move(on_opened);
    links_[link_id].on_data = std::move(on_data);
    links_[link_id].on_closed = std::move(on_closed);
}

bool C5Bridge::SockOpen(int link_id, bridge_proto_t proto, const std::string& host, uint16_t port) {
    if (link_id < 0 || link_id >= BRIDGE_MAX_LINKS) return false;
    if (host.size() > 255) return false;

    uint8_t buf[sizeof(bridge_sock_open_t) + 256];
    auto* o = (bridge_sock_open_t*)buf;
    o->proto = (uint8_t)proto;
    o->port = port;                 // 小端, 两端同为小端架构
    o->host_len = (uint8_t)host.size();
    memcpy(buf + sizeof(bridge_sock_open_t), host.data(), host.size());
    return SendFrame(BRIDGE_CMD_SOCK_OPEN, (uint8_t)link_id, buf,
                     (uint16_t)(sizeof(bridge_sock_open_t) + host.size()));
}

bool C5Bridge::SockSend(int link_id, const void* data, size_t len) {
    if (link_id < 0 || link_id >= BRIDGE_MAX_LINKS) return false;
    // 分片: 单帧 <= BRIDGE_MAX_PAYLOAD
    const uint8_t* p = (const uint8_t*)data;
    size_t remaining = len;
    while (remaining > 0) {
        uint16_t chunk = (uint16_t)(remaining > BRIDGE_MAX_PAYLOAD ? BRIDGE_MAX_PAYLOAD : remaining);
        if (!SendFrame(BRIDGE_CMD_SOCK_SEND, (uint8_t)link_id, p, chunk)) return false;
        p += chunk;
        remaining -= chunk;
    }
    return true;
}

void C5Bridge::SockClose(int link_id) {
    if (link_id < 0 || link_id >= BRIDGE_MAX_LINKS) return;
    SendFrame(BRIDGE_CMD_SOCK_CLOSE, (uint8_t)link_id, nullptr, 0);
}

void C5Bridge::RequestStatus() {
    SendFrame(BRIDGE_CMD_GET_STATUS, BRIDGE_NO_LINK, nullptr, 0);
}

void C5Bridge::RequestWifiStart() {
    SendFrame(BRIDGE_CMD_WIFI_CONNECT, BRIDGE_NO_LINK, nullptr, 0);
}

bool C5Bridge::ResetWifiCredentials(int timeout_ms) {
    xEventGroupClearBits(events_, EV_WIFI_RESET_DONE);
    if (!SendFrame(BRIDGE_CMD_WIFI_RESET, BRIDGE_NO_LINK, nullptr, 0)) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(events_, EV_WIFI_RESET_DONE, pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & EV_WIFI_RESET_DONE)) {
        ESP_LOGE(TAG, "wifi reset: no ack from C5 within %d ms", timeout_ms);
        return false;
    }
    // C5 收到后会自行重启, 本端的 WiFi 状态立即失效
    {
        std::lock_guard<std::mutex> lk(status_mutex_);
        wifi_connected_ = false;
    }
    xEventGroupClearBits(events_, EV_WIFI_CONNECTED);
    return true;
}

bool C5Bridge::WaitForNetworkOrProvision(int timeout_ms) {
    // 这里以前只等 EV_C5_READY, 等不到就干烧满 10 秒。
    //
    // C5 的 EVT_READY 在它开机约 0.4s 就发出去了, 而 S3 这边要到 ~2.9s 才起
    // UART 接收任务 (前面还有 LCD/LVGL/音频编解码器的初始化), 那一帧因此必然
    // 丢掉 —— READY 只发一次, 不重发。实测 bridge 启动 2862ms + 10000ms 超时
    // = 12862ms 才往下走, 而 C5 其实 3152ms 就拿到 IP 了, 白等近 10 秒, 表现为
    // "C5 早连上了, S3 却迟迟不进 idle, 眼睛跟着晚亮 10 秒"。
    //
    // 改成三个信号任一到达就继续: WIFI_CONNECTED / NEED_PROVISION 本来就是我们
    // 真正要等的结果, 它们到了就说明 C5 活着, 再等 READY 没有意义。
    if (!c5_ready_) {
        xEventGroupWaitBits(events_, EV_C5_READY | EV_WIFI_CONNECTED | EV_NEED_PROVISION,
                            pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    }
    // 两个结果任一到达就返回, 不必空等满超时:
    // C5 一开机就知道自己有没有凭据, 没有的话会立刻发 NEED_PROVISION。
    EventBits_t bits = xEventGroupWaitBits(events_, EV_WIFI_CONNECTED | EV_NEED_PROVISION,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & EV_WIFI_CONNECTED) != 0;
}

bool C5Bridge::RequestScan(std::vector<C5ApInfo>& out, int timeout_ms) {
    xEventGroupClearBits(events_, EV_SCAN_RESULT);
    if (!SendFrame(BRIDGE_CMD_WIFI_SCAN, BRIDGE_NO_LINK, nullptr, 0)) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(events_, EV_SCAN_RESULT, pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & EV_SCAN_RESULT)) {
        ESP_LOGW(TAG, "scan: no result from C5 within %d ms", timeout_ms);
        return false;
    }
    std::lock_guard<std::mutex> lk(scan_mutex_);
    out = scan_results_;
    return true;
}

bool C5Bridge::SendWifiConfig(const std::string& ssid, const std::string& password,
                              std::string& error_out, int timeout_ms) {
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
        error_out = "SSID 或密码长度不合法";
        return false;
    }

    // payload: ssid_len(1) + ssid + pwd_len(1) + pwd
    std::vector<uint8_t> buf;
    buf.push_back((uint8_t)ssid.size());
    buf.insert(buf.end(), ssid.begin(), ssid.end());
    buf.push_back((uint8_t)password.size());
    buf.insert(buf.end(), password.begin(), password.end());

    xEventGroupClearBits(events_, EV_CONFIG_RESULT);
    if (!SendFrame(BRIDGE_CMD_WIFI_CONFIG, BRIDGE_NO_LINK, buf.data(), (uint16_t)buf.size())) {
        error_out = "无法与 WiFi 模块通信";
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(events_, EV_CONFIG_RESULT, pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & EV_CONFIG_RESULT)) {
        error_out = "WiFi 模块无响应, 请重试";
        return false;
    }

    std::lock_guard<std::mutex> lk(config_mutex_);
    if (!config_ok_) {
        error_out = config_error_.empty() ? "连接失败" : config_error_;
        return false;
    }
    return true;
}

bool C5Bridge::PingC5(int timeout_ms) {
    xEventGroupClearBits(events_, EV_PONG);
    SendFrame(BRIDGE_CMD_PING, BRIDGE_NO_LINK, nullptr, 0);
    EventBits_t bits = xEventGroupWaitBits(events_, EV_PONG, pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & EV_PONG) != 0;
}

bool C5Bridge::FetchOtaUrl(std::string& out_url, int timeout_ms) {
    xEventGroupClearBits(events_, EV_OTA_URL);
    SendFrame(BRIDGE_CMD_GET_OTA_URL, BRIDGE_NO_LINK, nullptr, 0);
    EventBits_t bits = xEventGroupWaitBits(events_, EV_OTA_URL, pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & EV_OTA_URL)) {
        return false;   // 超时
    }
    std::lock_guard<std::mutex> lk(ota_url_mutex_);
    out_url = ota_url_;
    return true;
}
