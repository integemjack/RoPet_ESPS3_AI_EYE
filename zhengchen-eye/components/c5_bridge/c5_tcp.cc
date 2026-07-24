/*
 * c5_tcp.cc - Tcp 抽象 (esp-ml307 3.x) 在 C5 网桥 link 上的实现
 */
#include "c5_tcp.h"

#include <chrono>
#include <esp_log.h>

#define TAG "C5Tcp"

C5Tcp::C5Tcp(C5Bridge& bridge, bool tls) : bridge_(bridge), tls_(tls) {}

C5Tcp::~C5Tcp() {
    Disconnect();
}

bool C5Tcp::Connect(const std::string& host, int port) {
    link_id_ = bridge_.AllocLink();
    if (link_id_ < 0) {
        ESP_LOGE(TAG, "no free link");
        return false;
    }

    open_done_ = false;
    open_ok_ = false;
    pending_rx_.clear();

    bridge_.SetLinkCallbacks(link_id_,
        [this](bool ok, int err) {
            std::lock_guard<std::mutex> lk(mutex_);
            open_ok_ = ok;
            open_done_ = true;
            cv_.notify_all();
        },
        [this](const std::string& data) {
            std::function<void(const std::string&)> cb;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (!stream_callback_) {
                    // 回调还没设置, 先缓存
                    pending_rx_.append(data);
                    return;
                }
                cb = stream_callback_;
            }
            cb(data);
        },
        [this]() {
            connected_ = false;
            if (disconnect_callback_) {
                disconnect_callback_();
            }
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
    ESP_LOGI(TAG, "connected link=%d %s:%d tls=%d", link_id_, host.c_str(), port, tls_);
    return true;
}

void C5Tcp::OnStream(std::function<void(const std::string& data)> callback) {
    std::string flush;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stream_callback_ = std::move(callback);
        // 把连接建立到设置回调之间缓存的数据补发出去
        if (stream_callback_ && !pending_rx_.empty()) {
            flush.swap(pending_rx_);
        }
    }
    if (!flush.empty() && stream_callback_) {
        stream_callback_(flush);
    }
}

void C5Tcp::Disconnect() {
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

int C5Tcp::Send(const std::string& data) {
    if (link_id_ < 0 || !connected_) return -1;
    if (!bridge_.SockSend(link_id_, data.data(), data.size())) return -1;
    return (int)data.size();
}
