/*
 * bridge_uart.c - UART 分帧收发 (C5 <-> S3)
 */
#include "bridge_internal.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "bridge_uart";

static SemaphoreHandle_t s_tx_lock;   /* 串行化发送, 防止多任务交错组帧 */

void bridge_uart_init(void)
{
    s_tx_lock = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate  = BRIDGE_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART_PORT, BRIDGE_UART_BUF_SIZE,
                                        BRIDGE_UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART_PORT, BRIDGE_UART_TX_PIN,
                                 BRIDGE_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d init: tx=%d rx=%d baud=%d",
             BRIDGE_UART_PORT, BRIDGE_UART_TX_PIN, BRIDGE_UART_RX_PIN, BRIDGE_UART_BAUD);
}

esp_err_t bridge_send_frame(uint8_t type, uint8_t link_id,
                            const uint8_t *payload, uint16_t len)
{
    if (len > BRIDGE_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* 头部 + payload 一起算 CRC (CRC 覆盖 type..payload) */
    uint8_t hdr[BRIDGE_HEADER_LEN];
    hdr[0] = BRIDGE_MAGIC0;
    hdr[1] = BRIDGE_MAGIC1;
    hdr[2] = type;
    hdr[3] = link_id;
    hdr[4] = (uint8_t)(len & 0xFF);
    hdr[5] = (uint8_t)(len >> 8);

    /* CRC 从 type 开始 (hdr+2), 再叠加 payload */
    uint16_t crc = 0xFFFF;
    {
        const uint8_t *p = &hdr[2];
        size_t n = BRIDGE_HEADER_LEN - 2;
        for (size_t i = 0; i < n; i++) {
            crc ^= (uint16_t)p[i] << 8;
            for (int b = 0; b < 8; b++)
                crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    if (payload && len) {
        for (size_t i = 0; i < len; i++) {
            crc ^= (uint16_t)payload[i] << 8;
            for (int b = 0; b < 8; b++)
                crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    uint8_t crc_le[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uart_write_bytes(BRIDGE_UART_PORT, (const char *)hdr, BRIDGE_HEADER_LEN);
    if (payload && len) {
        uart_write_bytes(BRIDGE_UART_PORT, (const char *)payload, len);
    }
    uart_write_bytes(BRIDGE_UART_PORT, (const char *)crc_le, BRIDGE_CRC_LEN);
    xSemaphoreGive(s_tx_lock);
    return ESP_OK;
}

void bridge_log(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        bridge_send_frame(BRIDGE_EVT_LOG, BRIDGE_NO_LINK,
                          (const uint8_t *)buf, (uint16_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
    }
}

/* ---- 接收状态机 ---- */
typedef enum {
    ST_MAGIC0, ST_MAGIC1, ST_TYPE, ST_LINK, ST_LEN0, ST_LEN1,
    ST_PAYLOAD, ST_CRC0, ST_CRC1
} rx_state_t;

static void rx_task(void *arg)
{
    static uint8_t payload[BRIDGE_MAX_PAYLOAD];
    rx_state_t st = ST_MAGIC0;
    uint8_t type = 0, link = 0;
    uint16_t len = 0, idx = 0;
    uint16_t rx_crc = 0;
    uint8_t byte;

    while (1) {
        int r = uart_read_bytes(BRIDGE_UART_PORT, &byte, 1, portMAX_DELAY);
        if (r != 1) continue;

        switch (st) {
        case ST_MAGIC0:
            st = (byte == BRIDGE_MAGIC0) ? ST_MAGIC1 : ST_MAGIC0;
            break;
        case ST_MAGIC1:
            st = (byte == BRIDGE_MAGIC1) ? ST_TYPE : ST_MAGIC0;
            break;
        case ST_TYPE:  type = byte; st = ST_LINK; break;
        case ST_LINK:  link = byte; st = ST_LEN0; break;
        case ST_LEN0:  len = byte; st = ST_LEN1; break;
        case ST_LEN1:
            len |= (uint16_t)byte << 8;
            if (len > BRIDGE_MAX_PAYLOAD) { st = ST_MAGIC0; break; } /* 非法长度, 重新同步 */
            idx = 0;
            st = (len == 0) ? ST_CRC0 : ST_PAYLOAD;
            break;
        case ST_PAYLOAD:
            payload[idx++] = byte;
            if (idx >= len) st = ST_CRC0;
            break;
        case ST_CRC0: rx_crc = byte; st = ST_CRC1; break;
        case ST_CRC1: {
            rx_crc |= (uint16_t)byte << 8;
            /* 校验: 用共享的 CRC 覆盖 type..payload。为省内存分两段算 */
            uint16_t crc = 0xFFFF;
            uint8_t meta[4] = { type, link, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
            for (int i = 0; i < 4; i++) {
                crc ^= (uint16_t)meta[i] << 8;
                for (int b = 0; b < 8; b++)
                    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
            }
            for (int i = 0; i < len; i++) {
                crc ^= (uint16_t)payload[i] << 8;
                for (int b = 0; b < 8; b++)
                    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
            }
            if (crc == rx_crc) {
                bridge_dispatch_frame(type, link, payload, len);
            } else {
                ESP_LOGW(TAG, "CRC mismatch type=0x%02x len=%u", type, len);
            }
            st = ST_MAGIC0;
            break;
        }
        }
    }
}

/* main 里调用一次以启动接收任务 */
void bridge_uart_start_rx_task(void)
{
    xTaskCreate(rx_task, "bridge_rx", 4096, NULL, 12, NULL);
}
