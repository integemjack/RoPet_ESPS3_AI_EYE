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

/* CRC16-CCITT (poly 0x1021, init 0xFFFF) - 查表法, 结果与逐位算法完全一致 */
static const uint16_t bridge_crc16_table[256] = {
    0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF,
    0x1231,0x0210,0x3273,0x2252,0x52B5,0x4294,0x72F7,0x62D6,0x9339,0x8318,0xB37B,0xA35A,0xD3BD,0xC39C,0xF3FF,0xE3DE,
    0x2462,0x3443,0x0420,0x1401,0x64E6,0x74C7,0x44A4,0x5485,0xA56A,0xB54B,0x8528,0x9509,0xE5EE,0xF5CF,0xC5AC,0xD58D,
    0x3653,0x2672,0x1611,0x0630,0x76D7,0x66F6,0x5695,0x46B4,0xB75B,0xA77A,0x9719,0x8738,0xF7DF,0xE7FE,0xD79D,0xC7BC,
    0x48C4,0x58E5,0x6886,0x78A7,0x0840,0x1861,0x2802,0x3823,0xC9CC,0xD9ED,0xE98E,0xF9AF,0x8948,0x9969,0xA90A,0xB92B,
    0x5AF5,0x4AD4,0x7AB7,0x6A96,0x1A71,0x0A50,0x3A33,0x2A12,0xDBFD,0xCBDC,0xFBBF,0xEB9E,0x9B79,0x8B58,0xBB3B,0xAB1A,
    0x6CA6,0x7C87,0x4CE4,0x5CC5,0x2C22,0x3C03,0x0C60,0x1C41,0xEDAE,0xFD8F,0xCDEC,0xDDCD,0xAD2A,0xBD0B,0x8D68,0x9D49,
    0x7E97,0x6EB6,0x5ED5,0x4EF4,0x3E13,0x2E32,0x1E51,0x0E70,0xFF9F,0xEFBE,0xDFDD,0xCFFC,0xBF1B,0xAF3A,0x9F59,0x8F78,
    0x9188,0x81A9,0xB1CA,0xA1EB,0xD10C,0xC12D,0xF14E,0xE16F,0x1080,0x00A1,0x30C2,0x20E3,0x5004,0x4025,0x7046,0x6067,
    0x83B9,0x9398,0xA3FB,0xB3DA,0xC33D,0xD31C,0xE37F,0xF35E,0x02B1,0x1290,0x22F3,0x32D2,0x4235,0x5214,0x6277,0x7256,
    0xB5EA,0xA5CB,0x95A8,0x8589,0xF56E,0xE54F,0xD52C,0xC50D,0x34E2,0x24C3,0x14A0,0x0481,0x7466,0x6447,0x5424,0x4405,
    0xA7DB,0xB7FA,0x8799,0x97B8,0xE75F,0xF77E,0xC71D,0xD73C,0x26D3,0x36F2,0x0691,0x16B0,0x6657,0x7676,0x4615,0x5634,
    0xD94C,0xC96D,0xF90E,0xE92F,0x99C8,0x89E9,0xB98A,0xA9AB,0x5844,0x4865,0x7806,0x6827,0x18C0,0x08E1,0x3882,0x28A3,
    0xCB7D,0xDB5C,0xEB3F,0xFB1E,0x8BF9,0x9BD8,0xABBB,0xBB9A,0x4A75,0x5A54,0x6A37,0x7A16,0x0AF1,0x1AD0,0x2AB3,0x3A92,
    0xFD2E,0xED0F,0xDD6C,0xCD4D,0xBDAA,0xAD8B,0x9DE8,0x8DC9,0x7C26,0x6C07,0x5C64,0x4C45,0x3CA2,0x2C83,0x1CE0,0x0CC1,
    0xEF1F,0xFF3E,0xCF5D,0xDF7C,0xAF9B,0xBFBA,0x8FD9,0x9FF8,0x6E17,0x7E36,0x4E55,0x5E74,0x2E93,0x3EB2,0x0ED1,0x1EF0,
};

/* 增量更新: 可分段累加 (先算 meta, 再算 payload) */
static inline uint16_t bridge_crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)((crc << 8) ^ bridge_crc16_table[((crc >> 8) ^ data[i]) & 0xFF]);
    }
    return crc;
}

/* 一次性计算 (init 0xFFFF) */
static inline uint16_t bridge_crc16(const uint8_t *data, size_t len)
{
    return bridge_crc16_update(0xFFFF, data, len);
}

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_PROTOCOL_H */
