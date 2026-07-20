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
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
} link_t;

static link_t s_links[BRIDGE_MAX_LINKS];
static SemaphoreHandle_t s_table_lock;

void bridge_sock_init(void)
{
    s_table_lock = xSemaphoreCreateMutex();
    memset(s_links, 0, sizeof(s_links));
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

/* ---- TCP/TLS 接收任务 ---- */
static void tls_rx_task(void *arg)
{
    uint8_t link_id = (uint8_t)(uintptr_t)arg;
    link_t *lk = &s_links[link_id];
    uint8_t *buf = malloc(RX_CHUNK);
    if (!buf) { vTaskDelete(NULL); return; }

    while (!lk->closing) {
        int n = esp_tls_conn_read(lk->tls, buf, RX_CHUNK);
        if (n > 0) {
            bridge_send_frame(BRIDGE_EVT_SOCK_DATA, link_id, buf, (uint16_t)n);
        } else if (n == 0 || (n != ESP_TLS_ERR_SSL_WANT_READ && n != ESP_TLS_ERR_SSL_WANT_WRITE)) {
            /* 远端关闭或错误 */
            break;
        }
    }
    free(buf);

    bool report = false;
    xSemaphoreTake(s_table_lock, portMAX_DELAY);
    if (lk->used && !lk->closing) {
        report = true;                 /* 被动关闭, 需通知 S3 */
    }
    link_free_locked(lk);
    xSemaphoreGive(s_table_lock);

    if (report) notify_closed(link_id);
    vTaskDelete(NULL);
}

/* ---- UDP 接收任务 ---- */
static void udp_rx_task(void *arg)
{
    uint8_t link_id = (uint8_t)(uintptr_t)arg;
    link_t *lk = &s_links[link_id];
    uint8_t *buf = malloc(RX_CHUNK);
    if (!buf) { vTaskDelete(NULL); return; }

    while (!lk->closing) {
        int n = recv(lk->udp_fd, buf, RX_CHUNK, 0);
        if (n > 0) {
            bridge_send_frame(BRIDGE_EVT_SOCK_DATA, link_id, buf, (uint16_t)n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        } else {
            break;
        }
    }
    free(buf);

    bool report = false;
    xSemaphoreTake(s_table_lock, portMAX_DELAY);
    if (lk->used && !lk->closing) report = true;
    link_free_locked(lk);
    xSemaphoreGive(s_table_lock);

    if (report) notify_closed(link_id);
    vTaskDelete(NULL);
}

void bridge_sock_open(uint8_t link_id, bridge_proto_t proto,
                      const char *host, uint16_t port)
{
    if (link_id >= BRIDGE_MAX_LINKS) {
        send_opened_result(link_id, false, -1);
        return;
    }

    xSemaphoreTake(s_table_lock, portMAX_DELAY);
    link_t *lk = &s_links[link_id];
    if (lk->used) {                    /* 复用前先清理旧连接 */
        lk->closing = true;
        link_free_locked(lk);
    }
    memset(lk, 0, sizeof(*lk));
    lk->udp_fd = -1;
    lk->proto = proto;
    lk->used = true;
    lk->closing = false;
    lk->send_lock = xSemaphoreCreateMutex();
    xSemaphoreGive(s_table_lock);

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
        xTaskCreate(tls_rx_task, "lk_tls", 6144, (void *)(uintptr_t)link_id, 11, &lk->rx_task);

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
        xTaskCreate(udp_rx_task, "lk_udp", 4096, (void *)(uintptr_t)link_id, 11, &lk->rx_task);
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
