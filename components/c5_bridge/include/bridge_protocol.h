/*
 * bridge_protocol.h
 *
 * S3(主控) <-> C5(WiFi 网桥) 之间的 UART 帧协议定义。
 * 本文件被 C5 固件和 S3 端共同引用，两端必须保持一致。
 * (与 c5_wifi_bridge/bridge_protocol.h 内容完全相同)
 *
 * 帧格式 (小端):
 *   +--------+--------+------+---------+--------+-------------+-------+
 *   | 0xAA   | 0x55   | type | link_id | len(2) | payload(len)| crc(2)|
 *   +--------+--------+------+---------+--------+-------------+-------+
 *   crc16 : CRC16-CCITT (0xFFFF init), 覆盖 type..payload
 */
#ifndef BRIDGE_PROTOCOL_H
#define BRIDGE_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_MAGIC0        0xAA
#define BRIDGE_MAGIC1        0x55
#define BRIDGE_MAX_LINKS     6
#define BRIDGE_NO_LINK       0xFF
#define BRIDGE_MAX_PAYLOAD   4096
#define BRIDGE_HEADER_LEN    6
#define BRIDGE_CRC_LEN       2

typedef enum {
    BRIDGE_CMD_PING          = 0x01,
    BRIDGE_CMD_RESET         = 0x02,
    BRIDGE_CMD_WIFI_CONFIG   = 0x03, /* [已废弃] 配网由 C5 自主完成 */
    BRIDGE_CMD_WIFI_CONNECT  = 0x04,
    BRIDGE_CMD_WIFI_DISCONNECT = 0x05,
    BRIDGE_CMD_GET_STATUS    = 0x06,
    BRIDGE_CMD_GET_OTA_URL   = 0x07, /* 请求 C5 配网页保存的 OTA 地址; C5 回 EVT_OTA_URL */

    BRIDGE_CMD_SOCK_OPEN     = 0x10,
    BRIDGE_CMD_SOCK_SEND     = 0x11,
    BRIDGE_CMD_SOCK_CLOSE    = 0x12,
    BRIDGE_CMD_UDP_SENDTO    = 0x13,

    BRIDGE_EVT_READY         = 0x80,
    BRIDGE_EVT_PONG          = 0x81,
    BRIDGE_EVT_WIFI_STATUS   = 0x82,
    BRIDGE_EVT_OTA_URL       = 0x83, /* payload: OTA 地址字符串 (可空) */
    BRIDGE_EVT_SOCK_OPENED   = 0x90,
    BRIDGE_EVT_SOCK_DATA     = 0x91,
    BRIDGE_EVT_SOCK_CLOSED   = 0x92,
    BRIDGE_EVT_LOG           = 0xE0,
} bridge_msg_type_t;

typedef enum {
    BRIDGE_PROTO_TCP = 0,
    BRIDGE_PROTO_TLS = 1,
    BRIDGE_PROTO_UDP = 2,
} bridge_proto_t;

typedef struct __attribute__((packed)) {
    uint8_t  proto;
    uint16_t port;
    uint8_t  host_len;
    /* char host[host_len]; */
} bridge_sock_open_t;

typedef struct __attribute__((packed)) {
    uint8_t  ok;
    int32_t  err;
} bridge_sock_result_t;

typedef struct __attribute__((packed)) {
    uint8_t  connected;
    int8_t   rssi;
    uint8_t  band;      /* 1 = 2.4G, 2 = 5G, 0 = 未知 */
    uint8_t  ip[4];
} bridge_wifi_status_t;

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_PROTOCOL_H */
