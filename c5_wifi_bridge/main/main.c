/*
 * main.c - C5 WiFi Bridge 入口 & 帧分发
 *
 * 角色: 把自己变成一个"UART WiFi 调制解调器"。S3 主控通过 UART 发命令,
 *      C5 用自身 5GHz WiFi 建立 TCP/TLS/UDP 连接并透传数据。
 */
#include "bridge_internal.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "c5_bridge";

void bridge_dispatch_frame(uint8_t type, uint8_t link_id,
                           const uint8_t *payload, uint16_t len)
{
    switch (type) {
    case BRIDGE_CMD_PING:
        bridge_send_frame(BRIDGE_EVT_PONG, BRIDGE_NO_LINK, NULL, 0);
        break;

    case BRIDGE_CMD_RESET:
        ESP_LOGW(TAG, "reset requested");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
        break;

    case BRIDGE_CMD_WIFI_CONFIG: {
        /* payload: [ssid_len(1)][ssid][pwd...] */
        if (len < 1) break;
        uint8_t ssid_len = payload[0];
        if (1 + ssid_len > len) break;
        char ssid[33] = {0};
        char pwd[65] = {0};
        uint8_t sl = ssid_len < 32 ? ssid_len : 32;
        memcpy(ssid, &payload[1], sl);
        uint16_t pwd_len = len - 1 - ssid_len;
        uint16_t pl = pwd_len < 64 ? pwd_len : 64;
        if (pwd_len > 0) memcpy(pwd, &payload[1 + ssid_len], pl);
        bridge_wifi_set_config(ssid, pwd);
        break;
    }

    case BRIDGE_CMD_WIFI_CONNECT:
        bridge_wifi_connect();
        break;

    case BRIDGE_CMD_WIFI_DISCONNECT:
        bridge_wifi_disconnect();
        break;

    case BRIDGE_CMD_GET_STATUS:
        bridge_wifi_report_status();
        break;

    case BRIDGE_CMD_SOCK_OPEN: {
        /* payload: bridge_sock_open_t + host[host_len] */
        if (len < sizeof(bridge_sock_open_t)) break;
        const bridge_sock_open_t *o = (const bridge_sock_open_t *)payload;
        uint8_t host_len = o->host_len;
        if (sizeof(bridge_sock_open_t) + host_len > len) break;
        char host[128] = {0};
        uint8_t hl = host_len < 127 ? host_len : 127;
        memcpy(host, payload + sizeof(bridge_sock_open_t), hl);
        bridge_sock_open(link_id, (bridge_proto_t)o->proto, host, o->port);
        break;
    }

    case BRIDGE_CMD_SOCK_SEND:
        bridge_sock_send(link_id, payload, len);
        break;

    case BRIDGE_CMD_SOCK_CLOSE:
        bridge_sock_close(link_id);
        break;

    default:
        ESP_LOGW(TAG, "unknown frame type 0x%02x", type);
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "C5 WiFi Bridge starting...");

    bridge_uart_init();
    bridge_sock_init();
    bridge_wifi_init();

    bridge_uart_start_rx_task();

    /* 告诉 S3: C5 已就绪 (对应 ML307 的 OnMaterialReady) */
    bridge_send_frame(BRIDGE_EVT_READY, BRIDGE_NO_LINK, NULL, 0);
    ESP_LOGI(TAG, "ready, waiting for host commands");

    /* 周期性上报状态, 让 S3 侧信号图标能刷新 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (bridge_wifi_is_connected()) {
            bridge_wifi_report_status();
        }
    }
}
