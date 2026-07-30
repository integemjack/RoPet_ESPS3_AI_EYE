/*
 * bridge_wifi.cc - C5 WiFi 管理
 *
 * 角色变更 (重要):
 * 以前 C5 自己开 SoftAP + captive portal 做配网。但 C5 是**单射频**: STA 一去
 * 关联目标 AP (尤其 5G, 信道 128), 驱动就会把 SoftAP 强行拖到同一信道, 手机
 * 立刻掉线数秒。实测手机会因此把配网页的 JS 冻结, 于是"验证结果"永远送不到
 * 页面上 —— 试过同步等待、异步轮询、服务端状态注入、captive 探测重投, 全部
 * 败在同一个根因上。
 *
 * 现在改成: **配网热点由 S3 的 2.4G 射频承载** (那颗射频在 C5 模式下本来闲置)。
 * S3 出热点和配网页, 手机始终连在 S3 上、从头到尾不掉线; C5 这边不再有热点要
 * 维持, 射频独占, 爱怎么换信道换信道。职责因此简化为三件事:
 *   1. 扫描 5G 频段, 把 SSID 列表回传给 S3 (S3 的 2.4G 射频看不见 5G AP)
 *   2. 用 S3 下发的凭据做一次真实连接验证, 把成功/失败原因回传
 *   3. 验证通过才写 NVS, 之后用 WifiStation 正常连接
 *
 * 于是配网的"结果送达"变成一次普通的同步 HTTP 响应, 不再需要任何绕行设计。
 */
#include "bridge_internal.h"   /* 头文件自带 extern "C" 守卫 */

#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_mac.h"      /* MACSTR / MAC2STR */
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_station.h"
#include "ssid_manager.h"
#include "nvs.h"

static const char *TAG = "bridge_wifi";

/* 处于"等待 S3 配网"状态: 已初始化射频但没有可用凭据, 只响应扫描/验证请求。 */
static bool s_provisioning = false;

#define PROV_CONNECTED_BIT  BIT0   /* 拿到 IP */
#define PROV_FAILED_BIT     BIT1
#define PROV_ASSOCIATED_BIT BIT2   /* 关联 + 4 次握手都过了, 只差 DHCP */
static EventGroupHandle_t s_prov_events = nullptr;
static volatile uint8_t s_prov_disconnect_reason = 0;
static volatile bool s_prov_connecting = false;

/* 配网扫描的结果缓存: 每个 SSID 保留信号最强的那个 5G BSS。
 *
 * 验证凭据时直接锁定这里的 bssid + channel, 省掉 esp_wifi_connect() 内部那次
 * 全信道扫描 —— 在 C5 上它要扫 2.4G 14 个 + 5G 28 个信道, 实测 12s 以上,
 * 会把验证窗口吃光, 表现为"关联明明成功了却报连接失败"。 */
struct ProvBss {
    std::string ssid;
    uint8_t     bssid[6];
    uint8_t     channel;
    int8_t      rssi;
    uint8_t     authmode;
};
static std::vector<ProvBss> s_scan_cache;

/* 回读一次频段模式, 确认 2.4G + 5G 已生效 (纯诊断用, 不改变行为)。 */
static void bridge_wifi_log_band_mode(void)
{
#if CONFIG_SOC_WIFI_SUPPORT_5G
    wifi_band_mode_t mode = WIFI_BAND_MODE_2G_ONLY;
    if (esp_wifi_get_band_mode(&mode) == ESP_OK) {
        ESP_LOGI(TAG, "band mode = %d (3 = 2.4G + 5G)", (int)mode);
    }
#endif
}

extern "C" void bridge_wifi_init(void)
{
    /* 只做 event loop + NVS。射频的初始化按状态分两条路走:
     * 有凭据 -> WifiStation::Start() 内部初始化; 无凭据 -> 下面的配网模式自己初始化。 */
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "nvs + event loop ready");
}

/* 配网模式下的 STA 事件: 只用来驱动"验证"这一件事的成败判定。 */
static void prov_wifi_event_handler(void* arg, esp_event_base_t base,
                                    int32_t id, void* data)
{
    if (id == WIFI_EVENT_STA_CONNECTED) {
        /* 关联 + 4 次握手都过了。单独标出来是为了把"密码错"和"连上了但
         * DHCP 不给 IP"分开 —— 后者不该报成密码错误让用户白改密码。 */
        xEventGroupSetBits(s_prov_events, PROV_ASSOCIATED_BIT);
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
        s_prov_disconnect_reason = ev->reason;
        xEventGroupClearBits(s_prov_events, PROV_ASSOCIATED_BIT);

        if (!s_prov_connecting) {
            return;   /* 验证结束后我们自己断开的, 不要触发重连 */
        }

        /* 只有"确定性失败"才立即判负。双频同名 SSID 时驱动会先在 2.4G 上
         * auth 失败再切到 5G, 那种瞬时失败必须继续等, 否则明明能连上却被判失败。 */
        bool fatal = (ev->reason == WIFI_REASON_AUTH_FAIL ||
                      ev->reason == WIFI_REASON_NO_AP_FOUND ||
                      ev->reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                      ev->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);
        ESP_LOGW(TAG, "provision STA disconnected, reason=%d (%s)", ev->reason,
                 fatal ? "fatal" : "retryable");
        if (fatal) {
            xEventGroupSetBits(s_prov_events, PROV_FAILED_BIT);
        } else {
            esp_wifi_connect();
        }
    }
}

static void prov_ip_event_handler(void* arg, esp_event_base_t base,
                                  int32_t id, void* data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_prov_events, PROV_CONNECTED_BIT);
    }
}

/* 进入"等待 S3 配网"状态: 纯 STA 模式起射频, 不开任何热点。 */
static void enter_provision_mode()
{
    s_provisioning = true;
    s_prov_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &prov_wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &prov_ip_event_handler, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
    /* 顺序不能反: 先切频段模式再设国家码, 否则国家码只会应用到 2.4G。
     * "01" = world safe mode, 配合 802.11d 跟随所连 AP 的 country IE。 */
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
    ESP_ERROR_CHECK(esp_wifi_set_country_code("01", true));
#endif
    bridge_wifi_log_band_mode();

    /* 告诉 S3: 我没凭据, 请开配网热点 */
    bridge_send_frame(BRIDGE_EVT_NEED_PROVISION, BRIDGE_NO_LINK, NULL, 0);
    bridge_log("C5 has no wifi credentials, S3 please start provisioning AP");
    ESP_LOGW(TAG, "no saved ssid -> provisioning mode (waiting for S3)");

    bridge_wifi_report_status();
}

extern "C" void bridge_wifi_start(void)
{
    auto &ssid_manager = SsidManager::GetInstance();
    if (ssid_manager.GetSsidList().empty()) {
        enter_provision_mode();
        return;
    }

    auto &wifi_station = WifiStation::GetInstance();
    wifi_station.OnConnected([](const std::string &ssid) {
        ESP_LOGI(TAG, "connected to %s", ssid.c_str());
        bridge_wifi_report_status();
    });
    wifi_station.Start();

    bridge_wifi_log_band_mode();

    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        ESP_LOGW(TAG, "connect timeout with saved credentials");
        /* 凭据存在但连不上 (换了路由器/改了密码): 同样请 S3 开配网热点,
         * 让用户能重新配。这里不清除旧凭据, 配网成功时会被覆盖。 */
        bridge_send_frame(BRIDGE_EVT_NEED_PROVISION, BRIDGE_NO_LINK, NULL, 0);
        return;
    }

    ESP_LOGI(TAG, "wifi connected");
    bridge_wifi_report_status();
}

extern "C" bool bridge_wifi_is_connected(void)
{
    if (s_provisioning) return false;
    return WifiStation::GetInstance().IsConnected();
}

extern "C" bool bridge_wifi_is_provisioning(void)
{
    return s_provisioning;
}

extern "C" void bridge_wifi_announce_provision(void)
{
    if (!s_provisioning) return;
    bridge_send_frame(BRIDGE_EVT_NEED_PROVISION, BRIDGE_NO_LINK, NULL, 0);
}

/* 扫描 5G 频段, 结果写进 s_scan_cache (同 SSID 只留最强的那个 BSS, 按 RSSI 降序)。
 * 只扫 5G: 本产品固定用 5G, 而且 S3 的 2.4G 射频自己就能看见 2.4G,
 * 混在一起只会让用户误选到连不上的 2.4G 同名 SSID。
 * 返回 true 表示扫描本身成功 (扫到 0 个 AP 也算成功)。 */
static bool prov_scan_5g(void)
{
    s_scan_cache.clear();

    wifi_scan_config_t scan_config = {};
    /* 被动扫描每信道停留 360ms(默认) -> 150ms。国家码 "01" 下 5G 只能被动扫,
     * 28 个信道按默认值要 ~10s, 配网页拉 SSID 列表就得干等这么久。
     * AP 的 beacon 间隔实测 102.4ms, 停留 150ms 足够收到一个。
     * (与 wifi_station.cc 里 StartScan() 的取值保持一致) */
    scan_config.scan_time.passive = 150;
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
    scan_config.channel = 0;   /* 0 才会启用下面的 bitmap */
    scan_config.channel_bitmap.ghz_2_channels = 0;
    scan_config.channel_bitmap.ghz_5_channels =
        WIFI_CHANNEL_36  | WIFI_CHANNEL_40  | WIFI_CHANNEL_44  | WIFI_CHANNEL_48  |
        WIFI_CHANNEL_52  | WIFI_CHANNEL_56  | WIFI_CHANNEL_60  | WIFI_CHANNEL_64  |
        WIFI_CHANNEL_100 | WIFI_CHANNEL_104 | WIFI_CHANNEL_108 | WIFI_CHANNEL_112 |
        WIFI_CHANNEL_116 | WIFI_CHANNEL_120 | WIFI_CHANNEL_124 | WIFI_CHANNEL_128 |
        WIFI_CHANNEL_132 | WIFI_CHANNEL_136 | WIFI_CHANNEL_140 | WIFI_CHANNEL_144 |
        WIFI_CHANNEL_149 | WIFI_CHANNEL_153 | WIFI_CHANNEL_157 | WIFI_CHANNEL_161 |
        WIFI_CHANNEL_165 | WIFI_CHANNEL_169 | WIFI_CHANNEL_173 | WIFI_CHANNEL_177;
#endif

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);   /* 阻塞扫描 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) return true;

    std::vector<wifi_ap_record_t> records(ap_num);
    esp_wifi_scan_get_ap_records(&ap_num, records.data());
    records.resize(ap_num);

    /* 排序: 先 5GHz, 同频段内再按 RSSI 降序。
     *
     * 上面的 channel_bitmap 只是"尽量"限制到 5G, 实测挡不住 2.4G 的 BSS ——
     * 扫描结果里照样有 ch6 的条目 (S3 自己的配网热点、同名双频路由器的 2.4G)。
     * 而下面的去重是"同 SSID 只保留第一个", 只按 RSSI 排的话, 双频同名时
     * 2.4G 往往更强 (穿透好), 缓存里就存成了 2.4G 的 BSS。
     *
     * 后果很直接: bridge_wifi_try_config 会把 STA 锁死到那个 2.4G BSS,
     * 而实测这台路由器的 2.4G BSS 直接拒绝认证 (reason=202), 验证秒失败并
     * 报"密码错误" —— 密码其实是对的。wifi_station.cc 的 HandleScanResult
     * 早就为同一个坑做了 5G 优先排序, 这里保持一致。
     * (2.4G 信道号 1~14, 5G 远大于 14) */
    std::sort(records.begin(), records.end(),
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
                  bool a_is_5g = a.primary > 14;
                  bool b_is_5g = b.primary > 14;
                  if (a_is_5g != b_is_5g) {
                      return a_is_5g;
                  }
                  return a.rssi > b.rssi;
              });

    /* 同 SSID 只保留信号最强的那个 BSS: 配网页不必列出一堆重复项,
     * 验证时也直接用这一条锁定信道。 */
    for (const auto& r : records) {
        std::string ssid(reinterpret_cast<const char*>(r.ssid));
        if (ssid.empty()) continue;
        auto same = [&](const ProvBss& b) { return b.ssid == ssid; };
        if (std::find_if(s_scan_cache.begin(), s_scan_cache.end(), same)
                != s_scan_cache.end()) {
            continue;
        }
        ProvBss bss;
        bss.ssid = ssid;
        memcpy(bss.bssid, r.bssid, sizeof(bss.bssid));
        bss.channel  = r.primary;
        bss.rssi     = r.rssi;
        bss.authmode = static_cast<uint8_t>(r.authmode);
        s_scan_cache.push_back(bss);
    }
    return true;
}

/* 从缓存里取出某个 SSID 的 BSS。按值拷出去 —— 调用方拿到后可能会清缓存。 */
static bool prov_lookup(const char* ssid, ProvBss* out)
{
    for (const auto& b : s_scan_cache) {
        if (b.ssid == ssid) {
            *out = b;
            return true;
        }
    }
    return false;
}

static void prov_forget(const char* ssid)
{
    s_scan_cache.erase(std::remove_if(s_scan_cache.begin(), s_scan_cache.end(),
                                      [&](const ProvBss& b) { return b.ssid == ssid; }),
                       s_scan_cache.end());
}

/* 扫描 5G 频段并把结果回传 S3。 */
extern "C" void bridge_wifi_scan_and_report(void)
{
    if (!s_provisioning) {
        ESP_LOGW(TAG, "scan requested but not in provisioning mode");
    }

    if (!prov_scan_5g()) {
        uint8_t zero = 0;
        bridge_send_frame(BRIDGE_EVT_WIFI_SCAN_RESULT, BRIDGE_NO_LINK, &zero, 1);
        return;
    }

    std::vector<uint8_t> payload;
    payload.push_back(0);   /* count 占位 */
    uint8_t count = 0;
    for (const auto& b : s_scan_cache) {
        /* 单帧上限 4096, 每条最多 3 + 32 字节, 留足余量后封顶 40 条 */
        if (count >= 40) break;
        payload.push_back(static_cast<uint8_t>(b.rssi));
        payload.push_back(b.authmode);
        payload.push_back(static_cast<uint8_t>(b.ssid.size()));
        payload.insert(payload.end(), b.ssid.begin(), b.ssid.end());
        count++;
    }
    payload[0] = count;

    ESP_LOGI(TAG, "scan done, report %d ssid(s) to S3", count);
    bridge_send_frame(BRIDGE_EVT_WIFI_SCAN_RESULT, BRIDGE_NO_LINK,
                      payload.data(), static_cast<uint16_t>(payload.size()));
}

/* 把验证通过的 BSS 写进 WifiStation 的快速重连记录 (NVS 命名空间 "wifi")。
 * 键名必须和 wifi_station.cc 里的 StartFastConnect/SaveFastConnect 一致。 */
static void prov_seed_fast_connect(const char* ssid, const uint8_t* bssid, uint8_t channel)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, "last_ssid", ssid);
    nvs_set_blob(nvs, "last_bssid", bssid, 6);
    nvs_set_u8(nvs, "last_ch", channel);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "seeded fast-connect record: %s ch=%u", ssid, channel);
}

/* 回传验证结果。ok=0 时附带错误文案 (UTF-8, 直接显示在 S3 的配网页上)。 */
static void report_config_result(bool ok, const char* err)
{
    uint8_t buf[128];
    buf[0] = ok ? 1 : 0;
    size_t n = 0;
    if (!ok && err) {
        n = strlen(err);
        if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
        memcpy(buf + 1, err, n);
    }
    bridge_send_frame(BRIDGE_EVT_WIFI_CONFIG_RESULT, BRIDGE_NO_LINK,
                      buf, static_cast<uint16_t>(1 + n));
}

/* 用 S3 下发的凭据做一次真实连接验证。
 * 成功才写 NVS —— 这样"保存的凭据"永远是验证过的。
 * 注意: 无论成败都不在这里重启; 失败要让用户在 S3 的配网页上重填,
 * 成功后的重启时序由 S3 统一编排 (它要先把 HTTP 响应发给手机)。 */
extern "C" void bridge_wifi_try_config(const char* ssid, const char* password)
{
    if (!s_provisioning) {
        report_config_result(false, "设备不在配网状态");
        return;
    }
    if (!ssid || ssid[0] == '\0') {
        report_config_result(false, "SSID 不能为空");
        return;
    }

    ESP_LOGI(TAG, "verifying credentials for %s", ssid);
    esp_wifi_scan_stop();

    /* 先确定要连哪个 BSS。缓存来自配网页那次扫描, 通常直接命中;
     * 没命中就现扫一次 5G (约 4s) —— 仍然远比让 esp_wifi_connect()
     * 自己去做 2.4G + 5G 全信道扫描 (12s+) 便宜。 */
    ProvBss bss;
    if (!prov_lookup(ssid, &bss)) {
        ESP_LOGI(TAG, "%s not in scan cache, rescanning 5G", ssid);
        prov_scan_5g();
    }
    if (!prov_lookup(ssid, &bss)) {
        ESP_LOGW(TAG, "verify failed for %s (no 5G BSS found)", ssid);
        report_config_result(false, "找不到该 WiFi, 请确认名称和信号");
        return;
    }
    ESP_LOGI(TAG, "target BSS " MACSTR " ch=%u rssi=%d",
             MAC2STR(bss.bssid), bss.channel, bss.rssi);

    xEventGroupClearBits(s_prov_events,
                         PROV_CONNECTED_BIT | PROV_FAILED_BIT | PROV_ASSOCIATED_BIT);
    s_prov_disconnect_reason = 0;

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password ? password : "",
            sizeof(wifi_config.sta.password) - 1);
    /* 锁死到扫描时看见的那个 5G BSS: FAST_SCAN 只在指定信道上探一次,
     * 既不会跑去试同名的 2.4G BSS (每次 auth 超时约 1s), 也不用等全信道扫完。
     * failure_retry_cnt 这里不设: 它只对 ALL_CHANNEL_SCAN 生效, 而且会把断连
     * 事件吞掉 —— 上一版 reason 永远是 0 就是它干的, 错误文案跟着一起失真。 */
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.bssid_set   = true;
    memcpy(wifi_config.sta.bssid, bss.bssid, sizeof(wifi_config.sta.bssid));
    wifi_config.sta.channel     = bss.channel;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    s_prov_connecting = true;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_prov_connecting = false;
        report_config_result(false, "无法发起连接");
        return;
    }

    /* 分两段等, 别把关联和 DHCP 挤在同一个窗口里。
     * 上一版就是挤在一个 15s 里: 驱动的全信道扫描吃掉 12s、同名 2.4G BSS 的
     * auth 超时再吃掉 1s, 轮到 DHCP 只剩 1s, 于是关联成功也被判成失败。 */
    EventBits_t bits = xEventGroupWaitBits(s_prov_events,
                                           PROV_ASSOCIATED_BIT | PROV_FAILED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(12000));
    bool associated = (bits & PROV_ASSOCIATED_BIT) != 0;
    if (associated) {
        /* 关联上了才给 DHCP 单独计时。弱信号 (RSSI < -80) 下 DHCP 要重传几轮。 */
        bits = xEventGroupWaitBits(s_prov_events,
                                   PROV_CONNECTED_BIT | PROV_FAILED_BIT,
                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(12000));
    }
    s_prov_connecting = false;

    if (bits & PROV_CONNECTED_BIT) {
        ESP_LOGI(TAG, "credentials verified, saving %s", ssid);
        SsidManager::GetInstance().AddSsid(ssid, password ? password : "");
        /* 顺手把刚验证通过的 BSS 种给 WifiStation 的快速重连记录 (NVS 键
         * last_ssid/last_bssid/last_ch, 见 wifi_station.cc: StartFastConnect)。
         * 否则配网成功后的第一次开机没有记录, 还要再全扫一遍 ~4s。
         * 这里的 bssid/channel 刚刚才连通过, 是可信的。 */
        prov_seed_fast_connect(ssid, bss.bssid, bss.channel);
        report_config_result(true, nullptr);
        /* 保持连接状态: S3 收到成功后会安排双方重启, 重启后正常走 WifiStation */
        return;
    }

    // 先把真实原因取出来再断开: esp_wifi_disconnect() 自己会产生一个
    // reason=8 (ASSOC_LEAVE) 的断连事件, 之后再读就只剩我们自己造的原因了。
    uint8_t fail_reason = s_prov_disconnect_reason;
    esp_wifi_disconnect();

    /* 刚才用的 BSS 信息可能已经过期 (132 之类的 DFS 信道被雷达赶走就会换台),
     * 丢掉这条缓存, 用户点重试时会重新扫到新的信道。 */
    prov_forget(ssid);

    const char* reason;
    if (associated) {
        /* 关联和 4 次握手都过了 -> 密码是对的, 卡在 DHCP。别再喊"密码错误"。 */
        reason = "已连上 WiFi 但拿不到 IP 地址, 请检查路由器";
    } else {
        switch (fail_reason) {
        case WIFI_REASON_NO_AP_FOUND:
            reason = "找不到该 WiFi, 请确认名称和信号";
            break;
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            reason = "密码错误";
            break;
        case 0:
            /* 一次断连事件都没有: BSS 扫得到但关联不上, 基本都是信号太弱 */
            reason = "连接超时, WiFi 信号可能太弱";
            break;
        default:
            reason = "连接失败, 请检查密码";
            break;
        }
    }
    ESP_LOGW(TAG, "verify failed for %s (associated=%d, reason=%d)",
             ssid, associated ? 1 : 0, fail_reason);
    report_config_result(false, reason);
}

extern "C" void bridge_wifi_reset_credentials(void)
{
    auto &ssid_manager = SsidManager::GetInstance();
    size_t before = ssid_manager.GetSsidList().size();
    ssid_manager.Clear();

    ESP_LOGW(TAG, "wifi credentials cleared (%u entries removed)", (unsigned)before);
    bridge_log("C5 wifi credentials cleared (%u entries), rebooting", (unsigned)before);
}

extern "C" void bridge_wifi_report_ota_url(void)
{
    /* 保留接口以兼容旧 S3 固件。新流程里 OTA 地址由 S3 的配网页直接存在 S3 的
     * NVS 上, 不再需要从 C5 同步。 */
    char ota_url[256] = {0};
    size_t len = sizeof(ota_url);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "ota_url", ota_url, &len);
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ota_url[0] = '\0';
        len = 0;
    } else {
        len = strlen(ota_url);
    }
    bridge_send_frame(BRIDGE_EVT_OTA_URL, BRIDGE_NO_LINK,
                      (const uint8_t *)ota_url, (uint16_t)len);
}

extern "C" void bridge_wifi_report_status(void)
{
    bridge_wifi_status_t st;
    memset(&st, 0, sizeof(st));

    if (s_provisioning) {
        bridge_send_frame(BRIDGE_EVT_WIFI_STATUS, BRIDGE_NO_LINK,
                          (const uint8_t *)&st, sizeof(st));
        return;
    }

    auto &wifi_station = WifiStation::GetInstance();
    bool connected = wifi_station.IsConnected();
    st.connected = connected ? 1 : 0;

    if (connected) {
        st.rssi = wifi_station.GetRssi();
        uint8_t ch = wifi_station.GetChannel();
        st.band = (ch > 14) ? 2 : 1;

        std::string ip = wifi_station.GetIpAddress();
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            st.ip[0] = (uint8_t)a; st.ip[1] = (uint8_t)b;
            st.ip[2] = (uint8_t)c; st.ip[3] = (uint8_t)d;
        }
    }

    bridge_send_frame(BRIDGE_EVT_WIFI_STATUS, BRIDGE_NO_LINK,
                      (const uint8_t *)&st, sizeof(st));
}
