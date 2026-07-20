/*
 * bridge_internal.h - C5 固件内部共享声明
 */
#ifndef BRIDGE_INTERNAL_H
#define BRIDGE_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "bridge_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- UART 引脚 / 参数 (对接 S3 侧原 ML307 的 TX/RX) ----
 * 注意: 交叉连线!  C5.TX -> S3.RX,  C5.RX -> S3.TX
 * 下面是 C5 自己的引脚, 按实际硬件修改。
 */
#define BRIDGE_UART_PORT      UART_NUM_1
#define BRIDGE_UART_TX_PIN    5
#define BRIDGE_UART_RX_PIN    4
#define BRIDGE_UART_BAUD      921600
#define BRIDGE_UART_BUF_SIZE  8192

/* ---- UART 层 ---- */
void bridge_uart_init(void);
void bridge_uart_start_rx_task(void);
/* 组帧并发送 (线程安全, 内部加锁)。payload 可为 NULL(len=0)。 */
esp_err_t bridge_send_frame(uint8_t type, uint8_t link_id,
                            const uint8_t *payload, uint16_t len);
/* 便捷: 发送一行日志到 S3 (会被 S3 打到它的日志里) */
void bridge_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* 收到一整帧后的分发入口 (由 uart 任务调用) */
void bridge_dispatch_frame(uint8_t type, uint8_t link_id,
                           const uint8_t *payload, uint16_t len);

/* ---- WiFi 层 ---- */
void bridge_wifi_init(void);
void bridge_wifi_set_config(const char *ssid, const char *pwd);
void bridge_wifi_connect(void);
void bridge_wifi_disconnect(void);
/* 填充并主动上报一次 WiFi 状态帧 */
void bridge_wifi_report_status(void);
bool bridge_wifi_is_connected(void);

/* ---- Socket 层 ---- */
void bridge_sock_init(void);
void bridge_sock_open(uint8_t link_id, bridge_proto_t proto,
                      const char *host, uint16_t port);
void bridge_sock_send(uint8_t link_id, const uint8_t *data, uint16_t len);
void bridge_sock_close(uint8_t link_id);
/* WiFi 断开时, 关闭所有 socket */
void bridge_sock_close_all(void);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_INTERNAL_H */
