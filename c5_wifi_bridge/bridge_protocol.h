/*
 * bridge_protocol.h
 *
 * S3(主控) <-> C5(WiFi 网桥) 之间的 UART 帧协议定义。
 * 本文件被 C5 固件和 S3 端共同引用，请保持两端一致。
 *
 * 帧格式 (小端):
 *   +--------+--------+------+---------+--------+-------------+-------+
 *   | 0xAA   | 0x55   | type | link_id | len(2) | payload(len)| crc(2)|
 *   +--------+--------+------+---------+--------+-------------+-------+
 *   magic = 0xAA 0x55
 *   type    : 1 字节, 见 bridge_msg_type_t
 *   link_id : 1 字节, socket 编号 (0..BRIDGE_MAX_LINKS-1); 非 socket 类消息填 0xFF
 *   len     : 2 字节小端, payload 长度
 *   payload : len 字节
 *   crc16   : 2 字节小端, CRC16-CCITT (0xFFFF init), 覆盖 type..payload
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
#define BRIDGE_MAX_LINKS     6       /* 最多同时打开的 socket 数 */
#define BRIDGE_NO_LINK       0xFF    /* 非 socket 类消息的 link_id 占位 */
#define BRIDGE_MAX_PAYLOAD   4096    /* 单帧最大 payload */
#define BRIDGE_HEADER_LEN    6       /* magic(2)+type(1)+link(1)+len(2) */
#define BRIDGE_CRC_LEN       2

/* 消息类型。0x0x = 控制/WiFi, 0x1x = socket, 0xEx = 事件/日志 */
typedef enum {
    /* ---- S3 -> C5 : 控制 ---- */
    BRIDGE_CMD_PING          = 0x01, /* payload: 空; C5 回 EVT_PONG */
    BRIDGE_CMD_RESET         = 0x02, /* 复位 C5 (重启) */
    BRIDGE_CMD_WIFI_CONFIG   = 0x03, /* [已废弃] 配网改由 C5 自主完成(SoftAP+网页), S3 不下发凭据 */
    BRIDGE_CMD_WIFI_CONNECT  = 0x04, /* 让 C5 (重新)启动网络: 用已存凭据连, 无则自动开热点配网 */
    BRIDGE_CMD_WIFI_DISCONNECT = 0x05, /* [保留] */
    BRIDGE_CMD_GET_STATUS    = 0x06, /* 请求上报状态; C5 回 EVT_WIFI_STATUS */
    BRIDGE_CMD_GET_OTA_URL   = 0x07, /* 请求 C5 配网页保存的 OTA 地址; C5 回 EVT_OTA_URL */

    /* ---- S3 -> C5 : socket ---- */
    BRIDGE_CMD_SOCK_OPEN     = 0x10, /* payload: bridge_sock_open_t 头 + host 字符串 */
    BRIDGE_CMD_SOCK_SEND     = 0x11, /* payload: 原始数据 */
    BRIDGE_CMD_SOCK_CLOSE    = 0x12, /* 关闭 link */
    BRIDGE_CMD_UDP_SENDTO    = 0x13, /* payload: bridge_udp_hdr_t + data (面向无连接时使用) */

    /* ---- C5 -> S3 : 事件 ---- */
    BRIDGE_EVT_READY         = 0x80, /* C5 启动完成 */
    BRIDGE_EVT_PONG          = 0x81,
    BRIDGE_EVT_WIFI_STATUS   = 0x82, /* payload: bridge_wifi_status_t */
    BRIDGE_EVT_OTA_URL       = 0x83, /* payload: OTA 地址字符串 (可空, 空表示未配置) */
    BRIDGE_EVT_SOCK_OPENED   = 0x90, /* payload: bridge_sock_result_t (link_id 在帧头) */
    BRIDGE_EVT_SOCK_DATA     = 0x91, /* payload: 原始数据 */
    BRIDGE_EVT_SOCK_CLOSED   = 0x92, /* 远端关闭或出错 */
    BRIDGE_EVT_LOG           = 0xE0, /* payload: ASCII 调试文本 */
} bridge_msg_type_t;

/* socket 协议类型 */
typedef enum {
    BRIDGE_PROTO_TCP = 0,
    BRIDGE_PROTO_TLS = 1,
    BRIDGE_PROTO_UDP = 2,
} bridge_proto_t;

/* CMD_SOCK_OPEN payload 头, 其后紧跟 host 字符串 (host_len 字节, 不含 '\0') */
typedef struct __attribute__((packed)) {
    uint8_t  proto;      /* bridge_proto_t */
    uint16_t port;       /* 小端 */
    uint8_t  host_len;   /* host 字符串长度 */
    /* char host[host_len]; */
} bridge_sock_open_t;

/* EVT_SOCK_OPENED payload */
typedef struct __attribute__((packed)) {
    uint8_t  ok;         /* 1 成功, 0 失败 */
    int32_t  err;        /* 失败时的错误码 (小端) */
} bridge_sock_result_t;

/* EVT_WIFI_STATUS payload */
typedef struct __attribute__((packed)) {
    uint8_t  connected;  /* 1 已连接 */
    int8_t   rssi;       /* dBm */
    uint8_t  band;       /* 1 = 2.4G, 2 = 5G, 0 = 未知 */
    uint8_t  ip[4];      /* IPv4, 网络序 (ip[0].ip[1].ip[2].ip[3]) */
} bridge_wifi_status_t;

/* CRC16-CCITT (poly 0x1021, init 0xFFFF) */
static inline uint16_t bridge_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_PROTOCOL_H */
