/*
 * c5_mqtt.h - 在 C5 网桥的 TCP/TLS link 之上实现精简 MQTT 3.1.1 客户端
 *
 * 实现 esp-ml307 3.x 的 Mqtt 抽象, 支持 xiaozhi 所需的:
 *   CONNECT / PUBLISH(QoS0) / SUBSCRIBE / UNSUBSCRIBE / PINGREQ / DISCONNECT
 *   解析 CONNACK / PUBLISH(incoming) / SUBACK / PINGRESP
 * 端口 8883 走 TLS, 其余走明文 TCP。
 *
 * 底层传输用 C5Tcp (异步 OnStream 回调), 不再自起接收线程; 仅保留一个
 * 轻量 ping 线程维持 keep-alive。
 */
#ifndef C5_MQTT_H
#define C5_MQTT_H

#include <mqtt.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "c5_bridge.h"
#include "c5_tcp.h"

class C5Mqtt : public Mqtt {
public:
    explicit C5Mqtt(C5Bridge& bridge);
    ~C5Mqtt() override;

    bool Connect(const std::string broker_address, int broker_port,
                 const std::string client_id, const std::string username,
                 const std::string password) override;
    void Disconnect() override;
    bool Publish(const std::string topic, const std::string payload, int qos = 0) override;
    bool Subscribe(const std::string topic, int qos = 0) override;
    bool Unsubscribe(const std::string topic) override;
    bool IsConnected() override { return connected_; }

private:
    void ProcessBuffer();
    bool SendPacket(const std::vector<uint8_t>& pkt);
    static void EncodeRemainingLength(std::vector<uint8_t>& out, uint32_t len);
    static void EncodeString(std::vector<uint8_t>& out, const std::string& s);
    void PingTask();
    static void PingTaskEntry(void* arg);

    C5Bridge& bridge_;
    std::unique_ptr<C5Tcp> tcp_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    uint16_t packet_id_ = 0;

    std::string rx_buffer_;
    std::mutex rx_mutex_;
    std::mutex send_mutex_;

    std::mutex connack_mutex_;
    std::condition_variable connack_cv_;
    bool connack_received_ = false;
    bool connack_ok_ = false;

    TaskHandle_t ping_task_ = nullptr;
};

#endif // C5_MQTT_H
