/*
 * c5_transport.cc
 */
#include "c5_transport.h"

#include <cstring>
#include <chrono>
#include <esp_log.h>

#define TAG "C5Transport"

C5Transport::C5Transport(C5Bridge& bridge, bool tls) : bridge_(bridge), tls_(tls) {}

C5Transport::~C5Transport() {
    Disconnect();
}

bool C5Transport::Connect(const char* host, int port) {
    link_id_ = bridge_.AllocLink();
    if (link_id_ < 0) {
        ESP_LOGE(TAG, "no free link");
        return false;
    }

    open_done_ = false;
    open_ok_ = false;
    closed_ = false;
    rx_buffer_.clear();

    bridge_.SetLinkCallbacks(link_id_,
        [this](bool ok, int err) {
            std::lock_guard<std::mutex> lk(mutex_);
            open_ok_ = ok;
            open_done_ = true;
            cv_.notify_all();
        },
        [this](const std::string& data) {
            std::lock_guard<std::mutex> lk(mutex_);
            rx_buffer_.append(data);
            cv_.notify_all();
        },
        [this]() {
            std::lock_guard<std::mutex> lk(mutex_);
            closed_ = true;
            connected_ = false;
            cv_.notify_all();
        });

    bridge_proto_t proto = tls_ ? BRIDGE_PROTO_TLS : BRIDGE_PROTO_TCP;
    if (!bridge_.SockOpen(link_id_, proto, host, (uint16_t)port)) {
        bridge_.FreeLink(link_id_);
        link_id_ = -1;
        return false;
    }

    std::unique_lock<std::mutex> lk(mutex_);
    if (!cv_.wait_for(lk, std::chrono::seconds(15), [this]() { return open_done_; })) {
        ESP_LOGE(TAG, "connect timeout");
        lk.unlock();
        bridge_.SockClose(link_id_);
        bridge_.FreeLink(link_id_);
        link_id_ = -1;
        return false;
    }
    if (!open_ok_) {
        lk.unlock();
        bridge_.FreeLink(link_id_);
        link_id_ = -1;
        return false;
    }
    connected_ = true;
    ESP_LOGI(TAG, "connected link=%d %s:%d tls=%d", link_id_, host, port, tls_);
    return true;
}

void C5Transport::Disconnect() {
    if (link_id_ >= 0) {
        bridge_.SockClose(link_id_);
        bridge_.FreeLink(link_id_);
        link_id_ = -1;
    }
    connected_ = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cv_.notify_all();
    }
}

int C5Transport::Send(const char* data, size_t length) {
    if (link_id_ < 0 || !connected_) return -1;
    if (!bridge_.SockSend(link_id_, data, length)) return -1;
    return (int)length;
}

// 同步阻塞读: 有数据即返回; 连接关闭返回 0; (WebSocket 在独立线程调用)
int C5Transport::Receive(char* buffer, size_t bufferSize) {
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait(lk, [this]() { return !rx_buffer_.empty() || closed_ || link_id_ < 0; });

    if (!rx_buffer_.empty()) {
        size_t n = std::min(bufferSize, rx_buffer_.size());
        memcpy(buffer, rx_buffer_.data(), n);
        rx_buffer_.erase(0, n);
        return (int)n;
    }
    // 关闭 / 已断开
    return 0;
}
