#include "dual_network_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include <esp_log.h>

static const char *TAG = "DualNetworkBoard";

DualNetworkBoard::DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin, size_t ml307_rx_buffer_size, int32_t default_net_type) 
    : Board(), 
      ml307_tx_pin_(ml307_tx_pin), 
      ml307_rx_pin_(ml307_rx_pin), 
      ml307_rx_buffer_size_(ml307_rx_buffer_size) {
    
    // 从Settings加载网络类型
    network_type_ = LoadNetworkTypeFromSettings(default_net_type);
    
    // 只初始化当前网络类型对应的板卡
    InitializeCurrentBoard();
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    Settings settings("network", true);
    int network_type = settings.GetInt("type", default_net_type);
    // 0 = WiFi(S3自带), 1 = ML307(4G), 2 = C5(WiFi网桥,含5G)
    switch (network_type) {
        case 1:  return NetworkType::ML307;
        case 2:  return NetworkType::C5;
        default: return NetworkType::WIFI;
    }
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type;
    switch (type) {
        case NetworkType::ML307: network_type = 1; break;
        case NetworkType::C5:    network_type = 2; break;
        default:                 network_type = 0; break;
    }
    settings.SetInt("type", network_type);
}

void DualNetworkBoard::InitializeCurrentBoard() {
    if (network_type_ == NetworkType::ML307) {
        ESP_LOGI(TAG, "Initialize ML307 board");
        current_board_ = std::make_unique<Ml307Board>(ml307_tx_pin_, ml307_rx_pin_, ml307_rx_buffer_size_);
    } else if (network_type_ == NetworkType::C5) {
        ESP_LOGI(TAG, "Initialize ESP32-C5 WiFi bridge board");
        // C5 与 ML307 共用同一组 UART 引脚 (互斥使用)
        current_board_ = std::make_unique<Esp32C5Board>(ml307_tx_pin_, ml307_rx_pin_, ml307_rx_buffer_size_);
    } else {
        ESP_LOGI(TAG, "Initialize WiFi board");
        current_board_ = std::make_unique<WifiBoard>();
    }
}

void DualNetworkBoard::SwitchNetworkType() {
    auto display = GetDisplay();
    // 三态循环: WIFI(S3自带) -> ML307(4G) -> C5(WiFi网桥,含5G) -> WIFI
    switch (network_type_) {
        case NetworkType::WIFI:
            SaveNetworkTypeToSettings(NetworkType::ML307);
            display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
            break;
        case NetworkType::ML307:
            SaveNetworkTypeToSettings(NetworkType::C5);
            // C5 也是 WiFi, 复用 WiFi 切换通知
            display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
            break;
        case NetworkType::C5:
        default:
            SaveNetworkTypeToSettings(NetworkType::WIFI);
            display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
            break;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    auto& app = Application::GetInstance();
    app.Reboot();
}

 
std::string DualNetworkBoard::GetBoardType() {
    return current_board_->GetBoardType();
}

void DualNetworkBoard::StartNetwork() {
    auto display = Board::GetInstance().GetDisplay();
    
    if (network_type_ == NetworkType::ML307) {
        display->SetStatus(Lang::Strings::DETECTING_MODULE);
        auto& app = Application::GetInstance();
        app.PlaySound(Lang::Sounds::P3_4G_MODE);
        vTaskDelay(pdMS_TO_TICKS(2000));
        app.ResetDecoder();
        app.PlaySound(Lang::Sounds::P3_NETING);
    } else {
        // WIFI (S3自带) 和 C5(WiFi网桥) 都按 WiFi 模式处理
        display->SetStatus(Lang::Strings::CONNECTING);
        auto& app = Application::GetInstance();
        app.PlaySound(Lang::Sounds::P3_WIFI_MODE);
        vTaskDelay(pdMS_TO_TICKS(2000));
        app.ResetDecoder();
        app.PlaySound(Lang::Sounds::P3_NETING);
    }
    current_board_->StartNetwork();
}

Http* DualNetworkBoard::CreateHttp() {
    return current_board_->CreateHttp();
}

WebSocket* DualNetworkBoard::CreateWebSocket() {
    return current_board_->CreateWebSocket();
}

Mqtt* DualNetworkBoard::CreateMqtt() {
    return current_board_->CreateMqtt();
}

Udp* DualNetworkBoard::CreateUdp() {
    return current_board_->CreateUdp();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return current_board_->GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveMode(bool enabled) {
    current_board_->SetPowerSaveMode(enabled);
}

std::string DualNetworkBoard::GetBoardJson() {   
    return current_board_->GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return current_board_->GetDeviceStatusJson();
}
