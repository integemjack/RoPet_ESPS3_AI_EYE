/*
 * bridge_sock.c - socket 代理层
 *
 * 每个 link_id 对应一条连接:
 *   TCP / TLS -> 使用 esp-tls (TLS 时做加密, TCP 时明文)
 *   UDP       -> 使用 lwip BSD socket
 * 每条连接起一个接收任务, 把收到的数据打成 EVT_SOCK_DATA 帧回传 S3。
 */
#include "bridge_internal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_tls.h"

static const char *TAG = "bridge_sock";

/* 单帧回传给 S3 的最大数据块, 需 <= BRIDGE_MAX_PAYLOAD */
#define RX_CHUNK   1400

typedef struct {
    bool           used;
    bridge_proto_t proto;
    esp_tls_t     *tls;      /* TCP/TLS 用 */
    int            udp_fd;    /* UDP 用 */
    TaskHandle_t   rx_task;
    SemaphoreHandle_t send_lock;
    volatile bool  closing;
    /* 槽位世代号: 每次 bridge_sock_open 复用槽位时 +1。
     *
     * 用来解决一个竞态: S3 单独重启 (C5 不重启) 时会复用同一个 link_id, 而上一
     * 条连接的 rx_task 可能还阻塞在 read 里没退出。等它醒来时槽位已经被新连接
     * 接管, 它却以为这还是自己的槽位, 于是 link_free_locked() 把新状态抹掉、
     * 再报一个 EVT_SOCK_CLOSED。随后 worker 看到 !used 就 "open aborted",
     * 什么结果都不回, S3 只能干等满 15 秒连接超时。
     *
     * rx_task 启动时记下自己的世代, 退出时只有世代仍匹配才清理/上报。 */
    uint32_t       generation;
} link_t;

static link_t s_links[BRIDGE_MAX_LINKS];
static SemaphoreHandle_t s_table_lock;

/* ---- 建连请求队列 ----
 * DNS 解析和 TLS 握手都是秒级的同步阻塞操作。以前它们直接跑在 UART 收帧任务
 * (bridge_rx) 上: 握手期间 C5 完全不读 UART, 而这根线没有流控、ring buffer 只有
 * 8192 字节, S3 侧继续发就会溢出丢帧 —— 表现为建连时段的 CRC 错误和音频卡顿。
 * 现在把建连丢给独立 worker, 收帧任务只负责入队, 永不阻塞。 */
typedef struct {
    uint8_t        link_id;
    bridge_proto_t proto;
    uint16_t       port;
    uint32_t       generation;   /* 入队时的槽位世代, 用于识别请求是否已过期 */
    char           host[128];
} sock_open_req_t;

#define OPEN_QUEUE_LEN   BRIDGE_MAX_LINKS

static QueueHandle_t s_open_queue;

static void sock_open_blocking(uint8_t link_id, bridge_proto_t proto,
                               const char *host, uint16_t port, uint32_t generation);

static void sock_open_worker(void *arg)
{
    sock_open_req_t req;
    while (1) {
        if (xQueueReceive(s_open_queue, &req, portMAX_DELAY) == pdTRUE) {
            sock_open_blocking(req.link_id, req.proto, req.host, req.port,
                               req.generation);
        }
    }
}

void bridge_sock_init(void)
{
    s_table_lock = xSemaphoreCreateMutex();
    memset(s_links, 0, sizeof(s_links));

    s_open_queue = xQueueCreate(OPEN_QUEUE_LEN, sizeof(sock_open_req_t));
    /* 栈要够 TLS 握手用 (mbedtls 吃栈), 与原先在 rx 任务里的用量对齐 */
    xTaskCreate(sock_open_worker, "sock_open", 6144, NULL, 10, NULL);
}

static void send_opened_result(uint8_t link_id, bool ok, int32_t err)
{
    bridge_sock_result_t r = { .ok = ok ? 1 : 0, .err = err };
    bridge_send_frame(BRIDGE_EVT_SOCK_OPENED, link_id, (const uint8_t *)&r, sizeof(r));
}

static void notify_closed(uint8_t link_id)
{
    bridge_send_frame(BRIDGE_EVT_SOCK_CLOSED, link_id, NULL, 0);
}

/* 释放一条 link 的底层资源 (不含任务自身), 需持有 table_lock 调用 */
static void link_free_locked(link_t *lk)
{
    if (lk->tls) {
        esp_tls_conn_destroy(lk->tls);
        lk->tls = NULL;
    }
    if (lk->udp_fd >= 0) {
        close(lk->udp_fd);
        lk->udp_fd = -1;
    }
    if (lk->send_lock) {
        vSemaphoreDelete(lk->send_lock);
        lk->send_lock = NULL;
    }
    lk->used = false;
    lk->rx_task = NULL;
}

/* rx_task 的参数把 link_id 和世代号打包进一个指针, 免得再分配内存。 */
#define RX_ARG_PACK(link_id, gen)  ((void *)(uintptr_t)(((uint32_t)(gen) << 8) | (link_id)))
#define RX_ARG_LINK(arg)           ((uint8_t)((uintptr_t)(arg) & 0xFF))
#define RX_ARG_GEN(arg)            ((uint32_t)((uintptr_t)(arg) >> 8))

/* rx_task 退出时的收尾: 只有槽位还属于本世代才清理并上报。
 * 返回 true 表示需要通知 S3 (被动关闭)。 */
static bool rx_task_finish(uint8_t link_id, uint32_t my_gen)
{
    bool report = false;
    xSemaphoreTake(s_table_lock, portMAX_DELAY);
    link_t *lk = &s_links[link_id];
    if (lk->generation != my_gen) {
        /* 槽位已被新连接接管 —— 什么都别动, 否则会抹掉新状态。 */
        xSemaphoreGive(s_table_lock);
        ESP_LOGW(TAG, "link=%u stale rx task (gen=%lu, now=%lu) exiting quietly",
                 link_id, (unsigned long)my_gen, (unsigned long)lk->generation);
        return false;
    }
    if (lk->used && !lk->closing) {
        report = true;                 /* 被动关闭, 需通知 S3 */
    }
    link_free_locked(lk);
    xSemaphoreGive(s_table_lock);
    return report;
}

/* ---- TCP/TLS 接收任务 ---- */
static void tls_rx_task(void *arg)
{
    uint8_t link_id = RX_ARG_LINK(arg);
    uint32_t my_gen = RX_ARG_GEN(arg);
    link_t *lk = &s_links[link_id];
    uint8_t *buf = malloc(RX_CHUNK);
    if (!buf) { vTaskDelete(NULL); return; }

    int last_n = 0;          /* 退出循环时 esp_tls_conn_read 的返回值 */
    int last_errno = 0;
    uint32_t total_rx = 0;   /* 本条连接累计收到的字节数 */

    while (!lk->closing) {
        errno = 0;
        int n = esp_tls_conn_read(lk->tls, buf, RX_CHUNK);
        if (n > 0) {
            total_rx += (uint32_t)n;
            bridge_send_frame(BRIDGE_EVT_SOCK_DATA, link_id, buf, (uint16_t)n);
            continue;
        }
        if (n == 0) {
            last_n = 0;          /* 远端正常关闭 (FIN) */
            break;
        }

        /* n < 0: 要区分"暂时没数据"和"真出错"。
         *
         * 这里原来只认 mbedtls 的 ESP_TLS_ERR_SSL_WANT_READ/WANT_WRITE (-0x6900
         * 一类的常量)。但明文 TCP 走的是 cfg.is_plain_tcp = true, 底层就是裸
         * socket, 无数据可读时返回 -1/EAGAIN —— 和那两个常量根本不相等, 于是
         * 每次读空窗口都被当成致命错误 break 掉, 表现为 WebSocket 隔十几秒
         * 莫名断开一次 (间隔取决于流量节奏, 不是定时器)。
         *
         * 所以两种模式都要放过: TLS 认 mbedtls 的 WANT_*, 明文 TCP 认
         * EAGAIN/EWOULDBLOCK/EINTR (与下面 udp_rx_task 的处理一致)。 */
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            /* 非阻塞 socket 的读空窗口。让出 CPU 再试, 否则这里会空转成忙等。 */
            vTaskDelay(1);
            continue;
        }

        last_n = n;
        last_errno = errno;
        break;
    }
    free(buf);

    if (!lk->closing) {
        if (last_n == 0) {
            ESP_LOGW(TAG, "link=%u closed by peer (FIN), rx_total=%lu",
                     link_id, (unsigned long)total_rx);
        } else {
            ESP_LOGW(TAG, "link=%u read error n=%d (-0x%X) errno=%d (%s), rx_total=%lu",
                     link_id, last_n, -last_n, last_errno, strerror(last_errno),
                     (unsigned long)total_rx);
        }
    }

    if (rx_task_finish(link_id, my_gen)) notify_closed(link_id);
    vTaskDelete(NULL);
}

/* ---- UDP 接收任务 ---- */
static void udp_rx_task(void *arg)
{
    uint8_t link_id = RX_ARG_LINK(arg);
    uint32_t my_gen = RX_ARG_GEN(arg);
    link_t *lk = &s_links[link_id];
    uint8_t *buf = malloc(RX_CHUNK);
    if (!buf) { vTaskDelete(NULL); return; }

    int last_n = 0;
    int last_errno = 0;

    while (!lk->closing) {
        errno = 0;
        int n = recv(lk->udp_fd, buf, RX_CHUNK, 0);
        if (n > 0) {
            bridge_send_frame(BRIDGE_EVT_SOCK_DATA, link_id, buf, (uint16_t)n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        } else {
            last_n = n;
            last_errno = errno;
            break;
        }
    }
    free(buf);

    if (!lk->closing) {
        ESP_LOGW(TAG, "link=%u udp recv ended n=%d errno=%d (%s)",
                 link_id, last_n, last_errno, strerror(last_errno));
    }

    if (rx_task_finish(link_id, my_gen)) notify_closed(link_id);
    vTaskDelete(NULL);
}

/* 收帧任务调用: 只占好槽位然后入队, 绝不阻塞。 */
void bridge_sock_open(uint8_t link_id, bridge_proto_t proto,
                      const char *host, uint16_t port)
{
    if (link_id >= BRIDGE_MAX_LINKS) {
        send_opened_result(link_id, false, -1);
        return;
    }

    /* 槽位必须在这里就占住 (而不是留给 worker): S3 收到 EVT_SOCK_OPENED 之前
     * 不会发数据, 但 SOCK_CLOSE 可能随时来, 槽位状态得立刻是一致的。 */
    xSemaphoreTake(s_table_lock, portMAX_DELAY);
    link_t *lk = &s_links[link_id];
    if (lk->used) {                    /* 复用前先清理旧连接 */
        lk->closing = true;
        link_free_locked(lk);
    }
    /* 世代号要跨 memset 保留并递增: 旧连接的 rx_task 可能还没退出, 靠这个值
     * 才能认出"槽位已经不是我的了"。 */
    uint32_t next_gen = lk->generation + 1;
    memset(lk, 0, sizeof(*lk));
    lk->generation = next_gen;
    lk->udp_fd = -1;
    lk->proto = proto;
    lk->used = true;
    lk->closing = false;
    lk->send_lock = xSemaphoreCreateMutex();
    xSemaphoreGive(s_table_lock);

    sock_open_req_t req = {
        .link_id = link_id,
        .proto   = proto,
        .port    = port,
        .generation = next_gen,
    };
    snprintf(req.host, sizeof(req.host), "%s", host);

    /* 队列长度等于 link 数, 且每条 link 同时只会有一个未完成的 open,
     * 正常情况下不可能满。满了说明状态不一致, 直接回失败而不是阻塞。 */
    if (xQueueSend(s_open_queue, &req, 0) != pdTRUE) {
        ESP_LOGE(TAG, "open queue full, link=%u", link_id);
        xSemaphoreTake(s_table_lock, portMAX_DELAY);
        link_free_locked(lk);
        xSemaphoreGive(s_table_lock);
        send_opened_result(link_id, false, -7);
    }
}

/* worker 任务调用: 这里可以放心阻塞 (DNS / TLS 握手)。 */
static void sock_open_blocking(uint8_t link_id, bridge_proto_t proto,
                               const char *host, uint16_t port, uint32_t generation)
{
    link_t *lk = &s_links[link_id];

    /* 世代对不上: 这个 open 请求已经被更新的一次 open 取代了。新请求会自己回
     * 结果, 这里必须保持沉默, 否则会抢答。 */
    if (lk->generation != generation) {
        ESP_LOGW(TAG, "open request superseded, link=%u (req gen=%lu, now=%lu)",
                 link_id, (unsigned long)generation, (unsigned long)lk->generation);
        return;
    }

    /* 入队后到这里之前, S3 可能已经发了 SOCK_CLOSE。那就别再建连了。
     *
     * 注意必须回一个失败结果: 以前这里直接 return, 而 S3 的 C5Tcp::Connect()
     * 在等 EVT_SOCK_OPENED, 等不到就要干耗满 15 秒超时。开机时 S3 单独重启
     * (C5 不重启) 复用 link 0 就会踩到, 表现为 "C5Tcp: connect timeout" +
     * OTA 检查失败告警。 */
    if (!lk->used || lk->closing) {
        ESP_LOGW(TAG, "open aborted before start, link=%u", link_id);
        send_opened_result(link_id, false, -8);
        return;
    }

    if (proto == BRIDGE_PROTO_TCP || proto == BRIDGE_PROTO_TLS) {
        esp_tls_cfg_t cfg = {0};
        if (proto == BRIDGE_PROTO_TLS) {
            cfg.crt_bundle_attach = NULL; /* 见下方说明: 需要证书校验时挂 bundle */
            /* 未挂证书 bundle 时, esp-tls 默认不校验服务器证书(跳过)。
             * 生产环境应启用 esp_crt_bundle_attach 以校验。 */
            cfg.skip_common_name = true;
        }
        esp_tls_t *tls = esp_tls_init();
        if (!tls) {
            xSemaphoreTake(s_table_lock, portMAX_DELAY);
            link_free_locked(lk);
            xSemaphoreGive(s_table_lock);
            send_opened_result(link_id, false, -2);
            return;
        }
        /* TCP 明文: 用 plain-tcp 模式; TLS: 正常握手 */
        int ret;
        if (proto == BRIDGE_PROTO_TCP) {
            cfg.is_plain_tcp = true;
        }
        ret = esp_tls_conn_new_sync(host, strlen(host), port, &cfg, tls);
        if (ret != 1) {
            esp_tls_conn_destroy(tls);
            xSemaphoreTake(s_table_lock, portMAX_DELAY);
            link_free_locked(lk);
            xSemaphoreGive(s_table_lock);
            send_opened_result(link_id, false, -3);
            return;
        }
        lk->tls = tls;
        send_opened_result(link_id, true, 0);
        xTaskCreate(tls_rx_task, "lk_tls", 6144, RX_ARG_PACK(link_id, generation),
                    11, &lk->rx_task);

    } else { /* UDP */
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
        struct addrinfo *res = NULL;
        char portstr[8];
        snprintf(portstr, sizeof(portstr), "%u", port);
        if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
            xSemaphoreTake(s_table_lock, portMAX_DELAY);
            link_free_locked(lk);
            xSemaphoreGive(s_table_lock);
            send_opened_result(link_id, false, -4);
            return;
        }
        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            freeaddrinfo(res);
            xSemaphoreTake(s_table_lock, portMAX_DELAY);
            link_free_locked(lk);
            xSemaphoreGive(s_table_lock);
            send_opened_result(link_id, false, -5);
            return;
        }
        /* connect UDP: 固定对端, 之后可用 send/recv */
        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            close(fd);
            freeaddrinfo(res);
            xSemaphoreTake(s_table_lock, portMAX_DELAY);
            link_free_locked(lk);
            xSemaphoreGive(s_table_lock);
            send_opened_result(link_id, false, -6);
            return;
        }
        freeaddrinfo(res);
        lk->udp_fd = fd;
        send_opened_result(link_id, true, 0);
        xTaskCreate(udp_rx_task, "lk_udp", 4096, RX_ARG_PACK(link_id, generation),
                    11, &lk->rx_task);
    }
}

void bridge_sock_send(uint8_t link_id, const uint8_t *data, uint16_t len)
{
    if (link_id >= BRIDGE_MAX_LINKS) return;
    link_t *lk = &s_links[link_id];
    if (!lk->used || lk->closing) return;

    xSemaphoreTake(lk->send_lock, portMAX_DELAY);
    if (lk->proto == BRIDGE_PROTO_UDP) {
        if (lk->udp_fd >= 0) send(lk->udp_fd, data, len, 0);
    } else if (lk->tls) {
        size_t written = 0;
        while (written < len && !lk->closing) {
            int w = esp_tls_conn_write(lk->tls, data + written, len - written);
            if (w > 0) {
                written += w;
            } else if (w == ESP_TLS_ERR_SSL_WANT_WRITE || w == ESP_TLS_ERR_SSL_WANT_READ) {
                vTaskDelay(pdMS_TO_TICKS(1));
            } else {
                break;    /* 写错误, 交给 rx_task 检测断开 */
            }
        }
    }
    xSemaphoreGive(lk->send_lock);
}

void bridge_sock_close(uint8_t link_id)
{
    if (link_id >= BRIDGE_MAX_LINKS) return;
    link_t *lk = &s_links[link_id];

    xSemaphoreTake(s_table_lock, portMAX_DELAY);
    if (!lk->used) {
        xSemaphoreGive(s_table_lock);
        return;
    }
    ESP_LOGW(TAG, "link=%u close requested by S3", link_id);
    lk->closing = true;      /* 通知 rx_task 退出 */
    /* 主动关闭底层 fd 以唤醒阻塞的 recv/read */
    if (lk->udp_fd >= 0) {
        shutdown(lk->udp_fd, SHUT_RDWR);
    }
    xSemaphoreGive(s_table_lock);
    /* 资源真正释放交由 rx_task 退出时的 link_free_locked 完成,
     * 这里是主动关闭, rx_task 不会再回传 EVT_SOCK_CLOSED */
}

void bridge_sock_close_all(void)
{
    for (uint8_t i = 0; i < BRIDGE_MAX_LINKS; i++) {
        if (s_links[i].used) {
            bridge_sock_close(i);
        }
    }
}
