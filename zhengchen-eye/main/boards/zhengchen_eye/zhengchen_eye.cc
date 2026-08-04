#include "dual_network_board.h"
#include "display/lcd_display.h"
#include "audio/codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "config.h"
#include "assets/lang_config.h"
#include "font_awesome_symbols.h"

#include <cstring>

#include <esp_lcd_panel_vendor.h>
#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include "power_save_timer.h"
#include "../zhengchen-1.54tft-wifi/power_manager.h"
#include "driver/touch_pad.h"
#include "device_state_event.h"
#include "eye_motion.h"

#if CONFIG_USE_LCD_240X240_GIF1 || CONFIG_USE_LCD_240X240_GIF2
#include <esp_lcd_gc9a01.h>
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};

#else
#include "esp_lcd_gc9d01n.h"
#endif

#define TAG "zhengchen_eye"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);

// 在显示层拦情绪消息, 顺手驱动四肢/尾巴。
// 走子类而不是改 application.cc: 情绪的派发在核心代码里 (application.cc 的
// "llm" 分支), 那是上游小智的文件, 改了以后每次同步上游都要处理冲突。
// SetEmotion 是虚函数, 覆写它零侵入。
class EyeLcdDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;

    void SetEmotion(const char* emotion) override {
        SpiLcdDisplay::SetEmotion(emotion);
        EyeMotion::GetInstance().OnEmotion(emotion);
    }

    // 注意: 这里刻意不再拦 SetChatMessage 做本地关键词识别。动作指令一律走
    // 服务端 LLM 的 MCP 工具调用 (self.pet.*), 触发词在服务端配置, 改词不用
    // 重新编译固件。
};


class zhengchen_eye : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    LcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    uint32_t touch_value = 0;
    uint32_t touch_value1 = 0;


    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_7);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
                ESP_LOGW(TAG, "Charging, disable power save timer");
            } else {
                power_save_timer_->SetEnabled(true);
                ESP_LOGW(TAG, "Not charging, enable power save timer");
            }
        });
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(240, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
         
        power_save_timer_->SetEnabled(true);
    }

    void touch_init() {
        touch_pad_init();
        touch_pad_config(TOUCH_PAD_NUM4); // 配置 GPIO4 为触摸引脚
        touch_pad_config(TOUCH_PAD_NUM5); // 配置 GPIO5 为触摸引脚
        touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER); // 设置 FSM 模式为定时器模式
        touch_pad_fsm_start();
        vTaskDelay(40 / portTICK_PERIOD_MS);
    }

    static void touch_read_task(void* arg) {
        zhengchen_eye* self = static_cast<zhengchen_eye*>(arg);
        auto& app = Application::GetInstance();
        while (1) {
            touch_pad_read_raw_data(TOUCH_PAD_NUM4, &self->touch_value);
            touch_pad_read_raw_data(TOUCH_PAD_NUM5, &self->touch_value1);
            // printf("DEBUG RAW -> Head(4): %ld, Body(5): %ld\n", self->touch_value, self->touch_value1);
            if (self->touch_value > 30000) {
                printf("DEBUG RAW -> Left(4): %ld, Right(5): %ld\n", self->touch_value, self->touch_value1);
                // if (app.GetDeviceState() == kDeviceStateIdle) {
                if (app.GetDeviceState() == kDeviceStateIdle || app.GetDeviceState() == kDeviceStateListening) {
                    // 动作先出: 物理交互的反馈要立刻, 不能等 LLM 一个来回。
                    EyeMotion::GetInstance().OnTouch(/*left=*/true);
                    // app.WakeWordInvoke("摸了头，回答情绪");
                    app.WakeWordInvoke("摸了左头，情绪化回应");
                }
                // else if (app.GetDeviceState() == kDeviceStateListening) {
                //     app.WakeWordInvoke("正在抚摸你的头，给出情绪化回答");
                // }
            }

            if (self->touch_value1 > 30000) {
                printf("DEBUG RAW -> Left(4): %ld, Right(5): %ld\n", self->touch_value, self->touch_value1);
                // if (app.GetDeviceState() == kDeviceStateIdle) {
                if (app.GetDeviceState() == kDeviceStateIdle || app.GetDeviceState() == kDeviceStateListening) {
                    EyeMotion::GetInstance().OnTouch(/*left=*/false);
                    // app.WakeWordInvoke("正在抚摸你的身体，给出情绪化回答");
                    // app.WakeWordInvoke("摸了身体，回答情绪");
                    app.WakeWordInvoke("摸了右头，情绪化回应");
                }
            }
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    }
   
    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
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
        // [已禁用] 多击切换网络类型 (WIFI / 4G / C5)。
        //
        // 本机固定走 C5 网桥, 且配网热点跑在 S3 自己的 2.4G 射频上。误触切到
        // WIFI 模式会让那颗射频改去做 STA, 与配网热点冲突; 切到 4G 模式则整个
        // C5 链路失效。这两种情况用户都只能靠再次多击摸回来, 代价远大于收益,
        // 因此不再绑定该手势。需要切换时改 NVS 的 network.type 即可
        // (0=WIFI, 1=ML307/4G, 2=C5), 见 DualNetworkBoard::LoadNetworkTypeFromSettings。
        //
        // boot_button_.OnMultipleClick([this]() { SwitchNetworkType(); });
        boot_button_.OnLongPress([this]() {
            // WIFI 模式清本机凭据; C5 模式发帧让网桥清它自己的凭据。两者都会重启进配网。
            ESP_LOGW(TAG, "boot button: long press -> reset wifi configuration");
            ResetWifiConfiguration();
        });

#if CONFIG_USE_DEVICE_AEC
        ESP_LOGW(TAG, "4GGGGGGGG");
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            
        });
#endif
    }


    // 舵机挂在 C5 上, 只有当前网络模式真的是 C5 时链路才存在。
    // 用 static_cast 而不是 dynamic_cast: IDF 默认 -fno-rtti, 且这里的类型
    // 已经由 GetNetworkType() 判定, 与本文件 InitializeButtons() 里对
    // WifiBoard 的处理方式一致。
    C5Motion* ResolveMotion() {
        if (GetNetworkType() != NetworkType::C5) {
            return nullptr;
        }
        return static_cast<Esp32C5Board&>(GetCurrentBoard()).GetMotion();
    }

    void InitializeMotion() {
        auto& eye_motion = EyeMotion::GetInstance();

        // 惰性解析: UART 要等 StartNetwork() 里的 bridge_.Start() 之后才通,
        // 而这里还在板卡构造期。传闭包进去, 用的时候再取。
        eye_motion.Initialize([this]() { return ResolveMotion(); });
        eye_motion.RegisterMcpTools();

        DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
            [](DeviceState previous, DeviceState current) {
                EyeMotion::GetInstance().OnDeviceStateChanged(previous, current);
            });
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeGc9107Display(){
       
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = -1;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

#if CONFIG_USE_LCD_240X240_GIF1 || CONFIG_USE_LCD_240X240_GIF2
       // 初始化液晶屏驱动芯片
       ESP_LOGD(TAG, "Install LCD driver");
       esp_lcd_panel_dev_config_t panel_config = {};
       panel_config.reset_gpio_num =DISPLAY_RES;
       panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
       panel_config.bits_per_pixel = 16;
      
       ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
       gc9a01_vendor_config_t gc9107_vendor_config = {
           .init_cmds = gc9107_lcd_init_cmds,
           .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
       };       

       esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        panel_config.vendor_config = &gc9107_vendor_config;
#else
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#endif       
         
        display_ = new EyeLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
            {
                .text_font = &font_puhui_16_4,
                .icon_font = &font_awesome_16_4,
                .emoji_font = DISPLAY_HEIGHT >= 240 ? font_emoji_64_init() : font_emoji_32_init(),
            });
    }

public:
    zhengchen_eye() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 2),  // 默认使用 C5 (WiFi网桥,含5G)
        boot_button_(BOOT_BUTTON_GPIO){
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeButtons();
#if !CONFIG_USE_NOLCD
        InitializeSpi();
        InitializeGc9107Display();
        GetBacklight()->SetBrightness(100);
#endif
        touch_init();
        InitializeMotion();
        GetAudioCodec()->SetOutputVolume(100);
        xTaskCreate(touch_read_task, "touch_read_task", 2048, this, 5, NULL);
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
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
    
#if !CONFIG_USE_NOLCD
    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
#endif

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
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

DECLARE_BOARD(zhengchen_eye);
