/*
 * bridge_wifi.c - C5 WiFi STA 管理 (支持 5GHz 双频)
 */
#include "bridge_internal.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "bridge_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_sta_netif = NULL;
static volatile bool s_connected = false;
static int s_retry = 0;

static char s_ssid[33] = {0};
static char s_pwd[65]  = {0};

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        bridge_sock_close_all();       /* WiFi 掉线, 所有 socket 失效 */
        if (s_retry < MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "retry connect %d/%d", s_retry, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
        bridge_wifi_report_status();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "got ip");
        bridge_wifi_report_status();
    }
}

void bridge_wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_wifi_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* 允许 2.4G + 5G 双频扫描/连接 (C5 特性)。
     * 若当前 IDF 版本无此 API, 该调用可去掉, 默认即会使用支持的频段。 */
#ifdef WIFI_BAND_MODE_AUTO
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi init done (dual-band STA)");
}

void bridge_wifi_set_config(const char *ssid, const char *pwd)
{
    strncpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pwd, pwd ? pwd : "", sizeof(s_pwd) - 1);
    s_pwd[sizeof(s_pwd) - 1] = '\0';
    ESP_LOGI(TAG, "config ssid=%s", s_ssid);
}

void bridge_wifi_connect(void)
{
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_pwd, sizeof(wc.sta.password) - 1);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;   /* 全信道扫, 才能发现 5G AP */
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    s_retry = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to %s ...", s_ssid);
}

void bridge_wifi_disconnect(void)
{
    esp_wifi_disconnect();
    s_connected = false;
}

bool bridge_wifi_is_connected(void)
{
    return s_connected;
}

void bridge_wifi_report_status(void)
{
    bridge_wifi_status_t st = {0};
    st.connected = s_connected ? 1 : 0;

    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        st.rssi = ap.rssi;
        /* primary 信道 > 14 视为 5GHz */
        st.band = (ap.primary > 14) ? 2 : 1;
    } else {
        st.rssi = 0;
        st.band = 0;
    }

    if (s_connected && s_sta_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            uint32_t addr = ip.ip.addr;   /* 小端存放的网络字节序 */
            st.ip[0] = (uint8_t)(addr & 0xFF);
            st.ip[1] = (uint8_t)((addr >> 8) & 0xFF);
            st.ip[2] = (uint8_t)((addr >> 16) & 0xFF);
            st.ip[3] = (uint8_t)((addr >> 24) & 0xFF);
        }
    }
    bridge_send_frame(BRIDGE_EVT_WIFI_STATUS, BRIDGE_NO_LINK,
                      (const uint8_t *)&st, sizeof(st));
}
