#ifndef ESP32C5_BOARD_H
#define ESP32C5_BOARD_H

#include "board.h"
#include "c5_bridge.h"

// 通过 UART 外接 ESP32-C5 WiFi 网桥的板卡。
// 与 Ml307Board 平行: C5 负责 WiFi(含5G)+配网+socket 透传, S3 通过帧协议驱动。
class Esp32C5Board : public Board {
protected:
    C5Bridge bridge_;

    virtual std::string GetBoardJson() override;
    void WaitForNetworkReady();

public:
    Esp32C5Board(gpio_num_t tx_pin, gpio_num_t rx_pin, size_t rx_buffer_size = 8192);
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual Http* CreateHttp() override;
    virtual WebSocket* CreateWebSocket() override;
    virtual Mqtt* CreateMqtt() override;
    virtual Udp* CreateUdp() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
};

#endif // ESP32C5_BOARD_H
