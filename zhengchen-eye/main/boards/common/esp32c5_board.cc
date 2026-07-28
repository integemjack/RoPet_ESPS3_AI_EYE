#include "esp32c5_board.h"

#include "application.h"
#include "display.h"
#include "font_awesome_symbols.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>

static const char *TAG = "Esp32C5Board";

Esp32C5Board::Esp32C5Board(gpio_num_t tx_pin, gpio_num_t rx_pin, size_t rx_buffer_size,
                           gpio_num_t rts_pin, gpio_num_t cts_pin)
    : bridge_(tx_pin, rx_pin, rx_buffer_size), rts_pin_(rts_pin), cts_pin_(cts_pin) {
    network_ = std::make_unique<C5Network>(bridge_);
}

std::string Esp32C5Board::GetBoardType() {
    return "esp32c5";
}

void Esp32C5Board::StartNetwork() {
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::DETECTING_MODULE);

    // 若板级配置了流控引脚, 在 Start 前启用 (需 C5 端也启用并接好 RTS/CTS)
    if (rts_pin_ != GPIO_NUM_NC && cts_pin_ != GPIO_NUM_NC) {
        bridge_.SetFlowControlPins(rts_pin_, cts_pin_);
        ESP_LOGI(TAG, "UART flow control enabled: rts=%d cts=%d", rts_pin_, cts_pin_);
    }

    // 启动 UART + 接收任务; C5 上电后会自主连 WiFi / 开热点配网
    bridge_.Start(921600);

    WaitForNetworkReady();
}

void Esp32C5Board::WaitForNetworkReady() {
    auto& application = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);

    // 等 C5 上线, 或等它明确说"我没凭据"。后者会立刻返回, 不必空等满 120 秒。
    if (bridge_.WaitForNetworkOrProvision(120000)) {
        ESP_LOGI(TAG, "C5 WiFi ready, ip=%s rssi=%d band=%dG",
                 bridge_.GetIpAddress().c_str(), bridge_.GetRssi(),
                 bridge_.GetBand() == 2 ? 5 : 2);
        return;
    }

    if (bridge_.needs_provision()) {
        EnterProvisioningMode();
        return;   // 不会返回, EnterProvisioningMode 内部常驻
    }

    application.Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "sad", Lang::Sounds::P3_ERR_REG);
}

// 用 S3 自己那颗闲置的 2.4G 射频开配网热点。
// 关键在于 S3 只做 AP、不做 STA, 信道固定, 手机全程不掉线; 真正的连接验证
// 交给 C5 的射频去做, 两者互不干扰 —— 详见 c5_provision_portal.h。
void Esp32C5Board::EnterProvisioningMode() {
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    portal_ = std::make_unique<C5ProvisionPortal>(bridge_);
    portal_->Start("Xiaozhi");

    std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
    hint += portal_->ssid();
    hint += Lang::Strings::ACCESS_VIA_BROWSER;
    hint += portal_->url();
    hint += "\n\n";
    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "",
                      Lang::Sounds::P3_WIFICONFIG);

    // 配网完成后由 /submit 的处理流程重启双方, 这里只需常驻
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// 规范化用户在配网页填写的 OTA 地址:
//  1. 去除首尾空白 (含 \r \n \t 空格)
//  2. 若地址非空、且不含 query('?')/fragment('#')、且末尾没有 '/', 则自动补 '/'
static std::string NormalizeOtaUrl(const std::string& raw) {
    size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = raw.find_last_not_of(" \t\r\n");
    std::string url = raw.substr(b, e - b + 1);

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
    auto display = Board::GetInstance().GetDisplay();
    std::string ota_url;
    int attempt = 0;
    while (!bridge_.FetchOtaUrl(ota_url, 5000)) {
        attempt++;
        if (bridge_.PingC5(1000)) {
            ESP_LOGW(TAG, "ota_url fetch timeout but C5 is ALIVE (PONG ok), "
                          "likely a dropped frame, retry #%d ...", attempt);
        } else {
            ESP_LOGE(TAG, "ota_url fetch timeout and C5 NOT responding to PING, "
                          "check wiring / C5 firmware. retry #%d ...", attempt);
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
        settings.SetString("ota_url", "");
        ESP_LOGI(TAG, "C5 has no ota_url, cleared S3 setting (use default)");
    }
}

void Esp32C5Board::ResetWifiConfiguration() {
    auto display = Board::GetInstance().GetDisplay();
    display->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    // WiFi 凭据保存在 C5 的 NVS 里, S3 本地没有可删的东西, 只能下发命令。
    if (bridge_.ResetWifiCredentials(3000)) {
        ESP_LOGW(TAG, "C5 acked wifi reset, rebooting S3");
    } else {
        // 没等到应答: C5 可能没跑新固件, 或串口异常。用 PING 区分这两种情况,
        // 便于现场判断。无论哪种都继续重启, 避免卡在一个半死的网络状态。
        ESP_LOGE(TAG, "C5 did not ack wifi reset (alive=%d), rebooting S3 anyway",
                 bridge_.PingC5(1000) ? 1 : 0);
    }

    // C5 重启后会开配网热点; S3 也重启, 重新走 StartNetwork 等它上线。
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

NetworkInterface* Esp32C5Board::GetNetwork() {
    return network_.get();
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
