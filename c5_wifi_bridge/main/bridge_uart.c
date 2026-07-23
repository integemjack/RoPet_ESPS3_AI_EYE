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

    bool flow_ctrl = (BRIDGE_UART_RTS_PIN >= 0 && BRIDGE_UART_CTS_PIN >= 0);
    uart_config_t cfg = {
        .baud_rate  = BRIDGE_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = flow_ctrl ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE,
        /* 剩 <= rx_flow_ctrl_thresh 字节空间时拉高 RTS 让对端暂停发送 */
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART_PORT, BRIDGE_UART_BUF_SIZE,
                                        BRIDGE_UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART_PORT, BRIDGE_UART_TX_PIN,
                                 BRIDGE_UART_RX_PIN,
                                 flow_ctrl ? BRIDGE_UART_RTS_PIN : UART_PIN_NO_CHANGE,
                                 flow_ctrl ? BRIDGE_UART_CTS_PIN : UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d init: tx=%d rx=%d baud=%d flow_ctrl=%d",
             BRIDGE_UART_PORT, BRIDGE_UART_TX_PIN, BRIDGE_UART_RX_PIN,
             BRIDGE_UART_BAUD, flow_ctrl ? 1 : 0);
}

/* 一次组装整帧再单次写出, 减少 uart_write_bytes 调用次数 (原来 3 次 -> 1 次)。
 * 用一个受 tx_lock 保护的静态缓冲, 避免大帧压栈。 */
static uint8_t s_tx_frame[BRIDGE_HEADER_LEN + BRIDGE_MAX_PAYLOAD + BRIDGE_CRC_LEN];

esp_err_t bridge_send_frame(uint8_t type, uint8_t link_id,
                            const uint8_t *payload, uint16_t len)
{
    if (len > BRIDGE_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);

    uint8_t *f = s_tx_frame;
    f[0] = BRIDGE_MAGIC0;
    f[1] = BRIDGE_MAGIC1;
    f[2] = type;
    f[3] = link_id;
    f[4] = (uint8_t)(len & 0xFF);
    f[5] = (uint8_t)(len >> 8);
    if (payload && len) {
        memcpy(&f[BRIDGE_HEADER_LEN], payload, len);
    }

    /* CRC 覆盖 type..payload, 查表法 */
    uint16_t crc = bridge_crc16_update(0xFFFF, &f[2], BRIDGE_HEADER_LEN - 2);
    if (payload && len) {
        crc = bridge_crc16_update(crc, &f[BRIDGE_HEADER_LEN], len);
    }
    f[BRIDGE_HEADER_LEN + len]     = (uint8_t)(crc & 0xFF);
    f[BRIDGE_HEADER_LEN + len + 1] = (uint8_t)(crc >> 8);

    int total = BRIDGE_HEADER_LEN + len + BRIDGE_CRC_LEN;
    uart_write_bytes(BRIDGE_UART_PORT, (const char *)f, total);

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

#define RX_READ_CHUNK  512   /* 每次尝试从 UART 批量读取的字节数 */

static void rx_task(void *arg)
{
    static uint8_t payload[BRIDGE_MAX_PAYLOAD];
    static uint8_t rxbuf[RX_READ_CHUNK];
    rx_state_t st = ST_MAGIC0;
    uint8_t type = 0, link = 0;
    uint16_t len = 0, idx = 0;
    uint16_t rx_crc = 0;

    while (1) {
        /* 批量读取: 先阻塞等 1 字节 (避免凑不满整块而永久阻塞),
         * 再用 0 超时把 ring buffer 里已到的其余字节一次取走。 */
        int got = uart_read_bytes(BRIDGE_UART_PORT, rxbuf, 1, portMAX_DELAY);
        if (got <= 0) continue;
        int more = uart_read_bytes(BRIDGE_UART_PORT, rxbuf + 1, RX_READ_CHUNK - 1, 0);
        if (more > 0) got += more;

        for (int i = 0; i < got; i++) {
            uint8_t byte = rxbuf[i];
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
            case ST_PAYLOAD: {
                /* 批量拷贝当前 buffer 中属于本 payload 的连续字节 */
                uint16_t need = len - idx;
                int avail = got - i;
                int n = (avail < need) ? avail : need;
                memcpy(&payload[idx], &rxbuf[i], n);
                idx += n;
                i += n - 1;   /* for 循环还会 +1 */
                if (idx >= len) st = ST_CRC0;
                break;
            }
            case ST_CRC0: rx_crc = byte; st = ST_CRC1; break;
            case ST_CRC1: {
                rx_crc |= (uint16_t)byte << 8;
                /* 校验: CRC 覆盖 type..payload, 查表法分两段算 */
                uint8_t meta[4] = { type, link, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
                uint16_t crc = bridge_crc16_update(0xFFFF, meta, 4);
                crc = bridge_crc16_update(crc, payload, len);
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
}

/* main 里调用一次以启动接收任务 */
void bridge_uart_start_rx_task(void)
{
    xTaskCreate(rx_task, "bridge_rx", 4096, NULL, 12, NULL);
}
