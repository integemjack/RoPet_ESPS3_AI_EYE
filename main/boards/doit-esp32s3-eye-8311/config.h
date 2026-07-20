/*
 * @Description: 
 * @Author: cjs丶
 * @Date: 2025-05-06 15:58:55
 * @LastEditTime: 2025-05-13 18:56:22
 * @LastEditors: cjs丶
 */
#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

//音频输入和输出的采样率
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_38
#define AUDIO_I2S_GPIO_WS GPIO_NUM_13
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_14
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_12
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_45

#define AUDIO_CODEC_USE_PCA9557
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_1
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_2
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  0x82

#define BUILTIN_LED_GPIO        GPIO_NUM_3
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC
                                                              
//分辨率
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
//镜像和轴交换：通常不需要镜像或轴交换，除非你的硬件设计需要特定的显示方向。
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
//默认不启用颜色反转。
#define DISPLAY_INVERT_COLOR    true
//RGB顺序：LH128R-IG01使用RGB垂直条纹排列，因此设置为 LCD_RGB_ELEMENT_ORDER_RGB。
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_BGR
//偏移量：通常不需要偏移，除非你的显示屏有特定的显示区域限制。
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
//背光输出取反：默认不取反，除非你的背光控制电路需要取反信号。
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_42
#define DISPLAY_SPI_SCLK_HZ     (40 * 1000 * 1000)


/* LCD size */
#define GC9A01_LCD_H_RES   (240)
#define GC9A01_LCD_V_RES   (240)
/* LCD settings */
#define GC9A01_LCD_SPI1_NUM         (SPI3_HOST)
#define GC9A01_LCD_SPI2_NUM         (SPI2_HOST)
#define GC9A01_LCD_PIXEL_CLK_HZ    (20 * 1000 * 1000)
#define GC9A01_LCD_CMD_BITS        (8)
#define GC9A01_LCD_PARAM_BITS      (8)
#define GC9A01_LCD_COLOR_SPACE     (ESP_LCD_COLOR_SPACE_RGB)
#define GC9A01_LCD_BITS_PER_PIXEL  (16)
#define GC9A01_LCD_DRAW_BUFF_DOUBLE (1)
#define GC9A01_LCD_DRAW_BUFF_HEIGHT (240)
#define GC9A01_LCD_BL_ON_LEVEL     (1)
/* LCD-SPI2 pins */
#define GC9A01_SPI2_LCD_GPIO_SCLK       (GPIO_NUM_21)
#define GC9A01_SPI2_LCD_GPIO_MOSI       (GPIO_NUM_41)
#define GC9A01_SPI2_LCD_GPIO_RST        (GPIO_NUM_39)
#define GC9A01_SPI2_LCD_GPIO_DC         (GPIO_NUM_40)
#define GC9A01_SPI2_LCD_GPIO_CS         (GPIO_NUM_NC)
#define GC9A01_SPI2_LCD_GPIO_BL         (GPIO_NUM_NC)
#define GC9A01_SPI2_LCD_GPIO_MISO       (GPIO_NUM_NC)

/* LCD-SPI1 pins */
#define GC9A01_SPI1_LCD_GPIO_SCLK       (GPIO_NUM_44)
#define GC9A01_SPI1_LCD_GPIO_MOSI       (GPIO_NUM_43)
#define GC9A01_SPI1_LCD_GPIO_RST        (GPIO_NUM_46)
#define GC9A01_SPI1_LCD_GPIO_DC         (GPIO_NUM_8)
#define GC9A01_SPI1_LCD_GPIO_CS         (GPIO_NUM_NC)
#define GC9A01_SPI1_LCD_GPIO_BL         (GPIO_NUM_NC)
#define GC9A01_SPI1_LCD_GPIO_MISO       (GPIO_NUM_NC)

#define ML307_RX_PIN GPIO_NUM_48
#define ML307_TX_PIN GPIO_NUM_47

#endif // _BOARD_CONFIG_H_
