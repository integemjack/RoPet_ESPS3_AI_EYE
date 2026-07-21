#include "esp32c5_board.h"

#include "application.h"
#include "display.h"
#include "font_awesome_symbols.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <web_socket.h>
#include <cJSON.h>

#include "c5_transport.h"
#include "c5_udp.h"
#include "c5_mqtt.h"
#include "c5_http.h"

static const char *TAG = "Esp32C5Board";

Esp32C5Board::Esp32C5Board(gpio_num_t tx_pin, gpio_num_t rx_pin, size_t rx_buffer_size)
    : bridge_(tx_pin, rx_pin, rx_buffer_size) {
}

std::string Esp32C5Board::GetBoardType() {
    return "esp32c5";
}

void Esp32C5Board::StartNetwork() {
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::DETECTING_MODULE);

    // 启动 UART + 接收任务; C5 上电后会自主连 WiFi / 开热点配网
    bridge_.Start(921600);

    WaitForNetworkReady();
}

void Esp32C5Board::WaitForNetworkReady() {
    auto& application = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);

    // 等 C5 完成 WiFi 连接 (含首次配网, 给足时间)
    if (!bridge_.WaitForNetworkReady(120000)) {
        application.Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "sad", Lang::Sounds::P3_ERR_REG);
        return;
    }
    ESP_LOGI(TAG, "C5 WiFi ready, ip=%s rssi=%d band=%dG",
             bridge_.GetIpAddress().c_str(), bridge_.GetRssi(),
             bridge_.GetBand() == 2 ? 5 : 2);

    // 从 C5 配网页取回单独配置的 OTA 地址, 同步到 S3 自己的 NVS。
    // Ota::GetCheckVersionUrl() 读的是 S3 的 Settings("wifi").ota_url,
    // 而配网页把地址存在 C5 的 NVS, 所以必须在这里把它搬到 S3 侧。
    SyncOtaUrlFromC5();
}

// 规范化用户在配网页填写的 OTA 地址:
//  1. 去除首尾空白 (含 \r \n \t 空格)
//  2. 若地址非空、且不含 query('?')/fragment('#')、且末尾没有 '/', 则自动补 '/'
//     (小智 OTA 地址是目录形式, 末尾必须带 '/', 用户常漏填)
static std::string NormalizeOtaUrl(const std::string& raw) {
    // trim
    size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = raw.find_last_not_of(" \t\r\n");
    std::string url = raw.substr(b, e - b + 1);

    // 带查询参数/锚点的地址不动, 避免破坏 URL 语义
    if (url.find('?') != std::string::npos || url.find('#') != std::string::npos) {
        return url;
    }
    if (!url.empty() && url.back() != '/') {
        url += '/';
    }
    return url;
}

void Esp32C5Board::SyncOtaUrlFromC5() {
    // 获取失败(通信超时)就一直重试, 直到 C5 明确返回结果为止。
    // 注意: FetchOtaUrl 返回 true 表示收到有效响应(即使是空串, 代表该机未配置);
    //       返回 false 表示超时, 需要继续重试。
    auto display = Board::GetInstance().GetDisplay();
    std::string ota_url;
    int attempt = 0;
    while (!bridge_.FetchOtaUrl(ota_url, 5000)) {
        attempt++;
        // 探活: 区分"C5 完全没响应(接线/固件问题)"与"C5 活着但 OTA 帧偶发丢失"
        if (bridge_.PingC5(1000)) {
            ESP_LOGW(TAG, "ota_url fetch timeout but C5 is ALIVE (PONG ok), "
                          "likely a dropped frame, retry #%d ...", attempt);
        } else {
            ESP_LOGE(TAG, "ota_url fetch timeout and C5 NOT responding to PING, "
                          "check wiring / C5 firmware. retry #%d ...", attempt);
            // 无响应多半是 C5 未就绪/接线问题, 给现场一个可见提示
            display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ota_url = NormalizeOtaUrl(ota_url);

    Settings settings("wifi", true);
    if (!ota_url.empty()) {
        settings.SetString("ota_url", ota_url);
        ESP_LOGI(TAG, "synced ota_url from C5 to S3 NVS: %s", ota_url.c_str());
    } else {
        // C5 上未配置 OTA 地址: 清掉 S3 上可能残留的旧值, 回落到编译期默认
        settings.SetString("ota_url", "");
        ESP_LOGI(TAG, "C5 has no ota_url, cleared S3 setting (use default)");
    }
}

Http* Esp32C5Board::CreateHttp() {
    return new C5Http(bridge_);
}

WebSocket* Esp32C5Board::CreateWebSocket() {
    // WebSocket(Transport*): wss 用 TLS 传输, ws 用明文 TCP 传输。
    // xiaozhi 服务端多为 wss, 这里默认使用 TLS 传输。
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    bool tls = (url.rfind("wss://", 0) == 0);
    return new WebSocket(new C5Transport(bridge_, tls));
}

Mqtt* Esp32C5Board::CreateMqtt() {
    return new C5Mqtt(bridge_);
}

Udp* Esp32C5Board::CreateUdp() {
    return new C5Udp(bridge_);
}

const char* Esp32C5Board::GetNetworkStateIcon() {
    if (!bridge_.network_ready()) {
        return FONT_AWESOME_WIFI_OFF;
    }
    int rssi = bridge_.GetRssi();
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string Esp32C5Board::GetBoardJson() {
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    board_json += "\"rssi\":" + std::to_string(bridge_.GetRssi()) + ",";
    board_json += "\"band\":\"" + std::string(bridge_.GetBand() == 2 ? "5G" : "2.4G") + "\",";
    board_json += "\"ip\":\"" + bridge_.GetIpAddress() + "\"}";
    return board_json;
}

void Esp32C5Board::SetPowerSaveMode(bool enabled) {
    // C5 侧可扩展省电; 暂不处理
}

std::string Esp32C5Board::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) {
        cJSON_AddStringToObject(screen, "theme", display->GetTheme().c_str());
    }
    cJSON_AddItemToObject(root, "screen", screen);

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "band", bridge_.GetBand() == 2 ? "5G" : "2.4G");
    int rssi = bridge_.GetRssi();
    if (rssi >= -60) {
        cJSON_AddStringToObject(network, "signal", "strong");
    } else if (rssi >= -70) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else {
        cJSON_AddStringToObject(network, "signal", "weak");
    }
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
