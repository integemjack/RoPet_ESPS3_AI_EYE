/*
 * bridge_wifi.cc - C5 WiFi 管理
 *
 * 直接复用小智同款组件 78/esp-wifi-connect (与主工程 S3 同版本 2.4.3):
 *   - WifiStation        : 用 NVS 里存的凭据连 WiFi (全信道扫描, 优先信号最强 -> 可连 5G)
 *   - WifiConfigurationAp: 连不上时开 SoftAP + captive portal 配网网页 (与小智一模一样)
 *   - SsidManager        : 凭据持久化到 C5 的 NVS
 *
 * 也就是把小智的配网网页"整套搬到 C5 上"。S3 不再参与 WiFi。
 *
 * 本文件的调用严格对齐主工程 main/boards/common/wifi_board.cc 中已验证可用的 API。
 */
#include "bridge_internal.h"   /* 头文件自带 extern "C" 守卫 */

#include <string>
#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_station.h"
#include "wifi_configuration_ap.h"
#include "ssid_manager.h"
#include "nvs.h"

static const char *TAG = "bridge_wifi";

static bool s_config_mode = false;

extern "C" void bridge_wifi_init(void)
{
    /* 严格对齐主工程 S3 的启动模式 (main.cc): 只做 event loop + NVS。
     * esp_netif_init / esp_wifi_init / netif 创建 均由 esp-wifi-connect 组件
     * 内部完成 (S3 的 main.cc 同样没有显式调用), 这里不要重复初始化,
     * 否则会触发 ESP_ERR_INVALID_STATE。 */
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "nvs + event loop ready (wifi driver managed by component)");
}

/* 进入配网模式: 开热点 + 网页, 等价于小智 WifiBoard::EnterWifiConfigMode */
static void enter_config_mode()
{
    s_config_mode = true;

    auto &wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage("zh-CN");
    wifi_ap.SetSsidPrefix("Xiaozhi");
    // 注意: esp-wifi-connect 2.4.3 的配网页默认就显示 OTA 地址输入框,
    // 无需额外开关。用户填写后会存入 C5 NVS 的 wifi.ota_url。
    wifi_ap.Start();

    bridge_log("C5 WiFi config mode. hotspot: %s url: %s",
               wifi_ap.GetSsid().c_str(), wifi_ap.GetWebServerUrl().c_str());
    ESP_LOGI(TAG, "config AP: %s  %s",
             wifi_ap.GetSsid().c_str(), wifi_ap.GetWebServerUrl().c_str());

    /* 上报一次状态: 未连接, 让 S3 屏幕显示"配网中"。
     * 配网完成后组件内部会重启设备, 无需在此轮询。 */
    bridge_wifi_report_status();
}

extern "C" void bridge_wifi_start(void)
{
    /* 没有任何已保存的 SSID -> 直接进配网 */
    auto &ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        ESP_LOGW(TAG, "no saved ssid, enter config mode");
        enter_config_mode();
        return;
    }

    auto &wifi_station = WifiStation::GetInstance();
    wifi_station.OnConnected([](const std::string &ssid) {
        ESP_LOGI(TAG, "connected to %s", ssid.c_str());
        bridge_wifi_report_status();
    });
    wifi_station.Start();

    /* 等待连接; 失败则进配网 (逻辑同 WifiBoard::StartNetwork) */
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        ESP_LOGW(TAG, "connect timeout, enter config mode");
        enter_config_mode();
        return;
    }

    ESP_LOGI(TAG, "wifi connected");
    bridge_wifi_report_status();
}

extern "C" bool bridge_wifi_is_connected(void)
{
    if (s_config_mode) return false;
    return WifiStation::GetInstance().IsConnected();
}

extern "C" void bridge_wifi_report_ota_url(void)
{
    /* 从 C5 的 NVS wifi.ota_url 读取配网页保存的 OTA 地址, 回传 S3。
     * esp-wifi-connect 的配网页 /advanced/submit 会写入这个 key。 */
    char ota_url[256] = {0};
    size_t len = sizeof(ota_url);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "ota_url", ota_url, &len);
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ota_url[0] = '\0';   /* 未配置: 回传空字符串 */
        len = 0;
    } else {
        len = strlen(ota_url);
    }
    ESP_LOGI(TAG, "report ota_url: '%s'", ota_url);
    bridge_send_frame(BRIDGE_EVT_OTA_URL, BRIDGE_NO_LINK,
                      (const uint8_t *)ota_url, (uint16_t)len);
}

extern "C" void bridge_wifi_report_status(void)
{
    bridge_wifi_status_t st;
    memset(&st, 0, sizeof(st));

    if (s_config_mode) {
        /* 配网中: connected=0, band=0 */
        bridge_send_frame(BRIDGE_EVT_WIFI_STATUS, BRIDGE_NO_LINK,
                          (const uint8_t *)&st, sizeof(st));
        return;
    }

    auto &wifi_station = WifiStation::GetInstance();
    bool connected = wifi_station.IsConnected();
    st.connected = connected ? 1 : 0;

    if (connected) {
        st.rssi = wifi_station.GetRssi();
        uint8_t ch = wifi_station.GetChannel();
        st.band = (ch > 14) ? 2 : 1;   /* 信道 > 14 判定为 5GHz */

        /* 解析 IP 字符串 (点分十进制) */
        std::string ip = wifi_station.GetIpAddress();
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            st.ip[0] = (uint8_t)a; st.ip[1] = (uint8_t)b;
            st.ip[2] = (uint8_t)c; st.ip[3] = (uint8_t)d;
        }
    }

    bridge_send_frame(BRIDGE_EVT_WIFI_STATUS, BRIDGE_NO_LINK,
                      (const uint8_t *)&st, sizeof(st));
}
