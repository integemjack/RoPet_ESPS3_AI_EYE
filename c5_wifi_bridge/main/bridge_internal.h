/*
 * bridge_internal.h - C5 固件内部共享声明
 */
#ifndef BRIDGE_INTERNAL_H
#define BRIDGE_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <esp_err.h>
#include "bridge_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- UART 引脚 / 参数 (对接 S3 侧原 ML307 的 TX/RX) ----
 * 注意: 交叉连线!  C5.TX -> S3.RX,  C5.RX -> S3.TX
 * 下面是 C5 自己的引脚, 按实际硬件修改。
 */
// 注意: ESP32-C5 的 GPIO13=USB D-, GPIO14=USB D+, 绝对不能用作 UART,
//       否则一启动就会掐断 USB(烧录后 esptool/monitor 全部失联)。
//       flash 相关引脚(通常 GPIO10~17 视封装)也应避开。
//       这里用 GPIO4(TX)/GPIO3(RX), 与 USB/flash 均不冲突。
#define BRIDGE_UART_PORT      UART_NUM_1
#define BRIDGE_UART_TX_PIN    9
#define BRIDGE_UART_RX_PIN    10
/* 硬件流控 (可选): 设为 -1 (UART_PIN_NO_CHANGE) 表示禁用。
 * 高波特率下强烈建议接上 RTS/CTS 以杜绝 ring buffer 溢出丢帧。
 * 接线: C5.RTS -> S3.CTS,  C5.CTS -> S3.RTS。两端都要启用才生效。 */
#define BRIDGE_UART_RTS_PIN   (-1)
#define BRIDGE_UART_CTS_PIN   (-1)
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

/* ---- WiFi 层 (复用 78/esp-wifi-connect, 配网/存储/连接全在 C5) ---- */
void bridge_wifi_init(void);   /* 初始化 nvs/netif/wifi 驱动 */
void bridge_wifi_start(void);  /* 用已存凭据连接; 失败则开热点配网 */
/* 填充并主动上报一次 WiFi 状态帧 */
void bridge_wifi_report_status(void);
bool bridge_wifi_is_connected(void);
/* 是否处于"等待 S3 配网"状态 (无可用凭据)。 */
bool bridge_wifi_is_provisioning(void);
/* 在配网状态下重发一次 NEED_PROVISION。
 * 必须周期性重发: 两端各自独立重启, C5 通常比 S3 先起来, 开机那一发很可能
 * 打在 S3 的 UART 还没初始化的窗口里, 一次性事件会直接丢失。 */
void bridge_wifi_announce_provision(void);
/* 读取配网页保存的 OTA 地址 (C5 NVS wifi.ota_url), 通过 EVT_OTA_URL 回传 S3 */
void bridge_wifi_report_ota_url(void);
/* 清除 NVS 里保存的全部 WiFi 凭据 (保留 ota_url)。
 * 返回后调用方负责重启: 无凭据启动会进入配网模式并通知 S3 开热点。 */
void bridge_wifi_reset_credentials(void);
/* 扫描 5G 频段, 结果通过 EVT_WIFI_SCAN_RESULT 回传 S3 (S3 只有 2.4G 射频)。 */
void bridge_wifi_scan_and_report(void);
/* 用 S3 下发的凭据做一次真实连接验证, 成功才写 NVS;
 * 结果通过 EVT_WIFI_CONFIG_RESULT 回传。无论成败都不在此处重启。 */
void bridge_wifi_try_config(const char* ssid, const char* password);

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
