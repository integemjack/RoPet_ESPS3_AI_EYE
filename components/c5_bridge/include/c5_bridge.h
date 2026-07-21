/*
 * c5_bridge.h - S3 侧 C5 WiFi 网桥驱动
 *
 * 负责:
 *   1. 通过 UART 与 C5 收发帧 (与 C5 的 bridge_uart.c 对应)
 *   2. 管理多条逻辑 socket (link), 每条 link 有独立的数据/关闭/打开结果回调
 *   3. 上报 C5 的 WiFi 状态
 *
 * 上层 (C5Transport/C5Udp/C5Mqtt/C5Http) 通过本类的 link API 收发数据。
 */
#ifndef C5_BRIDGE_H
#define C5_BRIDGE_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <mutex>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

#include "bridge_protocol.h"

// 每条逻辑连接 (socket) 的回调集合
struct C5Link {
    bool     in_use = false;
    // 打开结果 (SOCK_OPENED)
    std::function<void(bool ok, int err)> on_opened;
    // 收到数据 (SOCK_DATA)
    std::function<void(const std::string& data)> on_data;
    // 连接关闭 (SOCK_CLOSED)
    std::function<void()> on_closed;
};

class C5Bridge {
public:
    C5Bridge(gpio_num_t tx_pin, gpio_num_t rx_pin, size_t rx_buffer_size = 8192);
    ~C5Bridge();

    // 启动 UART + 接收任务, 等待 C5 就绪 (EVT_READY)
    void Start(int baud_rate = 921600);

    // 等待 C5 的 WiFi 连接就绪 (由 C5 自主完成配网/连接)
    // 返回 true 表示已连接; 超时返回 false
    bool WaitForNetworkReady(int timeout_ms = 120000);

    bool network_ready() const { return wifi_connected_; }
    int  GetRssi() const { return wifi_rssi_; }
    int  GetBand() const { return wifi_band_; }        // 1=2.4G 2=5G 0=unknown
    std::string GetIpAddress() const;

    // ---- link (socket) 管理, 供 C5Transport/C5Udp 使用 ----
    // 分配一条空闲 link, 返回 link_id; 失败返回 -1
    int  AllocLink();
    void FreeLink(int link_id);
    void SetLinkCallbacks(int link_id,
                          std::function<void(bool, int)> on_opened,
                          std::function<void(const std::string&)> on_data,
                          std::function<void()> on_closed);

    // 发送控制/数据帧
    bool SockOpen(int link_id, bridge_proto_t proto, const std::string& host, uint16_t port);
    bool SockSend(int link_id, const void* data, size_t len);
    void SockClose(int link_id);

    // WiFi 控制
    void RequestStatus();
    void RequestWifiStart();

    // 向 C5 请求配网页保存的 OTA 地址并等待返回。
    // 返回 true 且 out_url 有值表示 C5 上配置了地址; 空字符串表示未配置。
    // 超时返回 false。
    bool FetchOtaUrl(std::string& out_url, int timeout_ms = 5000);

    // 低层发帧 (线程安全)
    bool SendFrame(uint8_t type, uint8_t link_id, const uint8_t* payload, uint16_t len);

private:
    void RxTask();
    void DispatchFrame(uint8_t type, uint8_t link_id, const uint8_t* payload, uint16_t len);
    static void RxTaskEntry(void* arg);

    gpio_num_t   tx_pin_;
    gpio_num_t   rx_pin_;
    size_t       rx_buffer_size_;
    uart_port_t  uart_port_;

    SemaphoreHandle_t tx_lock_ = nullptr;
    EventGroupHandle_t events_ = nullptr;   // ready / wifi-connected 位
    TaskHandle_t rx_task_ = nullptr;

    C5Link links_[BRIDGE_MAX_LINKS];
    std::mutex links_mutex_;

    volatile bool c5_ready_ = false;
    volatile bool wifi_connected_ = false;
    volatile int  wifi_rssi_ = 0;
    volatile int  wifi_band_ = 0;
    uint8_t wifi_ip_[4] = {0,0,0,0};
    mutable std::mutex status_mutex_;

    // OTA 地址 (从 C5 配网页取回)
    std::string ota_url_;
    std::mutex  ota_url_mutex_;
};

#endif // C5_BRIDGE_H
