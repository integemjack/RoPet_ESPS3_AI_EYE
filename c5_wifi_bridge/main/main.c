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

    /* 配网/凭据/连接全部在 C5 自主完成 (复用 esp-wifi-connect),
     * S3 不再下发 WiFi 凭据。这里只保留一个"主动启动网络"命令备用。 */
    case BRIDGE_CMD_WIFI_CONNECT:
        bridge_wifi_start();
        break;

    case BRIDGE_CMD_GET_STATUS:
        bridge_wifi_report_status();
        break;

    case BRIDGE_CMD_GET_OTA_URL:
        bridge_wifi_report_ota_url();
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
    ESP_LOGI(TAG, "ready");

    /* C5 自主启动网络: 用 NVS 里存的凭据连; 没有则自动开热点配网 */
    ESP_LOGI(TAG, "calling bridge_wifi_start() ...");
    bridge_wifi_start();
    ESP_LOGI(TAG, "bridge_wifi_start() returned");

    /* 周期性上报状态 + 始终打印心跳(便于串口诊断) */
    uint32_t tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "heartbeat #%lu, wifi_connected=%d",
                 (unsigned long)(++tick), (int)bridge_wifi_is_connected());
        if (bridge_wifi_is_connected()) {
            bridge_wifi_report_status();
        }
    }
}
