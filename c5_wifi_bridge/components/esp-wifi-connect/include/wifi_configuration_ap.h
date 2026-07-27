#ifndef _WIFI_CONFIGURATION_AP_H_
#define _WIFI_CONFIGURATION_AP_H_

#include <string>
#include <vector>
#include <mutex>

#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>

#include "dns_server.h"

class WifiConfigurationAp {
public:
    static WifiConfigurationAp& GetInstance();
    void SetSsidPrefix(const std::string &&ssid_prefix);
    void SetLanguage(const std::string &&language);
    void Start();
    void Stop();
    void StartSmartConfig();
    bool ConnectToWifi(const std::string &ssid, const std::string &password);
    void Save(const std::string &ssid, const std::string &password);

    std::string GetSsid();
    std::string GetWebServerUrl();

    // Delete copy constructor and assignment operator
    WifiConfigurationAp(const WifiConfigurationAp&) = delete;
    WifiConfigurationAp& operator=(const WifiConfigurationAp&) = delete;

private:
    // Private constructor
    WifiConfigurationAp();
    ~WifiConfigurationAp();

    // [本地修改] 异步连接状态机。
    // C5 是单射频: STA 关联 5G AP 时 SoftAP 会被强制跟随到 5G 信道 (日志里的
    // "ap channel adjust o:1,1 n:128,2"), 手机 (在 2.4G ch1 上) 立刻掉线。
    // 如果 /submit 像上游那样同步阻塞 15s 等连接结果, 这个 HTTP 请求必然在
    // 途中被掐断 -> 浏览器报 "Failed to fetch", 尽管设备其实连上了。
    // 因此改为: /submit 立即返回, 连接在后台任务里做, 网页轮询 /submit/status。
    enum class ConnectState {
        kIdle,
        kConnecting,
        kSuccess,
        kFailed,
    };

    std::mutex mutex_;
    std::mutex connect_mutex_;
    ConnectState connect_state_ = ConnectState::kIdle;
    std::string connect_error_;
    std::string pending_ssid_;
    std::string pending_password_;
    DnsServer dns_server_;
    httpd_handle_t server_ = NULL;
    EventGroupHandle_t event_group_;
    std::string ssid_prefix_;
    std::string language_;
    esp_event_handler_instance_t instance_any_id_;
    esp_event_handler_instance_t instance_got_ip_;
    esp_timer_handle_t scan_timer_ = nullptr;
    bool is_connecting_ = false;
    esp_netif_t* ap_netif_ = nullptr;
    std::vector<wifi_ap_record_t> ap_records_;
    // [本地修改] 记录最后一次断连原因, 供 ConnectToWifi() 判断是"密码错误"
    // 还是"瞬时失败可继续重试"(双频同名 SSID 场景), 并用于日志诊断。
    uint8_t last_disconnect_reason_ = 0;

    // 高级配置项
    std::string ota_url_;
    int8_t max_tx_power_;
    bool remember_bssid_;

    void StartAccessPoint();
    void StartWebServer();
    // [本地修改] 提交连接请求 (非阻塞), 真正的连接在后台任务里执行。
    bool StartConnectAsync(const std::string &ssid, const std::string &password);
    static void ConnectTask(void* arg);
    // [本地修改] 只扫描 5GHz 信道 (C5 双频, 本产品只使用 5G)。
    void StartScan();

    // Event handlers
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void SmartConfigEventHandler(void* arg, esp_event_base_t event_base, 
                                      int32_t event_id, void* event_data);
    esp_event_handler_instance_t sc_event_instance_ = nullptr;
};

#endif // _WIFI_CONFIGURATION_AP_H_
