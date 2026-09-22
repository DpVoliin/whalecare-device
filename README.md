# whale-device · 桌面鲸鲸（T-Display-S3-Pro）

一块 2.33 寸触摸屏放在桌上，屏幕上养一只会游动、会眨眼的鲸鱼。
她该提醒你的时候会游过来抬头说话 —— 你点一下就算「收到了」，这个反馈直接喂给
[whalecare](https://github.com/DpVoliin/whalecare) 中枢里的 Thompson 学习。

**这块屏是 whalecare 的"第三条腿"**：不依赖手机通知、不用打开电脑 —— 它就摆在桌上。

---

## 硬件（就这一块，别买错）

| | |
|---|---|
| 型号 | **LILYGO T-Display-S3-Pro**（带 MVSR 板 / 带 ABS 外壳） |
| 屏幕 | 2.33" 触摸 IPS TFT · 222×480 · **ST7796**（SPI） |
| 触摸 | **CST226SE**（电容，I2C） |
| 主控 | ESP32-S3R8 · 16MB flash · 8MB PSRAM |
| 白送 | 环境光 + 接近传感器 · TF 卡 · Qwiic/STEMMA QT · 喇叭 + 麦克风（MVSR 板） |

> 为什么选它而不是那块 1.91" AMOLED：**这是常亮一整天**的桌面设备，
> AMOLED 静态画面会烧屏 ✗，LCD 不会 ✓；而且这块更大（2.33"）+ 自带接近传感器 ✓

---

## 编译状态（实测 ✓）

```
PlatformIO + espressif32 真编译通过 ✓
  RAM:   14.5%（47,360 / 327,680 字节）
  Flash: 14.6%（957,901 / 6,553,600 字节）   ← 还剩 85%，字库/传感器随便加
产物：.pio/build/whale/firmware.bin（合法 ESP32-S3 镜像：magic 0xe9 ✓）

注意：**仓库里不带编译好的 .bin** ✗ —— 因为编译要嵌你自己的 WiFi/中枢地址/token/证书，
      别人的 .bin 你刷了也连不上 ✓ 必须自己在本地编一次（就是上面那三步 ✓）
```

## 三步跑起来

```bash
# ① 填自己的东西（secrets.h 已在 .gitignore，绝不会被提交）
cp include/secrets.h.example include/secrets.h
#   里面要填：Wi-Fi（★ 只能 2.4GHz，ESP32-S3 连不上 5GHz）、中枢地址、
#            设备 token、中枢那张自签证书

# ② 编译
pio run

# ③ 烧录（插 USB-C）
pio run -t upload
```

**取设备 token 和证书**（在中枢那台服务器上敲）：

```
sudo -n python3 -c "import json;print(json.load(open('/root/hub/hub.json'))['mcu']['token'])"
sudo -n cat /root/hub/tls/hub.crt
```

### 烧不进去的两个常见原因（不是板子坏了）

1. **USB 线只能充电** ✗ —— 插上没有串口就是这个原因。换一根能传数据的线。
2. **自动复位失败** —— 按住 **BOOT** 键不放，插 USB，再松开，然后重新 `pio run -t upload`。

---

## 它怎么和中枢说话

| 方向 | 接口 | 用途 |
|---|---|---|
| 取提醒 | `GET /mcu/inbox?d=<设备名>&t=<token>` | 回一行：`ok\|<id>\|<文本>` / `none` / `err:token` |
| 回执 | `GET /mcu/ack?id=<id>&d=&t=` | 告诉中枢这条已经显示过了 |
| 反馈 | `POST /feedback` `{"verdict":"good"\|"bad"}` | 按键 → 她的 Thompson 学习（★ 只认 good/bad）|

这些接口**中枢那边早就有了**（whalecare v0.1.x 的"硬件无关下行口"），
所以本固件不需要中枢做任何改动 ✓

---

## v1 能做什么 / 还不能做什么（说清楚）

**能**：
- 待机游动 + 眨眼 + 说话时头上冒水汽（全部**代码画**，无第三方素材）
- 收到提醒 → 气泡显示 + 游过来抬头
- **IO12 = 收到 ✓ / IO16 = 别烦我 ✗** → 直接进她的学习闭环
- 长按 IO12 = 静音 1 小时（本地生效，不上报）
- 夜间自动变暗（23:00–07:00，跟中枢免打扰一致）

**还不能（诚实的下一步）**：
- **触摸**（CST226SE）→ v1.1。它的 I2C 寄存器协议要先把官方示例核对清楚再写 ——
  **不猜着写**：猜错的触摸驱动只会让人以为"这块板坏了"。
- **环境光/接近传感器** → v2（做自动亮度 + "你人在不在桌前"，那才是这块板最值的地方）
- **语音**（MVSR 板自带麦克风+喇叭）→ v2 之后再说

---

## 中文怎么显示

- v1 直接用 LovyanGFX 内置的 **efontCN**（3-clause BSD）→ 常用汉字能显示 ✓
  生僻字会缺（显示空白），不是 bug。
- 想要"她说过的话全都能显示" → 跑 `tools/gen_font.py` 生成**子集字库**：
  从你中枢里她真实说过的话提字（约 600 字 / ~40KB），16MB flash 绰绰有余。

---

## 排障

| 现象 | 原因 | 处理 |
|---|---|---|
| 屏幕全白 / 花屏 | 列偏移或反相不对 | `pins.h` 的 `LCD_OFFSET_X`（官方是 **49**）；颜色发负片就改 `cfg.invert` |
| 屏幕不亮 | 背光脚 | `pins.h` 的 `LCD_BL = 48` |
| 串口显示 PSRAM 0 | `memory_type` 不对 | `platformio.ini` 里必须是 `qio_opi`（S3R8 是 OPIO）|
| Wi-Fi 连不上 | 用了 5GHz | ESP32-S3 只支持 2.4GHz |
| 中枢回 `err:token` | token 不对 | 用 `/root/hub/hub.json` 里的 `mcu.token`（不是主 token）|
| 连不上中枢 | 证书没填对 | `WHALE_CA_CERT` 要贴 `/root/hub/tls/hub.crt` 的**全文** |

---

## 许可

本项目自有代码 **MIT**（见 `LICENSE`）。依赖与资料出处见 `THIRD-PARTY.md` ——
特别说明：LilyGo 的示例代码是 **GPL-3.0**，本仓库**一行都没抄**（只取引脚号等硬件事实）。
