/*
 * c5_provision_portal.cc - S3 侧配网门户实现
 * 设计背景见头文件。
 *
 * 网页用的是小智原版配网页 (assets/ 下两个文件从 78__esp-wifi-connect 原样拷来,
 * 未做任何修改)。原版 /submit 是同步的 —— 提交后等结果, 成功才跳 /done.html。
 * 这套流程之前在 C5 上跑不通, 是因为 C5 单射频、验证时热点会掉,请求被掐断;
 * 热点挪到 S3 之后这个前提消失了, 原页面因此可以原封不动地用。
 */
#include "c5_provision_portal.h"

#include <cstring>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <lwip/ip_addr.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "C5Portal"

// 内容即小智原版配网页 (assets/c5_portal*.html 与原文件逐字节一致, 只改了文件名)
extern const char index_html_start[] asm("_binary_c5_portal_html_start");
extern const char done_html_start[] asm("_binary_c5_portal_done_html_start");

void C5ProvisionPortal::Start(const std::string& ssid_prefix) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char buf[40];
    snprintf(buf, sizeof(buf), "%s-%02X%02X", ssid_prefix.c_str(), mac[4], mac[5]);
    ssid_ = buf;

    StartAccessPoint();
    StartWebServer();

    ESP_LOGW(TAG, "provisioning portal up: SSID=%s url=%s", ssid_.c_str(), url().c_str());
}

void C5ProvisionPortal::StartAccessPoint() {
    // S3 在 C5 网络模式下从未初始化过 esp_wifi, 这里是第一次也是唯一一次。
    // 容忍 INVALID_STATE: 事件循环可能已由 app 主流程创建。
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(ret);
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(ret);

    ap_netif_ = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif_);
    esp_netif_set_ip_info(ap_netif_, &ip_info);
    esp_netif_dhcps_start(ap_netif_);
    dns_server_.Start(ip_info.gw);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ssid_.c_str(), sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = ssid_.length();
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    // 显式固定信道。S3 这颗射频只做热点、不做 STA, 没有任何东西会把它拖走 ——
    // 这正是整套方案成立的前提: 手机的关联从头到尾稳定。
    wifi_config.ap.channel = 1;

    // 纯 AP 模式: S3 不需要连任何 WiFi, 上网由 C5 负责。
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// 高级选项存在 S3 自己的 NVS 上。
// 旧流程里 OTA 地址存在 C5 (配网页在那边), S3 要靠 SyncOtaUrlFromC5 拉回来;
// 现在配网页就在 S3, 直接本地读写, 那套同步逻辑不再需要。
static std::string LoadStr(const char* key) {
    char v[256] = {0};
    size_t len = sizeof(v);
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) return "";
    esp_err_t err = nvs_get_str(nvs, key, v, &len);
    nvs_close(nvs);
    return err == ESP_OK ? std::string(v) : std::string();
}

void C5ProvisionPortal::StartWebServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.lru_purge_enable = true;
    // /submit 会同步等 C5 验证 (最长 40s), 别让 httpd 提前判超时
    config.recv_wait_timeout = 45;
    config.send_wait_timeout = 45;
    ESP_ERROR_CHECK(httpd_start(&server_, &config));

    // ---- 配网首页 (小智原版页面, 未改动) ----
    httpd_uri_t index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            httpd_resp_set_type(req, "text/html");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &index));

    httpd_uri_t done = {
        .uri = "/done.html",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            httpd_resp_set_type(req, "text/html");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, done_html_start, HTTPD_RESP_USE_STRLEN);
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &done));

    // ---- SSID 列表: 来自 C5, S3 的 2.4G 射频看不见 5G AP ----
    httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            auto* self = static_cast<C5ProvisionPortal*>(req->user_ctx);
            std::vector<C5ApInfo> aps;
            self->bridge_.RequestScan(aps, 15000);

            cJSON* arr = cJSON_CreateArray();
            for (const auto& ap : aps) {
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "ssid", ap.ssid.c_str());
                cJSON_AddNumberToObject(o, "rssi", ap.rssi);
                cJSON_AddNumberToObject(o, "authmode", ap.authmode);
                cJSON_AddItemToArray(arr, o);
            }
            char* json = cJSON_PrintUnformatted(arr);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            esp_err_t r = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
            cJSON_free(json);
            cJSON_Delete(arr);
            return r;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &scan));

    // ---- 已保存列表 ----
    // 凭据存在 C5 的 NVS 上, S3 这边没有。目前回空列表: 页面在列表为空时会
    // 自动隐藏"已保存的 WiFi"整块, 不影响使用。
    // (进入配网页的前提本来就是 C5 没有可用凭据, 且长按重置会清空全部凭据,
    //  所以这一块在当前流程里基本用不到。要恢复需再加一组查询/删除帧。)
    httpd_uri_t saved_list = {
        .uri = "/saved/list",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &saved_list));

    // ---- 提交凭据: 同步等 C5 验证完再回 ----
    // 敢同步阻塞是因为热点在 S3 自己的射频上, C5 怎么折腾都不会影响手机与
    // 本连接。原版页面正是这么设计的, 现在终于能按它的本意工作。
    httpd_uri_t submit = {
        .uri = "/submit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            auto* self = static_cast<C5ProvisionPortal*>(req->user_ctx);

            if (req->content_len == 0 || req->content_len > 1024) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
                return ESP_FAIL;
            }
            std::vector<char> buf(req->content_len + 1, 0);
            int got = httpd_req_recv(req, buf.data(), req->content_len);
            if (got <= 0) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
                return ESP_FAIL;
            }

            cJSON* json = cJSON_Parse(buf.data());
            if (!json) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
                return ESP_FAIL;
            }
            cJSON* j_ssid = cJSON_GetObjectItem(json, "ssid");
            cJSON* j_pwd  = cJSON_GetObjectItem(json, "password");
            std::string ssid = cJSON_IsString(j_ssid) ? j_ssid->valuestring : "";
            std::string pwd  = cJSON_IsString(j_pwd)  ? j_pwd->valuestring  : "";
            cJSON_Delete(json);

            std::string error;
            /* C5 那边的最坏路径: 缓存未命中先补扫 5G (~4s) + 关联 12s + DHCP 12s。
             * 留到 40s 才不会在 C5 还在等 DHCP 时就先报"模块无响应"。 */
            bool ok = self->bridge_.SendWifiConfig(ssid, pwd, error, 40000);

            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            if (ok) {
                self->provisioned_ = true;
                httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
                // 页面收到 success 后会跳 /done.html, 那边倒计时 3 秒再 POST /reboot。
                // 这里另起一个兜底定时: 万一用户直接关了页面, 设备也要自己走完。
                xTaskCreate([](void* ctx) {
                    vTaskDelay(pdMS_TO_TICKS(20000));
                    auto* self = static_cast<C5ProvisionPortal*>(ctx);
                    ESP_LOGW(TAG, "reboot fallback fired (page never called /reboot)");
                    self->RebootBoth();
                }, "prov_fallback", 4096, self, 5, nullptr);
            } else {
                // 失败: 热点和页面都还在, 用户直接改密码重试
                cJSON* resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "error", error.c_str());
                char* s = cJSON_PrintUnformatted(resp);
                httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
                cJSON_free(s);
                cJSON_Delete(resp);
            }
            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &submit));

    // ---- 高级选项 (OTA 地址 / 发射功率 / 记住 BSSID), 存在 S3 的 NVS ----
    httpd_uri_t adv_get = {
        .uri = "/advanced/config",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ota_url", LoadStr("ota_url").c_str());
            nvs_handle_t nvs;
            if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
                int8_t power = 0;
                if (nvs_get_i8(nvs, "max_tx_power", &power) == ESP_OK) {
                    cJSON_AddNumberToObject(o, "max_tx_power", power);
                }
                uint8_t remember = 0;
                if (nvs_get_u8(nvs, "remember_bssid", &remember) == ESP_OK) {
                    cJSON_AddBoolToObject(o, "remember_bssid", remember != 0);
                }
                nvs_close(nvs);
            }
            char* s = cJSON_PrintUnformatted(o);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
            cJSON_free(s);
            cJSON_Delete(o);
            return r;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &adv_get));

    httpd_uri_t adv_submit = {
        .uri = "/advanced/submit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            if (req->content_len == 0 || req->content_len > 1024) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
                return ESP_FAIL;
            }
            std::vector<char> buf(req->content_len + 1, 0);
            if (httpd_req_recv(req, buf.data(), req->content_len) <= 0) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
                return ESP_FAIL;
            }
            cJSON* json = cJSON_Parse(buf.data());
            if (!json) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
                return ESP_FAIL;
            }

            nvs_handle_t nvs;
            if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
                cJSON* j = cJSON_GetObjectItem(json, "ota_url");
                if (cJSON_IsString(j)) nvs_set_str(nvs, "ota_url", j->valuestring);
                j = cJSON_GetObjectItem(json, "max_tx_power");
                if (cJSON_IsNumber(j)) nvs_set_i8(nvs, "max_tx_power", (int8_t)j->valueint);
                j = cJSON_GetObjectItem(json, "remember_bssid");
                if (cJSON_IsBool(j)) nvs_set_u8(nvs, "remember_bssid", cJSON_IsTrue(j) ? 1 : 0);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
            cJSON_Delete(json);

            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &adv_submit));

    // ---- done 页面倒计时结束后调用 ----
    httpd_uri_t reboot = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            auto* self = static_cast<C5ProvisionPortal*>(req->user_ctx);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
            xTaskCreate([](void* ctx) {
                vTaskDelay(pdMS_TO_TICKS(300));   // 等响应发完
                static_cast<C5ProvisionPortal*>(ctx)->RebootBoth();
            }, "prov_reboot", 4096, self, 5, nullptr);
            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &reboot));

    // ---- captive portal: 各系统的联网探测地址统一重定向到配网页 ----
    static const char* kProbeUris[] = {
        "/hotspot-detect.html", "/generate_204", "/gen_204",
        "/mobile/status.php", "/check_network_status.txt",
        "/ncsi.txt", "/connectivity-check.html", "/success.txt",
    };
    for (const char* uri : kProbeUris) {
        httpd_uri_t probe = {
            .uri = uri,
            .method = HTTP_GET,
            .handler = [](httpd_req_t* req) -> esp_err_t {
                httpd_resp_set_status(req, "302 Found");
                httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
                httpd_resp_set_hdr(req, "Connection", "close");
                return httpd_resp_send(req, NULL, 0);
            },
            .user_ctx = this
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &probe));
    }
}

// 幂等: /reboot 和兜底定时可能都触发。
void C5ProvisionPortal::RebootBoth() {
    bool expected = false;
    if (!rebooting_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!provisioned_) {
        ESP_LOGW(TAG, "reboot requested before a successful provision, ignored");
        return;
    }
    ESP_LOGW(TAG, "provisioned, rebooting C5 then S3");
    bridge_.SendFrame(BRIDGE_CMD_RESET, BRIDGE_NO_LINK, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(300));   // 等 C5 收到复位帧
    esp_restart();
}
