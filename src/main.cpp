// whale-device · T-Display-S3-Pro 固件 v0.1.0
// 桌面鲸鲸：待机游动 + 收到提醒就抬头说话 + 按键给反馈（喂给中枢的 Thompson 学习）
//
// 设计取舍（都写在注释里，改的时候知道为什么）：
//   · 屏幕是 **SPI** 不是 QSPI → 全屏刷一帧 213KB（~43ms），硬刷会卡。
//     所以：**只给会动的区域开小画布**（222×180，放内部 SRAM），整块 pushSprite；
//     静态部分（背景/气泡/状态栏）直接画屏上，只在变化时重画。
//     ★ 一开始我写的是"全屏画布 + 推脏矩形"：那是错的 —— pushImage 要紧凑缓冲，
//       而从全屏画布里抠子矩形行距是整屏宽（stride），推过去会撕裂/错位。
//       编译器不会报这种错，只有真机看得见 → 所以宁可用整块推（对且简单）。
//   · 文字：v1 用 ASCII + 大数字（时间/倒计时/电量）。中文要**子集字库**
//     （tools/gen_font.py 生成，见 README）—— 没生成时中文显示为 ▢，不假装能显示。
//   · 交互：IO12 = 收到 ✓（喂 Thompson）/ IO16 = 别烦我 ✗；长按 IO12 静音 1 小时。
//     触摸（CST226SE）留到 v1.1 —— 它的 I2C 协议要先核对官方示例，
//     不猜着写（猜错的驱动只会让人以为"这块板坏了"）。
//   · 屏幕常亮对 LCD 不烧屏，但背光会老化 → 默认 60% 亮度，夜间 15%。
//
// 许可证：本项目 MIT。依赖 LovyanGFX（FreeBSD）。引脚与协议事实来自 LilyGo
// 官方仓库，**未复制其 GPL 代码** —— 见 THIRD-PARTY.md。

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdarg.h>
#include <time.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include <Wire.h>

#include "secrets.h"     // 不进版本控制：WiFi + 中枢地址 + 设备 token + 证书

// 子集字库（tools/gen_font.py 生成；没生成就用内置 efontCN 顶着）
#if __has_include("whale_font.h")
#include "whale_font.h"
#define HAVE_WHALE_FONT 1
#else
#define HAVE_WHALE_FONT 0
#endif
#include "pins.h"        // 引脚（事实，来自官方 pin_config.h）

// ────────────────────────────────── 屏幕（ST7796 / SPI / 222×480）
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7796 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;      // 40MHz：213KB/帧 ≈ 43ms，配合局部刷新够用
            cfg.freq_read = 16000000;
            cfg.pin_sclk = LCD_SCLK;
            cfg.pin_mosi = LCD_MOSI;
            cfg.pin_miso = LCD_MISO;
            cfg.pin_dc = LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = LCD_CS;
            cfg.pin_rst = LCD_RST;
            cfg.pin_busy = -1;
            cfg.panel_width = LCD_WIDTH;    // 222
            cfg.panel_height = LCD_HEIGHT;  // 480
            cfg.offset_x = LCD_OFFSET_X;    // ★ 49：官方示例给的列偏移，写 0 会整体错位
            cfg.offset_y = LCD_OFFSET_Y;
            cfg.readable = false;
            cfg.invert = true;              // IPS 面板要反相；若颜色发负片就改成 false
            cfg.rgb_order = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = LCD_BL;
            cfg.invert = false;
            cfg.freq = 2000;
            cfg.pwm_channel = 1;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

static LGFX lcd;

// ★ 为什么不是"全屏画布 + 推脏矩形"：
//   pushImage(x,y,w,h,buf) 要的是**紧凑**的 w×h 缓冲；从全屏画布里抠子矩形，
//   行与行之间隔着整屏宽度（stride），直接推过去会错位/撕裂。
//   LovyanGFX 更省心的做法：**只给会动的区域开一块小画布**，整块 pushSprite
//   （整块推不涉及 stride ✓ 而且这块 222×180×2B=80KB 放**内部 SRAM** 比 PSRAM 快 ✓）。
//   静态部分（背景、气泡、状态栏）直接画在屏上，只在变化时重画 ✓
#define ZONE_Y   150          // 鲸鱼活动区在屏幕上的纵向起点
#define ZONE_H   180          // 活动区高度
static LGFX_Sprite zone(&lcd);

// ────────────────────────────────── 鲸鱼的调色板
static const uint16_t C_BG_TOP   = 0x0C4A;   // 深青底（LCD 上比纯黑好看）
static const uint16_t C_BG_BOT   = 0x0A26;
static const uint16_t C_WHALE    = 0x3D9F;   // 鲸身（青蓝）
static const uint16_t C_WHALE_D  = 0x2A79;   // 阴影
static const uint16_t C_BELLY    = 0xE77F;   // 肚子（浅色）
static const uint16_t C_EYE      = 0x0000;
static const uint16_t C_BUBBLE   = 0xFFFF;
static const uint16_t C_TEXT     = 0x2124;

// ────────────────────────────────── 状态机
enum Mood { MOOD_IDLE, MOOD_TALK, MOOD_HAPPY };
static Mood   g_mood      = MOOD_IDLE;
static String g_msg;                  // 当前气泡文字（v1：ASCII；中文见 README）
static String g_lastId;               // 已确认的提醒 id（去重）
static bool   g_hasMsg    = false;
static uint32_t g_moodUntil = 0;
static uint32_t g_lastPoll  = 0;
static uint32_t g_lastBlink = 0;
static uint32_t g_lastFrame = 0;
static bool   g_eyeOpen   = true;
static uint32_t g_muteUntil = 0;       // 长按静音
static int    g_brightness = 60;       // 0-100

// ────────────────────────────────── 小工具
static void logf_(const char *fmt, ...) {
    char b[220];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    Serial.println(b);
}

static bool nightNow() {
    struct tm t;
    if (!getLocalTime(&t, 50)) return false;
    return (t.tm_hour >= 23 || t.tm_hour < 7);      // 与中枢免打扰一致：23:00–07:00
}

static void applyBrightness() {
    int pct = nightNow() ? 15 : g_brightness;
    lcd.setBrightness((uint8_t)(pct * 255 / 100));
}

// ────────────────────────────────── 画面
// 整屏背景（只在初始化/大变化时画一次）
static void drawBackgroundOnScreen() {
    int yMid = LCD_HEIGHT * 2 / 3;
    lcd.fillRect(0, 0, LCD_WIDTH, yMid, C_BG_TOP);
    lcd.fillRect(0, yMid, LCD_WIDTH, LCD_HEIGHT - yMid, C_BG_BOT);
}

// 活动区里的背景（每次画鲸鱼前先铺一遍，避免拖影）
static void drawBackgroundInZone() {
    zone.fillRect(0, 0, LCD_WIDTH, ZONE_H, C_BG_TOP);
    int yMid = LCD_HEIGHT * 2 / 3 - ZONE_Y;
    if (yMid < ZONE_H) zone.fillRect(0, yMid, LCD_WIDTH, ZONE_H - yMid, C_BG_BOT);
}

// 鲸鱼：全部程序化绘制（没有第三方素材 → 可以安心开源）
static void drawWhale(int cx, int cy, float phase, bool talk) {
    int bob = (int)(sinf(phase) * 4.0f);            // 上下浮动
    int sway = (int)(sinf(phase * 0.7f) * 6.0f);    // 尾巴摆动
    int bodyW = 108, bodyH = 74;
    int x = cx - bodyW / 2, y = cy - bodyH / 2 + bob;

    // 尾巴（后画在身下）——两片，随 phase 摆
    zone.fillTriangle(x - 4, cy + bob, x - 34, cy - 18 + sway + bob, x - 30, cy + 22 + bob, C_WHALE_D);
    zone.fillTriangle(x - 4, cy + bob, x - 36, cy + 34 + sway + bob, x - 30, cy + 22 + bob, C_WHALE_D);

    // 身体：圆角矩形 + 肚子
    zone.fillRoundRect(x, y, bodyW, bodyH, 30, C_WHALE);
    zone.fillRoundRect(x + 16, y + bodyH - 30, bodyW - 44, 22, 11, C_BELLY);

    // 胸鳍
    zone.fillTriangle(x + 30, cy + 6 + bob, x + 12, cy + 30 + bob, x + 46, cy + 26 + bob, C_WHALE_D);

    // 眼睛（眨眼）
    int ex = x + bodyW - 34, ey = cy - 10 + bob;
    if (g_eyeOpen) {
        zone.fillCircle(ex, ey, 9, C_EYE);
        zone.fillCircle(ex - 3, ey - 3, 3, C_BUBBLE);      // 高光
    } else {
        zone.fillRect(ex - 9, ey - 2, 18, 4, C_EYE);        // 闭眼一条线
    }

    // 说话时头上冒一小股水汽
    if (talk) {
        for (int i = 0; i < 3; i++) {
            int px = x + bodyW - 40 + i * 10, py = y - 8 - (int)(sinf(phase * 2 + i) * 4);
            zone.fillCircle(px, py, 4 + i, C_BELLY);
        }
    }
}

static void drawBubble(const String &text) {
    // ★ 气泡是**屏上直绘**（不是活动区）：它在 y=348，而活动区只有 y=150..330。
    //   曾经把方框画进 zone、文字画在 lcd → 方框被推出活动区裁掉、文字却浮在屏上 ✗✗
    int bx = 12, by = 348, bw = LCD_WIDTH - 24, bh = 118;
    lcd.fillRoundRect(bx, by, bw, bh, 14, C_BUBBLE);
    lcd.fillTriangle(bx + 60, by, bx + 74, by - 16, bx + 88, by, C_BUBBLE);   // 小尖角
    lcd.setTextColor(C_TEXT);
    lcd.setTextWrap(true);
    // efontCN_16：LovyanGFX 内置中文（3-clause BSD，已核实存在）→ v1 就能显示常用汉字 ✓
    // 生僻字会空白：那是覆盖率问题，用 tools/gen_font.py 生成子集字库补上 ✓
#if HAVE_WHALE_FONT
    lcd.setFont(&whale_font);              // 自己生成的子集：她说过的话全都能显示 ✓
#else
    lcd.setFont(&fonts::efontCN_16);       // 没生成子集时用内置（常用汉字够 ✓ 生僻字空白）
#endif
    lcd.setCursor(bx + 10, by + 10);
    lcd.print(text);
    lcd.setFont(&fonts::Font0);
}

static void drawStatusLine(const char *s) {
    // 同为屏上直绘（同样踩过 zone/lcd 混用的坑）
    lcd.fillRect(0, 12, LCD_WIDTH, 26, C_BG_TOP);
    lcd.setTextColor(C_BELLY);
    lcd.setFont(&fonts::Font0);
    lcd.setCursor(8, 18);
    lcd.print(s);
}

// 整屏重画一次（状态切换时用；平时只推局部）
static void renderAll() {
    drawBackgroundOnScreen();
    drawBackgroundInZone();
    drawWhale(LCD_WIDTH / 2 + 10, 232 - ZONE_Y, millis() / 900.0f, g_mood == MOOD_TALK);
    zone.pushSprite(0, ZONE_Y);            // ★ 整块推：不涉及 stride，不会有错位
    if (g_hasMsg) drawBubble(g_msg);
    char st[64];
    struct tm t;
    if (getLocalTime(&t, 30)) {
        snprintf(st, sizeof(st), "%02d:%02d  %s  %d%%", t.tm_hour, t.tm_min,
                 WiFi.isConnected() ? "wifi" : "offline", g_brightness);
    } else {
        snprintf(st, sizeof(st), "--:--  %s", WiFi.isConnected() ? "wifi" : "offline");
    }
    drawStatusLine(st);
}

// ────────────────────────────────── HTTPS 请求（证书固定）
static bool httpsGet(const String &path, String &out) {
    WiFiClientSecure c;
    c.setCACert(WHALE_CA_CERT);          // ★ 你中枢那张自签证书（10 年）——不用 setInsecure
    c.setTimeout(8);
    String host = WHALE_HOST;
    int port = WHALE_PORT;
    if (!c.connect(host.c_str(), port)) { logf_("[https] 连不上 %s:%d", host.c_str(), port); return false; }
    c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nX-Token: %s\r\nConnection: close\r\n\r\n",
             path.c_str(), host.c_str(), WHALE_TOKEN);
    // 读响应（跳过头）
    String line; int guard = 0;
    while (c.connected() && guard++ < 200) {
        line = c.readStringUntil('\n');
        if (line == "\r" || line.length() == 0) break;
    }
    out = "";
    guard = 0;
    while (c.connected() && guard++ < 400) {
        String l = c.readStringUntil('\n');
        out += l;
        if (out.length() > 900) break;
    }
    c.stop();
    out.trim();
    return out.length() > 0;
}

static bool httpsPost(const String &path, const String &body) {
    WiFiClientSecure c;
    c.setCACert(WHALE_CA_CERT);
    c.setTimeout(8);
    String host = WHALE_HOST;
    int port = WHALE_PORT;
    if (!c.connect(host.c_str(), port)) return false;
    c.printf("POST %s HTTP/1.1\r\nHost: %s\r\nX-Token: %s\r\nContent-Type: application/json\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
             path.c_str(), host.c_str(), WHALE_TOKEN, (int)body.length(), body.c_str());
    unsigned long t0 = millis();
    while (c.connected() && millis() - t0 < 2000) { while (c.available()) c.read(); }
    c.stop();
    return true;
}

// ────────────────────────────────── 取提醒（一行协议：ok|id|文本 / none / err:xxx）
static void pollInbox() {
    if (g_muteUntil && millis() < g_muteUntil) return;      // 静音中不去取
    String body;
    String q = "/mcu/inbox?d=" + String(WHALE_DEVICE) + "&t=" + String(WHALE_TOKEN);
    if (!httpsGet(q, body)) return;
    if (body.startsWith("none")) return;
    if (body.startsWith("err:")) { logf_("[inbox] 中枢回了 %s", body.c_str()); return; }

    int p1 = body.indexOf('|');
    int p2 = body.indexOf('|', p1 + 1);
    if (p1 < 0 || p2 < 0) return;
    String id = body.substring(p1 + 1, p2);
    String text = body.substring(p2 + 1);
    if (id == g_lastId) return;                             // 去重（同一条不重复播）

    g_lastId = id;
    g_msg = text;
    g_hasMsg = true;
    g_mood = MOOD_TALK;
    g_moodUntil = millis() + 30000;                         // 30 秒后回待机
    renderAll();
    logf_("[inbox] #%s %s", id.c_str(), text.c_str());

    httpsGet("/mcu/ack?id=" + id + "&d=" + String(WHALE_DEVICE) + "&t=" + String(WHALE_TOKEN), body);
}

// ────────────────────────────────── 反馈（喂给她的 Thompson 学习）
static void sendFeedback(const char *verdict) {
    // ★ 中枢的契约：verdict 只能是 good / bad（不是 up/down）—— /feedback 的注释里写着
    String b = String("{\"verdict\":\"") + verdict + "\",\"src\":\"device\",\"note\":\"desk-pet\"}";
    if (httpsPost("/feedback", b)) {
        g_mood = MOOD_HAPPY;
        g_moodUntil = millis() + 3000;
        renderAll();
    }
}

// ────────────────────────────────── Arduino
// ────────────────────────────────── I2C 探测模式
// 开机时**按住 IO16** 进来：把 I2C 总线上有应答的地址全打出来。
//
// 为什么需要它：触摸（CST226SE）和光线/接近（LTR-553）的**寄存器协议**我没有可靠出处，
// 猜着写的驱动只会让人以为"这块板坏了"。所以先用这个模式把**硬件事实**拿到手：
// 芯片在不在、地址是多少、ID 是什么 —— 然后我照着写精确驱动 ✓
static void i2cProbeMode() {
    logf_("\n=== I2C 探测模式 ===");
    logf_("总线：SDA=%d SCL=%d", IIC_SDA, IIC_SCL);
    Wire.begin(IIC_SDA, IIC_SCL, 400000);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            logf_("  0x%02X 有应答", a);
            found++;
        }
    }
    logf_("共 %d 个设备", found);
    logf_("预期：CST226SE 触摸 / SY6970 电量计 / PCF85063 时钟 / LTR-553 光线接近");
    logf_("把上面几行发我，我就能写精确驱动（不猜）");
    logf_("（按一下 Reset 退出这个模式）");
    while (true) { delay(1000); }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    logf_("\n=== whale-device v0.1.0 (T-Display-S3-Pro) ===");

    pinMode(BTN_B, INPUT_PULLUP);
    if (digitalRead(BTN_B) == LOW) i2cProbeMode();     // ★ 按住 IO16 进探测模式

    pinMode(BTN_A, INPUT_PULLUP);       // IO12：收到 ✓
    pinMode(BTN_B, INPUT_PULLUP);       // IO16：别烦我 ✗

    lcd.init();
    lcd.setBrightness(180);
    zone.setPsram(false);                  // 80KB 放内部 SRAM：SPI 写更快，肉眼能看出差别
    if (!zone.createSprite(LCD_WIDTH, ZONE_H)) {
        logf_("[gfx] 活动区画布创建失败（内存不足）");
    }
    logf_("[gfx] 活动区画布 %dx%d · PSRAM %u 字节可用", LCD_WIDTH, ZONE_H, (unsigned)ESP.getFreePsram());

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    logf_("[wifi] 连接中 %s", WIFI_SSID);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
    logf_("[wifi] %s", WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "连不上（会一直重试）");

    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");   // 北京时间
    renderAll();
    g_lastPoll = millis() - 20000;      // 启动后立刻取一次
}

void loop() {
    uint32_t now = millis();

    // Wi-Fi 掉线就重连（桌面设备最怕"断了就再也不连"）
    if (WiFi.status() != WL_CONNECTED && (now % 10000) < 5) {
        WiFi.reconnect();
    }

    // 取提醒：20 秒一次
    if (now - g_lastPoll > 20000) {
        g_lastPoll = now;
        pollInbox();
    }

    // 眨眼：每 3~5 秒一次
    if (now - g_lastBlink > (uint32_t)(3000 + (now / 1000 % 2000))) {
        g_lastBlink = now;
        g_eyeOpen = !g_eyeOpen;                // 眨眼会在下一帧随活动区一起推上去
    }

    // 动画：待机 100ms 一帧（10fps），说话时 66ms（15fps）
    uint32_t interval = (g_mood == MOOD_IDLE) ? 100 : 66;
    if (now - g_lastFrame > interval) {
        g_lastFrame = now;
        drawBackgroundInZone();
        drawWhale(LCD_WIDTH / 2 + 10, 232 - ZONE_Y, now / 900.0f, g_mood == MOOD_TALK);
        zone.pushSprite(0, ZONE_Y);
    }

    // 情绪回落到待机
    if (g_moodUntil && now > g_moodUntil) {
        g_moodUntil = 0;
        g_mood = MOOD_IDLE;
        g_hasMsg = false;
        renderAll();
    }

    // 按键：IO12 = 收到 ✓（长按 1 秒 = 静音 1 小时）/ IO16 = 别烦我 ✗
    static uint32_t aDown = 0;
    if (digitalRead(BTN_A) == LOW) {
        if (!aDown) aDown = now;
        else if (now - aDown > 1000 && g_muteUntil < now) {
            g_muteUntil = now + 3600000;       // 静音 1 小时（本地生效，不上报）
            g_msg = "muted 1h"; g_hasMsg = true; g_mood = MOOD_TALK;
            g_moodUntil = now + 2500; renderAll();
            aDown = 0;
        }
    } else if (aDown) {
        if (now - aDown < 1000) sendFeedback("good");
        aDown = 0;
    }
    static uint32_t bDown = 0;
    if (digitalRead(BTN_B) == LOW) { if (!bDown) bDown = now; }
    else if (bDown) {
        if (now - bDown > 40) {
            sendFeedback("bad");               // 告诉她"这条别提"
            g_hasMsg = false;
            renderAll();
        }
        bDown = 0;
    }

    // 每分钟校一次亮度（夜间自动变暗）
    static uint32_t lastBright = 0;
    if (now - lastBright > 60000) { lastBright = now; applyBrightness(); }

    delay(5);
}
