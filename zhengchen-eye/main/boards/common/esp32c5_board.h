#ifndef ESP32C5_BOARD_H
#define ESP32C5_BOARD_H

#include <memory>

#include "board.h"
#include "c5_bridge.h"
#include "c5_motion.h"
#include "c5_network.h"
#include "c5_provision_portal.h"

// 通过 UART 外接 ESP32-C5 WiFi 网桥的板卡。
// 与 Ml307Board 平行: C5 负责 WiFi(含5G)+配网+socket 透传, S3 通过帧协议驱动。
// 网络能力通过 GetNetwork() 返回的 C5Network (NetworkInterface) 暴露给上层。
class Esp32C5Board : public Board {
protected:
    C5Bridge bridge_;
    std::unique_ptr<C5Network> network_;
    std::unique_ptr<C5Motion> motion_;

    std::unique_ptr<C5ProvisionPortal> portal_;

    virtual std::string GetBoardJson() override;
    void WaitForNetworkReady();
    // C5 报告无可用凭据时, 用 S3 的 2.4G 射频开配网热点 (不会返回)
    void EnterProvisioningMode();
    void SyncOtaUrlFromC5();

    // 硬件流控引脚 (可选, 默认禁用)。填有效 GPIO 且两端都启用才生效。
    gpio_num_t rts_pin_ = GPIO_NUM_NC;
    gpio_num_t cts_pin_ = GPIO_NUM_NC;

public:
    Esp32C5Board(gpio_num_t tx_pin, gpio_num_t rx_pin, size_t rx_buffer_size = 8192,
                 gpio_num_t rts_pin = GPIO_NUM_NC, gpio_num_t cts_pin = GPIO_NUM_NC);
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;

    // 舵机(四肢+尾巴)挂在 C5 上, 通过同一条 UART 下发动作指令。
    // 注意 UART 要等 StartNetwork() 里的 bridge_.Start() 之后才通, 在那之前
    // 发帧只会石沉大海 —— 板级代码不要在构造函数里就播动作。
    C5Motion* GetMotion() { return motion_.get(); }

    // 重置配网: 与 WifiBoard::ResetWifiConfiguration 语义对齐, 但凭据存在 C5 上,
    // 所以是发帧让 C5 清除并重启, S3 随后一起重启等待 C5 的配网热点。
    virtual void ResetWifiConfiguration();
};

#endif // ESP32C5_BOARD_H
