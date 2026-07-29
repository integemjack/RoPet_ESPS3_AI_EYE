#include "wifi_station.h"
#include <cstring>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"

#define TAG "wifi"
#define WIFI_EVENT_CONNECTED BIT0
#define MAX_RECONNECT_COUNT 5

// [本地修改] 被动扫描每信道停留时间: 默认 360ms -> 150ms。
//
// 下面 Start() 里把国家码设成了 "01" (world safe mode), 该模式下 5G 信道只能
// 被动扫描 (不允许主动发 probe)。C5 有 28 个 5G 信道, 按默认 360ms 就是
// 28 x 360ms ≈ 10.1s —— 实测开机日志里 band mode 确认 (1.39s) 到第一次 auth
// (11.38s) 中间整整 10 秒全在这里, 而关联 + DHCP 本身只要 1.4s。
//
// AP 的 beacon 间隔实测是 102.4ms, 停留 150ms 足够收到至少一个 beacon,
// 扫描因此降到 ~4s。万一某个信道漏扫, HandleScanResult 里的 10s 重扫定时器
// 会兜底, 不会卡死在"扫不到"。
//
// 注: 只缩短停留时间, 不裁剪信道集合 —— 实测把 bitmap 限制成只扫 5G 一秒都
// 省不下来 (9.96s -> 9.99s), 因为耗时的恰恰是 5G 那 28 个被动信道本身。
static void StartScan() {
    wifi_scan_config_t scan_config = {};
    scan_config.scan_time.passive = 150;
    esp_wifi_scan_start(&scan_config, false);
}

WifiStation& WifiStation::GetInstance() {
    static WifiStation instance;
    return instance;
}

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %d", err);
    }
    err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
    }
    err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
    if (err != ESP_OK) {
        remember_bssid_ = 0;
    }
    nvs_close(nvs);
}

WifiStation::~WifiStation() {
    vEventGroupDelete(event_group_);
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }
    
    // 取消注册事件处理程序
    if (instance_any_id_ != nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_));
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_));
        instance_got_ip_ = nullptr;
    }

    // Reset the WiFi stack
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::Start() {
    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));

    // Create the default event loop
    esp_netif_create_default_wifi_sta();

    // Initialize the WiFi stack in station mode
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
    // [本地修改] 双频芯片 (C5): 打开 2.4G + 5G, 否则扫描结果里没有 5G AP,
    // 已保存的 5G 凭据永远匹配不上, 会一直退回配网模式。
    // 顺序: 先切频段再设国家码, 反了国家码只会应用到 2.4G。
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
    ESP_ERROR_CHECK(esp_wifi_set_country_code("01", true));
#endif

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    // Setup the timer to scan WiFi
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            StartScan();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    // [本地修改] 排序规则: 先 5GHz, 同频段内再按 RSSI 降序。
    //
    // 上游只按 RSSI 排。但双频路由器通常用同一个 SSID 同时广播 2.4G 和 5G,
    // 而 2.4G 穿透好、RSSI 往往更高, 于是队列里 2.4G 的 BSS 排在前面。
    // 实测某些路由器的 2.4G BSS 会直接拒绝认证 (Disconnected reason=202),
    // 每次尝试约 10s, 要试满 MAX_RECONNECT_COUNT 才轮到 5G ——
    // 开机联网因此多花 30s 以上, S3 那边就表现为"等很久才连上网"。
    //
    // 本产品的配网页只扫 5GHz 信道, 用户选中的 SSID 必然有可用的 5G BSS,
    // 所以优先试 5G 是安全的; 2.4G 仍留在队列里做兜底, 只是不再排在前面。
    // (2.4G 信道号 1~14, 5G 远大于 14)
    std::sort(ap_records, ap_records + ap_num, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        bool a_is_5g = a.primary > 14;
        bool b_is_5g = b.primary > 14;
        if (a_is_5g != b_is_5g) {
            return a_is_5g;
        }
        return a.rssi > b.rssi;
    });

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    for (int i = 0; i < ap_num; i++) {
        auto ap_record = ap_records[i];
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid, 
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    free(ap_records);

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "Wait for next scan");
        esp_timer_start_once(timer_handle_, 10 * 1000);
        return;
    }

    StartConnect();
}

// [本地修改] 开机快速重连: 直接定连上次成功的 BSS, 整段扫描都省掉。
//
// esp_wifi_connect() 在 bssid_set + channel 都给定时不会扫描, 直接去打那个
// BSS。同样的做法已经在配网验证路径 (bridge_wifi.cc 的 bridge_wifi_try_config)
// 上验证过 —— 那边把 12s 的扫描降到了 0。
//
// 返回 false 表示没有可用记录 (首次开机 / 换过凭据 / 上次快路失败已清除),
// 调用方退回正常扫描流程。
bool WifiStation::StartFastConnect() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    char last_ssid[33] = {0};
    size_t ssid_len = sizeof(last_ssid);
    uint8_t bssid[6] = {0};
    size_t bssid_len = sizeof(bssid);
    uint8_t channel = 0;
    bool ok = nvs_get_str(nvs, "last_ssid", last_ssid, &ssid_len) == ESP_OK
           && nvs_get_blob(nvs, "last_bssid", bssid, &bssid_len) == ESP_OK
           && nvs_get_u8(nvs, "last_ch", &channel) == ESP_OK
           && bssid_len == sizeof(bssid) && channel != 0;
    nvs_close(nvs);
    if (!ok) {
        return false;
    }

    // 密码只存在 SsidManager 里。对不上就说明凭据被改过或删过, 老实重扫。
    auto ssid_list = SsidManager::GetInstance().GetSsidList();
    auto it = std::find_if(ssid_list.begin(), ssid_list.end(),
                           [&](const SsidItem& item) { return item.ssid == last_ssid; });
    if (it == ssid_list.end()) {
        return false;
    }

    ssid_ = it->ssid;
    password_ = it->password;
    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strncpy((char *)wifi_config.sta.ssid, it->ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, it->password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.channel = channel;
    memcpy(wifi_config.sta.bssid, bssid, sizeof(bssid));
    wifi_config.sta.bssid_set = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "Fast connect %s at %02x:%02x:%02x:%02x:%02x:%02x ch=%d (skip scan)",
             last_ssid, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], channel);

    fast_connect_active_ = true;
    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
    return true;
}

// 连上之后记下 BSS, 供下次开机的快路使用。
void WifiStation::SaveFastConnect() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, "last_ssid", ssid_.c_str());
    nvs_set_blob(nvs, "last_bssid", ap_info.bssid, sizeof(ap_info.bssid));
    nvs_set_u8(nvs, "last_ch", ap_info.primary);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// 快路失败: 记录里的 BSSID/信道多半已经过期 (换了路由器, 或 132 之类的 DFS
// 信道被雷达赶走换了台)。丢掉它, 免得下次开机再撞一次。
void WifiStation::EraseFastConnect() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs, "last_ssid");
    nvs_erase_key(nvs, "last_bssid");
    nvs_erase_key(nvs, "last_ch");
    nvs_commit(nvs);
    nvs_close(nvs);
}

void WifiStation::StartConnect() {
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, ap_record.ssid.c_str());
    strcpy((char *)wifi_config.sta.password, ap_record.password.c_str());
    if (remember_bssid_) {
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    // Get station info
    wifi_ap_record_t ap_info;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Get station info
    wifi_ap_record_t ap_info;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetPowerSaveMode(bool enabled) {
    ESP_ERROR_CHECK(esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (event_id == WIFI_EVENT_STA_START) {
        // [本地修改] 先试快路: 有上次成功的 BSS 记录就直接定连, 跳过扫描。
        // 没有记录 (首次开机/换过凭据) 才走原来的扫描流程。
        if (this_->StartFastConnect()) {
            return;
        }
        StartScan();
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGW(TAG, "Disconnected from %s, reason=%d",
                 this_->ssid_.c_str(), event->reason);

        // [本地修改] 快路失败不重试, 立刻退回扫描。
        // 记录里的 BSS 已经过期时, 在它上面重试 MAX_RECONNECT_COUNT 次
        // (每次约 1s 起) 比直接重扫还慢, 白白抵消掉快路省下来的时间。
        if (this_->fast_connect_active_) {
            this_->fast_connect_active_ = false;
            this_->EraseFastConnect();
            ESP_LOGW(TAG, "Fast connect failed, fall back to scan");
            StartScan();
            if (this_->on_scan_begin_) {
                this_->on_scan_begin_();
            }
            return;
        }

        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }
        
        ESP_LOGI(TAG, "No more AP to connect, wait for next scan");
        esp_timer_start_once(this_->timer_handle_, 10 * 1000);
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
    // [本地修改] 记下这次连上的 BSS, 下次开机直接定连
    this_->fast_connect_active_ = false;
    this_->SaveFastConnect();
}
