#include "dual_network_board.h"
#include "audio_codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "led/single_led.h"
#include "iot/thing_manager.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>
#include "esp_lvgl_port.h"
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include "../zhengchen-1.54tft-wifi/power_manager.h"
#include "driver/touch_pad.h"

#include "esp_random.h"

#define TAG "XiaoZhiEyeBoard"

LV_FONT_DECLARE(font_puhui_20_4);   
LV_FONT_DECLARE(font_awesome_20_4); 


class XiaoZhiEyeBoard : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    LcdDisplay* display_;
    uint32_t touch_value = 0;
    uint32_t touch_value1 = 0;
    PowerManager* power_manager_ = new PowerManager(GPIO_NUM_7);
    
   

// /* =================================*/
    void touch_init() {
        touch_pad_init();
        touch_pad_config(TOUCH_PAD_NUM4); // 配置 GPIO4 为触摸引脚
        touch_pad_config(TOUCH_PAD_NUM5); // 配置 GPIO5 为触摸引脚
        touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER); // 设置 FSM 模式为定时器模式
        touch_pad_fsm_start();
        vTaskDelay(40 / portTICK_PERIOD_MS);
    }


    static void touch_read_task(void* arg) {
        XiaoZhiEyeBoard* self = static_cast<XiaoZhiEyeBoard*>(arg);
        auto& app = Application::GetInstance();
        while (1) {
            touch_pad_read_raw_data(TOUCH_PAD_NUM4, &self->touch_value);
            touch_pad_read_raw_data(TOUCH_PAD_NUM5, &self->touch_value1);
            if (self->touch_value > 30000) {
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    app.WakeWordInvoke("(正在抚摸你的头，请提供相关的情绪价值，回答)");
                }
            }

            if (self->touch_value1 > 30000) {
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    app.WakeWordInvoke("(正在抚摸你的身体，请提供相关的情绪价值，回答)");
                }
            }
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    }

    
    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

    }

     
    
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (GetNetworkType() == NetworkType::WIFI) {
                if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                    // cast to WifiBoard
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.ResetWifiConfiguration();
                }
            }
            app.ToggleChatState();
        });

        boot_button_.OnMultipleClick([this]() {
            SwitchNetworkType();
        });

        boot_button_.OnLongPress([this]() {
            if (GetNetworkType() == NetworkType::WIFI) {
                auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                wifi_board.ResetWifiConfiguration();
            }
            
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            
        });
#endif
    }

   

public:

  // 第4个参数 default_net_type: 0=WiFi(S3自带) 1=ML307(4G) 2=C5(WiFi网桥,含5G)
  // 默认走 C5, 开机即用外接 C5 的 5G WiFi
  XiaoZhiEyeBoard() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, 4096, 2),boot_button_(BOOT_BUTTON_GPIO) {
    InitializeI2c();
    InitializeButtons();
    touch_init();
    GetAudioCodec()->SetOutputVolume(100);
    xTaskCreate(touch_read_task, "touch_read_task", 2048, this, 5, NULL);
}


    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }


    

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging)  override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            last_discharging = discharging;
        }
        level = std::max<uint32_t>(power_manager_->GetBatteryLevel(), 0);
        return true;
    }
    
    virtual bool GetTemperature(float& esp32temp)  override {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }

    virtual bool Gethead_value(uint32_t& head_value)  override {
        head_value = touch_value;
        printf("Touch1 value: %ld\n", touch_value);
        return true;
    }

    virtual bool Getbody_value(uint32_t& body_value)  override {
        body_value = touch_value1;
        printf("Touch2 value: %ld\n", touch_value1);
        return true;
    }
};

DECLARE_BOARD(XiaoZhiEyeBoard);