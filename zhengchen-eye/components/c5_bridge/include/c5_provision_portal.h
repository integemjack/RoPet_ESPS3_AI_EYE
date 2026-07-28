/*
 * c5_provision_portal.h - S3 侧配网门户
 *
 * 为什么热点在 S3 而不在 C5:
 * C5 是单射频。它自己开 SoftAP 做配网时, 一旦 STA 去关联目标 AP (尤其 5G,
 * 信道 128), 驱动会把 SoftAP 强行拖到同一信道, 手机掉线数秒。实测手机会因此
 * 冻结配网页的 JS, 导致"验证结果"永远送不到页面上 —— 同步等待、异步轮询、
 * 服务端状态注入、captive 探测重投都试过, 全部败在这个根因上。
 *
 * 而 S3 自带一颗 2.4G 射频, 在 C5 网络模式下完全闲置。用它承载配网热点后:
 *   - 手机始终连在 S3 上, 信道固定, 全程不掉线
 *   - C5 那边没有热点要维持, 换信道随意
 *   - /submit 可以同步等 C5 的验证结果再返回, 密码错就当场报错
 * 所有绕行设计因此可以删除。
 *
 * 分工: S3 出热点 + 网页 + DNS 劫持; C5 出 5G 扫描列表 + 真实连接验证 + 存凭据。
 */
#ifndef C5_PROVISION_PORTAL_H
#define C5_PROVISION_PORTAL_H

#include <string>
#include <atomic>

#include <esp_http_server.h>
#include <esp_netif.h>

#include "c5_bridge.h"
#include "dns_server.h"

class C5ProvisionPortal {
public:
    explicit C5ProvisionPortal(C5Bridge& bridge) : bridge_(bridge) {}

    // 启动 S3 的 SoftAP + DNS + Web 服务。ssid_prefix 用于生成 "前缀-XXXX"。
    void Start(const std::string& ssid_prefix);

    const std::string& ssid() const { return ssid_; }
    std::string url() const { return "http://192.168.4.1"; }

private:
    void StartAccessPoint();
    void StartWebServer();
    // 通知 C5 复位后重启自己 (幂等: /reboot 与兜底定时可能都触发)
    void RebootBoth();

    C5Bridge&      bridge_;
    DnsServer      dns_server_;
    httpd_handle_t server_ = nullptr;
    esp_netif_t*   ap_netif_ = nullptr;
    std::string    ssid_;
    bool              provisioned_ = false;   // 有过一次成功的验证
    std::atomic<bool> rebooting_{false};
};

#endif // C5_PROVISION_PORTAL_H
