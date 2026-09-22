// 引脚定义 —— 这些是**事实**，来自 LilyGo 官方 pin_config.h（同一块板）
//   https://github.com/Xinyuan-LilyGO/T-Display-S3-Pro-MVSRBoard/blob/main/libraries/private_library/pin_config.h
// 只取数字，没有复制它的代码（它的示例是 GPL-3.0，与本项目的 MIT 不兼容）→ 见 THIRD-PARTY.md
#pragma once

// ── 屏幕 ST7796（SPI）
#define LCD_WIDTH      222
#define LCD_HEIGHT     480
#define LCD_BL         48      // 背光（PWM）
#define LCD_MOSI       17
#define LCD_MISO       8
#define LCD_SCLK       18
#define LCD_CS         39
#define LCD_DC         9
#define LCD_RST        47
#define LCD_OFFSET_X   49      // ★ 官方示例给的列偏移：写 0 会整体错位/花屏
#define LCD_OFFSET_Y   0

// ── 触摸 CST226SE（I2C；v1.1 接，先留着）
#define TOUCH_RST      13
#define TOUCH_INT      21

// ── I2C 总线（触摸 / 电量计 SY6970 / 时钟 PCF85063 / 光线接近 LTR-553 共用）
#define IIC_SDA        5
#define IIC_SCL        6

// ── 按键（板上丝印 IO12 / IO16，侧边）
#define BTN_A          12      // 收到 ✓
#define BTN_B          16      // 别烦我 ✗

// ── 喇叭 MAX98357A（MVSR 板上的；v2 想做语音时用）
#define SPK_BCLK       4
#define SPK_LRCLK      15
#define SPK_DATA       11
#define SPK_SD_MODE    41

// ── 麦克风 MP34DT05TR（V1.1 板；v2）
#define MIC_LRCLK      1
#define MIC_DATA       2
#define MIC_EN         3

// ── SD 卡
#define SD_CS          14
