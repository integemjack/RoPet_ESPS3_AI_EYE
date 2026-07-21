/*
 * c5_udp.cc
 */
#include "c5_udp.h"

#include <chrono>
#include <esp_log.h>

#define TAG "C5Udp"

C5Udp::C5Udp(C5Bridge& bridge) : bridge_(bridge) {
}

C5Udp::~C5Udp() {
    Disconnect();
}

bool C5Udp::Connect(const std::string& host, int port) {
    link_id_ = bridge_.AllocLink();
    if (link_id_ < 0) {
        return false;
    }

    open_done_ = false;
    open_ok_ = false;

    bridge_.SetLinkCallbacks(link_id_,
        [this](bool ok, int err) {
            std::lock_guard<std::mutex> lk(mutex_);
            open_ok_ = ok;
            open_done_ = true;
            cv_.notify_all();
        },
        [this](const std::string& data) {
            if (message_callback_) message_callback_(data);
        },
        [this]() {
            connected_ = false;
        });

    if (!bridge_.SockOpen(link_id_, BRIDGE_PROTO_UDP, host, (uint16_t)port)) {
        bridge_.FreeLink(link_id_);
        link_id_ = -1;
        return false;
    }

    std::unique_lock<std::mutex> lk(mutex_);
    if (!cv_.wait_for(lk, std::chrono::seconds(10), [this]() { return open_done_; })) {
        lk.unlock();
        bridge_.SockClose(link_id_);
        bridge_.FreeLink(link_id_);
        link_id_ = -1;
        return false;
    }
    connected_ = open_ok_;
    if (!open_ok_) {
        lk.unlock();
        bridge_.FreeLink(link_id_);
        link_id_ = -1;
        return false;
    }
    ESP_LOGI(TAG, "udp connected link=%d %s:%d", link_id_, host.c_str(), port);
    return true;
}

void C5Udp::Disconnect() {
    if (link_id_ >= 0) {
        bridge_.SockClose(link_id_);
        bridge_.FreeLink(link_id_);
        link_id_ = -1;
    }
    connected_ = false;
}

int C5Udp::Send(const std::string& data) {
    if (link_id_ < 0 || !connected_) return -1;
    if (!bridge_.SockSend(link_id_, data.data(), data.size())) return -1;
    return (int)data.size();
}
