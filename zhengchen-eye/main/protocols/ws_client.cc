#include "ws_client.h"

#include <cstring>
#include <vector>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <network_interface.h>

#define TAG "WsClient"

const char* WsCloseReasonToString(WsCloseReason reason) {
    switch (reason) {
        case kWsCloseLocal:         return "local close";
        case kWsClosePeer:          return "peer close frame";
        case kWsCloseTransport:     return "transport closed (FIN/error)";
        case kWsClosePingTimeout:   return "no data within heartbeat window";
        case kWsCloseProtocolError: return "protocol error";
    }
    return "unknown";
}

static std::string Base64Encode(const uint8_t* data, size_t len) {
    static const char* kChars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        size_t chunk = len - i < 3 ? len - i : 3;
        uint32_t v = 0;
        for (size_t j = 0; j < 3; j++) {
            v = (v << 8) | (j < chunk ? data[i + j] : 0);
        }
        out.push_back(kChars[(v >> 18) & 0x3F]);
        out.push_back(kChars[(v >> 12) & 0x3F]);
        out.push_back(chunk > 1 ? kChars[(v >> 6) & 0x3F] : '=');
        out.push_back(chunk > 2 ? kChars[v & 0x3F] : '=');
    }
    return out;
}

WsClient::WsClient(NetworkInterface* network, int connect_id)
    : network_(network), connect_id_(connect_id) {
    events_ = xEventGroupCreate();
}

WsClient::~WsClient() {
    /* 主动销毁: 不再上报断开 (调用方已经知道), 也不要在析构里回调出去 —— 回调
     * 里很可能又访问本对象。 */
    disconnect_reported_ = true;
    connected_ = false;
    StopHeartbeat();

    if (tcp_ != nullptr) {
        /* 先断链: C5Tcp::Disconnect() 会释放网桥 link 并清掉回调表, 之后不会再
         * 有新的 dispatch 打到这些 lambda 上。 */
        tcp_->Disconnect();
    }
    on_data_ = nullptr;
    on_disconnected_ = nullptr;
    on_connected_ = nullptr;
    on_error_ = nullptr;
    tcp_.reset();

    if (events_ != nullptr) {
        vEventGroupDelete(events_);
    }
}

void WsClient::SetHeader(const char* key, const char* value) {
    headers_[key] = value;
}

void WsClient::EnableHeartbeat(int interval_s, int timeout_s) {
    heartbeat_interval_s_ = interval_s;
    heartbeat_timeout_s_ = timeout_s;
}

bool WsClient::Connect(const char* uri) {
    std::string uri_str(uri ? uri : "");

    auto scheme_end = uri_str.find("://");
    if (scheme_end == std::string::npos) {
        ESP_LOGE(TAG, "invalid uri: %s", uri_str.c_str());
        return false;
    }
    std::string protocol = uri_str.substr(0, scheme_end);
    std::string rest = uri_str.substr(scheme_end + 3);

    /* path 先切掉, 再判端口 —— 否则 path 里的 ':' 会被当成端口分隔符 */
    std::string host_port = rest;
    std::string path = "/";
    auto slash = rest.find('/');
    if (slash != std::string::npos) {
        host_port = rest.substr(0, slash);
        path = rest.substr(slash);
    }

    bool tls = (protocol == "wss" || protocol == "https");
    std::string host = host_port;
    int port = tls ? 443 : 80;
    auto colon = host_port.rfind(':');
    if (colon != std::string::npos && host_port.find(']') == std::string::npos) {
        host = host_port.substr(0, colon);
        port = atoi(host_port.c_str() + colon + 1);
        if (port <= 0 || port > 65535) {
            ESP_LOGE(TAG, "invalid port in uri: %s", uri_str.c_str());
            return false;
        }
    }

    /* 复位接收侧状态。这些以前是函数内 static, 重连时会带着上一条连接的残留。 */
    receive_buffer_.clear();
    current_message_.clear();
    message_fragmented_ = false;
    message_binary_ = false;
    handshake_completed_ = false;
    continuation_ = false;
    connected_ = false;
    disconnect_reported_ = true;  // 握手成功后才允许上报断开

    SetHeader("Upgrade", "websocket");
    SetHeader("Connection", "Upgrade");
    SetHeader("Sec-WebSocket-Version", "13");
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    SetHeader("Sec-WebSocket-Key", Base64Encode(nonce, sizeof(nonce)).c_str());

    tcp_ = tls ? network_->CreateSsl(connect_id_) : network_->CreateTcp(connect_id_);
    if (tcp_ == nullptr) {
        ESP_LOGE(TAG, "failed to create transport");
        return false;
    }

    xEventGroupClearBits(events_, kHandshakeOk | kHandshakeFailed);

    if (!tcp_->Connect(host, port)) {
        ESP_LOGE(TAG, "failed to connect %s:%d", host.c_str(), port);
        return false;
    }

    tcp_->OnStream([this](const std::string& data) { OnTcpData(data); });
    tcp_->OnDisconnected([this]() {
        if (!handshake_completed_) {
            /* 握手期间断链: 唤醒下面的等待, 别干等满超时 */
            xEventGroupSetBits(events_, kHandshakeFailed);
            return;
        }
        connected_ = false;
        ReportDisconnected(kWsCloseTransport);
    });

    std::string request = "GET " + path + " HTTP/1.1\r\n";
    if (headers_.find("Host") == headers_.end()) {
        request += "Host: " + host + "\r\n";
    }
    for (const auto& h : headers_) {
        request += h.first + ": " + h.second + "\r\n";
    }
    request += "\r\n";

    if (tcp_->Send(request) < 0) {
        ESP_LOGE(TAG, "failed to send handshake request");
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(events_, kHandshakeOk | kHandshakeFailed,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & kHandshakeOk)) {
        ESP_LOGE(TAG, "handshake %s", (bits & kHandshakeFailed) ? "failed" : "timeout");
        if (on_error_) {
            on_error_(-1);
        }
        return false;
    }

    connected_ = true;
    disconnect_reported_ = false;
    last_incoming_us_ = esp_timer_get_time();
    StartHeartbeat();

    if (on_connected_) {
        on_connected_();
    }
    return true;
}

void WsClient::Close() {
    if (connected_.exchange(false)) {
        SendControlFrame(0x8, nullptr, 0);
    }
    StopHeartbeat();
    ReportDisconnected(kWsCloseLocal);
}

void WsClient::ReportDisconnected(WsCloseReason reason) {
    if (disconnect_reported_.exchange(true)) {
        return;  // 只报一次
    }
    ESP_LOGW(TAG, "disconnected: %s", WsCloseReasonToString(reason));
    if (on_disconnected_) {
        on_disconnected_(reason);
    }
}

/* ---------------- 发送 ---------------- */

bool WsClient::Send(const std::string& text) {
    return Send(text.data(), text.size(), false);
}

bool WsClient::Send(const void* data, size_t len, bool binary, bool fin) {
    uint8_t first = fin ? 0x80 : 0x00;
    if (binary) {
        first |= 0x02;
    } else if (!continuation_) {
        first |= 0x01;
    }  // 否则 opcode 0 = 延续帧

    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!connected_ || tcp_ == nullptr) {
        return false;
    }
    if (!SendFrameLocked(data, len, first)) {
        return false;
    }
    continuation_ = !fin;
    return true;
}

bool WsClient::Ping() {
    return SendControlFrame(0x9, nullptr, 0);
}

bool WsClient::SendControlFrame(uint8_t opcode, const void* data, size_t len) {
    if (len > 125) {
        ESP_LOGE(TAG, "control frame payload too large: %u", (unsigned)len);
        return false;
    }
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (tcp_ == nullptr) {
        return false;
    }
    return SendFrameLocked(data, len, (uint8_t)(0x80 | opcode));
}

/* 必须持有 send_mutex_ 调用。整帧一次 Send 出去 —— 分多次写会让并发发送的帧
 * 在传输层交错, 对端只能看到一堆垃圾然后断开。 */
bool WsClient::SendFrameLocked(const void* data, size_t len, uint8_t first_byte) {
    std::string frame;
    frame.reserve(len + 14);
    frame.push_back((char)first_byte);

    if (len < 126) {
        frame.push_back((char)(0x80 | len));
    } else if (len <= 0xFFFF) {
        frame.push_back((char)(0x80 | 126));
        frame.push_back((char)((len >> 8) & 0xFF));
        frame.push_back((char)(len & 0xFF));
    } else {
        frame.push_back((char)(0x80 | 127));
        for (int i = 7; i >= 0; i--) {
            frame.push_back((char)((uint64_t)len >> (i * 8) & 0xFF));
        }
    }

    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    frame.append((const char*)mask, 4);

    auto payload = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        frame.push_back((char)(payload[i] ^ mask[i & 3]));
    }

    return tcp_->Send(frame) >= 0;
}

/* ---------------- 接收 ---------------- */

void WsClient::OnTcpData(const std::string& data) {
    last_incoming_us_ = esp_timer_get_time();
    receive_buffer_.append(data);

    if (!handshake_completed_) {
        auto end = receive_buffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (receive_buffer_.size() > 4096) {
                ESP_LOGE(TAG, "handshake response too large");
                receive_buffer_.clear();
                xEventGroupSetBits(events_, kHandshakeFailed);
            }
            return;
        }
        std::string response = receive_buffer_.substr(0, end + 4);
        receive_buffer_.erase(0, end + 4);

        if (response.find("HTTP/1.1 101") == std::string::npos) {
            auto line_end = response.find("\r\n");
            ESP_LOGE(TAG, "handshake rejected: %s",
                     response.substr(0, line_end == std::string::npos ? response.size() : line_end).c_str());
            xEventGroupSetBits(events_, kHandshakeFailed);
            return;
        }
        handshake_completed_ = true;
        xEventGroupSetBits(events_, kHandshakeOk);
    }

    if (!ParseFrames()) {
        connected_ = false;
        if (tcp_ != nullptr) {
            tcp_->Disconnect();
        }
        ReportDisconnected(kWsCloseProtocolError);
    }
}

bool WsClient::ParseFrames() {
    size_t off = 0;
    /* 全程用 uint8_t 取值。原实现这里是 const char* (xtensa 上 char 有符号),
     * 低字节 >= 0x80 的长度会算成负数再转成巨大的 uint64 —— 接收缓冲从此不再
     * 前进, 连接静默变哑。 */
    auto buf = (const uint8_t*)receive_buffer_.data();
    size_t size = receive_buffer_.size();

    while (true) {
        if (size - off < 2) break;

        uint8_t b0 = buf[off];
        uint8_t b1 = buf[off + 1];
        bool fin = (b0 & 0x80) != 0;
        uint8_t opcode = b0 & 0x0F;
        bool masked = (b1 & 0x80) != 0;
        uint64_t payload_len = b1 & 0x7F;
        size_t header = 2;

        if (payload_len == 126) {
            if (size - off < 4) break;
            payload_len = ((uint64_t)buf[off + 2] << 8) | buf[off + 3];
            header = 4;
        } else if (payload_len == 127) {
            if (size - off < 10) break;
            payload_len = 0;
            for (int i = 0; i < 8; i++) {
                payload_len = (payload_len << 8) | buf[off + 2 + i];
            }
            header = 10;
        }

        if (payload_len > kMaxFramePayload) {
            ESP_LOGE(TAG, "frame payload too large: %llu", (unsigned long long)payload_len);
            return false;
        }

        const uint8_t* mask_key = nullptr;
        if (masked) {
            if (size - off < header + 4) break;
            mask_key = buf + off + header;
            header += 4;
        }

        if (size - off < header + payload_len) break;  // 等更多数据

        if (opcode >= 0x8 && (!fin || payload_len > 125)) {
            ESP_LOGE(TAG, "invalid control frame: opcode=%u fin=%d len=%llu",
                     opcode, (int)fin, (unsigned long long)payload_len);
            return false;
        }

        std::string payload((const char*)(buf + off + header), payload_len);
        if (mask_key != nullptr) {
            for (size_t i = 0; i < payload.size(); i++) {
                payload[i] = (char)((uint8_t)payload[i] ^ mask_key[i & 3]);
            }
        }
        off += header + payload_len;

        switch (opcode) {
            case 0x0:  // 延续帧
                if (!message_fragmented_) {
                    ESP_LOGE(TAG, "continuation frame without a started message");
                    return false;
                }
                current_message_.append(payload);
                break;
            case 0x1:  // 文本
            case 0x2:  // 二进制
                if (message_fragmented_) {
                    ESP_LOGE(TAG, "new message frame while still fragmenting");
                    return false;
                }
                message_binary_ = (opcode == 0x2);
                current_message_ = std::move(payload);
                message_fragmented_ = !fin;
                break;
            case 0x8:  // close
                connected_ = false;
                if (tcp_ != nullptr) {
                    tcp_->Disconnect();
                }
                if (payload.size() >= 2) {
                    uint16_t code = ((uint8_t)payload[0] << 8) | (uint8_t)payload[1];
                    ESP_LOGW(TAG, "peer close: code=%u reason=%.*s", code,
                             (int)(payload.size() - 2), payload.data() + 2);
                }
                ReportDisconnected(kWsClosePeer);
                receive_buffer_.clear();
                return true;
            case 0x9:  // ping -> pong
                SendControlFrame(0xA, payload.data(), payload.size());
                continue;
            case 0xA:  // pong: last_incoming_us_ 已在 OnTcpData 更新
                continue;
            default:
                ESP_LOGE(TAG, "unknown opcode: %u", opcode);
                return false;
        }

        if (opcode <= 0x2 && fin) {
            message_fragmented_ = false;
            if (on_data_) {
                on_data_(current_message_.data(), current_message_.size(), message_binary_);
            }
            current_message_.clear();
        }
    }

    if (off > 0) {
        receive_buffer_.erase(0, off);
    }
    return true;
}

/* ---------------- 心跳 ---------------- */

void WsClient::StartHeartbeat() {
    if (heartbeat_timeout_s_ <= 0 || heartbeat_interval_s_ <= 0) {
        return;
    }
    if (heartbeat_task_ != nullptr) {
        return;
    }
    xEventGroupClearBits(events_, kHeartbeatStop);
    xTaskCreate([](void* arg) {
        auto self = (WsClient*)arg;
        self->HeartbeatTask();
        vTaskDelete(NULL);
    }, "ws_hb", 3072, this, 2, &heartbeat_task_);
}

void WsClient::StopHeartbeat() {
    if (heartbeat_task_ == nullptr) {
        return;
    }
    xEventGroupSetBits(events_, kHeartbeatStop);
    /* 等它自己退出, 否则任务里可能还在访问已析构的成员 */
    for (int i = 0; i < 200 && heartbeat_task_ != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void WsClient::HeartbeatTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(events_, kHeartbeatStop, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(heartbeat_interval_s_ * 1000));
        if (bits & kHeartbeatStop) {
            break;
        }
        if (!connected_) {
            break;
        }

        auto idle_us = esp_timer_get_time() - last_incoming_us_.load();
        if (idle_us > (int64_t)heartbeat_timeout_s_ * 1000000) {
            ESP_LOGW(TAG, "no incoming data for %lld s, treating link as dead",
                     (long long)(idle_us / 1000000));
            connected_ = false;
            if (tcp_ != nullptr) {
                tcp_->Disconnect();
            }
            ReportDisconnected(kWsClosePingTimeout);
            break;
        }

        if (!Ping()) {
            ESP_LOGW(TAG, "failed to send ping");
        }
    }
    heartbeat_task_ = nullptr;
}
