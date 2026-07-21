/*
 * c5_mqtt.cc - 精简 MQTT 3.1.1 客户端 (跑在 C5 网桥的 TCP/TLS 传输上)
 */
#include "c5_mqtt.h"

#include <cstring>
#include <esp_log.h>

#define TAG "C5Mqtt"

enum {
    MQTT_CONNECT     = 0x10,
    MQTT_CONNACK     = 0x20,
    MQTT_PUBLISH     = 0x30,
    MQTT_PUBACK      = 0x40,
    MQTT_SUBSCRIBE   = 0x80,
    MQTT_SUBACK      = 0x90,
    MQTT_UNSUBSCRIBE = 0xA0,
    MQTT_UNSUBACK    = 0xB0,
    MQTT_PINGREQ     = 0xC0,
    MQTT_PINGRESP    = 0xD0,
    MQTT_DISCONNECT  = 0xE0,
};

C5Mqtt::C5Mqtt(C5Bridge& bridge) : bridge_(bridge) {}

C5Mqtt::~C5Mqtt() {
    Disconnect();
}

void C5Mqtt::EncodeRemainingLength(std::vector<uint8_t>& out, uint32_t len) {
    do {
        uint8_t byte = len % 128;
        len /= 128;
        if (len > 0) byte |= 0x80;
        out.push_back(byte);
    } while (len > 0);
}

void C5Mqtt::EncodeString(std::vector<uint8_t>& out, const std::string& s) {
    out.push_back((uint8_t)(s.size() >> 8));
    out.push_back((uint8_t)(s.size() & 0xFF));
    out.insert(out.end(), s.begin(), s.end());
}

bool C5Mqtt::SendPacket(const std::vector<uint8_t>& pkt) {
    if (!transport_) return false;
    std::lock_guard<std::mutex> lk(send_mutex_);
    return transport_->Send((const char*)pkt.data(), pkt.size()) == (int)pkt.size();
}

bool C5Mqtt::Connect(const std::string broker_address, int broker_port,
                     const std::string client_id, const std::string username,
                     const std::string password) {
    // 建立底层传输 (8883 -> TLS)
    bool tls = (broker_port == 8883);
    transport_ = std::make_unique<C5Transport>(bridge_, tls);
    if (!transport_->Connect(broker_address.c_str(), broker_port)) {
        ESP_LOGE(TAG, "transport connect failed");
        transport_.reset();
        return false;
    }

    // ---- 组 CONNECT 报文 ----
    std::vector<uint8_t> vh;
    EncodeString(vh, "MQTT");
    vh.push_back(0x04);           // protocol level 3.1.1
    uint8_t flags = 0x02;         // clean session
    if (!username.empty()) flags |= 0x80;
    if (!password.empty()) flags |= 0x40;
    vh.push_back(flags);
    int ka = keep_alive_seconds_;
    vh.push_back((uint8_t)(ka >> 8));
    vh.push_back((uint8_t)(ka & 0xFF));

    std::vector<uint8_t> payload;
    EncodeString(payload, client_id);
    if (!username.empty()) EncodeString(payload, username);
    if (!password.empty()) EncodeString(payload, password);

    std::vector<uint8_t> pkt;
    pkt.push_back(MQTT_CONNECT);
    std::vector<uint8_t> rest;
    rest.insert(rest.end(), vh.begin(), vh.end());
    rest.insert(rest.end(), payload.begin(), payload.end());
    EncodeRemainingLength(pkt, rest.size());
    pkt.insert(pkt.end(), rest.begin(), rest.end());

    connack_received_ = false;
    connack_ok_ = false;

    // 先启动接收线程, 再发 CONNECT (否则可能错过 CONNACK)
    running_ = true;
    xTaskCreate(RxTaskEntry, "c5mqtt_rx", 6144, this, 11, &rx_task_);

    if (!SendPacket(pkt)) {
        running_ = false;
        return false;
    }

    {
        std::unique_lock<std::mutex> lk(connack_mutex_);
        if (!connack_cv_.wait_for(lk, std::chrono::seconds(10),
                                  [this]() { return connack_received_; })) {
            ESP_LOGE(TAG, "CONNACK timeout");
            return false;
        }
    }
    if (!connack_ok_) {
        ESP_LOGE(TAG, "CONNACK rejected");
        return false;
    }

    connected_ = true;
    if (on_connected_callback_) on_connected_callback_();

    xTaskCreate(PingTaskEntry, "c5mqtt_ping", 3072, this, 5, &ping_task_);
    ESP_LOGI(TAG, "MQTT connected to %s:%d", broker_address.c_str(), broker_port);
    return true;
}

void C5Mqtt::Disconnect() {
    if (connected_ && transport_) {
        std::vector<uint8_t> pkt = { MQTT_DISCONNECT, 0x00 };
        SendPacket(pkt);
    }
    connected_ = false;
    running_ = false;

    // 断开传输会唤醒阻塞在 Receive() 的接收线程
    if (transport_) {
        transport_->Disconnect();
    }
    // 给任务时间退出
    vTaskDelay(pdMS_TO_TICKS(50));
    rx_task_ = nullptr;
    ping_task_ = nullptr;
    if (transport_) {
        transport_.reset();
    }
}

bool C5Mqtt::Publish(const std::string topic, const std::string payload, int qos) {
    if (!connected_) return false;
    std::vector<uint8_t> vh;
    EncodeString(vh, topic);

    std::vector<uint8_t> pkt;
    pkt.push_back(MQTT_PUBLISH);   // QoS0
    std::vector<uint8_t> rest;
    rest.insert(rest.end(), vh.begin(), vh.end());
    rest.insert(rest.end(), payload.begin(), payload.end());
    EncodeRemainingLength(pkt, rest.size());
    pkt.insert(pkt.end(), rest.begin(), rest.end());
    return SendPacket(pkt);
}

bool C5Mqtt::Subscribe(const std::string topic, int qos) {
    if (!connected_) return false;
    std::vector<uint8_t> rest;
    uint16_t pid = ++packet_id_;
    rest.push_back((uint8_t)(pid >> 8));
    rest.push_back((uint8_t)(pid & 0xFF));
    EncodeString(rest, topic);
    rest.push_back((uint8_t)qos);

    std::vector<uint8_t> pkt;
    pkt.push_back(MQTT_SUBSCRIBE | 0x02);
    EncodeRemainingLength(pkt, rest.size());
    pkt.insert(pkt.end(), rest.begin(), rest.end());
    return SendPacket(pkt);
}

bool C5Mqtt::Unsubscribe(const std::string topic) {
    if (!connected_) return false;
    std::vector<uint8_t> rest;
    uint16_t pid = ++packet_id_;
    rest.push_back((uint8_t)(pid >> 8));
    rest.push_back((uint8_t)(pid & 0xFF));
    EncodeString(rest, topic);

    std::vector<uint8_t> pkt;
    pkt.push_back(MQTT_UNSUBSCRIBE | 0x02);
    EncodeRemainingLength(pkt, rest.size());
    pkt.insert(pkt.end(), rest.begin(), rest.end());
    return SendPacket(pkt);
}

void C5Mqtt::RxTaskEntry(void* arg) { static_cast<C5Mqtt*>(arg)->RxTask(); }
void C5Mqtt::PingTaskEntry(void* arg) { static_cast<C5Mqtt*>(arg)->PingTask(); }

void C5Mqtt::RxTask() {
    char buf[512];
    while (running_) {
        int n = transport_->Receive(buf, sizeof(buf));
        if (n <= 0) {
            // 连接关闭
            break;
        }
        rx_buffer_.append(buf, n);
        ProcessBuffer();
    }

    // 连接断开处理
    if (connected_) {
        connected_ = false;
        if (on_disconnected_callback_) on_disconnected_callback_();
    }
    vTaskDelete(nullptr);
}

void C5Mqtt::ProcessBuffer() {
    while (true) {
        if (rx_buffer_.size() < 2) return;

        uint32_t rem = 0, mult = 1;
        size_t pos = 1;
        bool complete = false;
        for (int i = 0; i < 4; i++) {
            if (pos >= rx_buffer_.size()) return;
            uint8_t b = (uint8_t)rx_buffer_[pos++];
            rem += (b & 0x7F) * mult;
            mult *= 128;
            if (!(b & 0x80)) { complete = true; break; }
        }
        if (!complete) return;

        size_t total = pos + rem;
        if (rx_buffer_.size() < total) return;

        uint8_t type = (uint8_t)rx_buffer_[0] & 0xF0;
        const char* body = rx_buffer_.data() + pos;

        if (type == MQTT_CONNACK) {
            uint8_t ret = (rem >= 2) ? (uint8_t)body[1] : 0xFF;
            {
                std::lock_guard<std::mutex> ck(connack_mutex_);
                connack_received_ = true;
                connack_ok_ = (ret == 0);
            }
            connack_cv_.notify_all();
        } else if (type == MQTT_PUBLISH) {
            uint8_t qos = ((uint8_t)rx_buffer_[0] >> 1) & 0x03;
            if (rem >= 2) {
                uint16_t tlen = ((uint8_t)body[0] << 8) | (uint8_t)body[1];
                if (2 + tlen <= rem) {
                    std::string topic(body + 2, tlen);
                    size_t off = 2 + tlen;
                    if (qos > 0) off += 2;
                    std::string payload;
                    if (off <= rem) payload.assign(body + off, rem - off);
                    if (on_message_callback_) on_message_callback_(topic, payload);
                }
            }
        }
        // 其它 (PINGRESP/SUBACK/UNSUBACK/PUBACK) 忽略

        rx_buffer_.erase(0, total);
    }
}

void C5Mqtt::PingTask() {
    int interval = keep_alive_seconds_ / 2;
    if (interval < 5) interval = 5;
    while (running_) {
        for (int i = 0; i < interval && running_; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!running_) break;
        if (connected_) {
            std::vector<uint8_t> pkt = { MQTT_PINGREQ, 0x00 };
            SendPacket(pkt);
        }
    }
    vTaskDelete(nullptr);
}
