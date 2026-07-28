# RoPet ESP32-S3 AI EYE

一只会说话、会动的桌面宠物。**两颗芯片、两套固件**：

| | ESP32-S3（主控） | Seeed XIAO ESP32C5（网桥） |
|---|---|---|
| 工程 | `zhengchen-eye/` | `c5_wifi_bridge/` |
| 职责 | 小智 AI 语音、圆屏眼睛、触摸、MCP、**动作决策** | 5GHz WiFi 上网、配网、**舵机执行** |
| 连接 | 两者通过一组 UART（921600）以自定义帧协议通信 | |

两个分工上的关键取舍：

- **C5 当"UART WiFi 调制解调器"**，插在 S3 原本接 ML307 4G 模组的 TX/RX 上，让小智走 C5 的
  5GHz WiFi。**完全不影响原有 4G 方案** —— ML307 和 C5 共用同一组串口，互斥使用。
- **舵机（四肢 + 尾巴）挂在 C5 上**，因为 S3 的引脚已经用尽。分层是
  「S3 决定做什么动作（语义），C5 决定怎么走出来（50Hz 插值 + 硬件 PWM）」，
  一次动作只发一条 ~10 字节的帧，不在 UART 上跑角度流。

## 目录结构

```
RoPet_ESPS3_AI_EYE/
├── README.md               本文件
├── zhengchen-eye/          【S3 主控固件】小智 fork
│   ├── main/
│   │   ├── application.cc          主状态机 / 协议消息分发
│   │   ├── mcp_server.cc           MCP 工具服务端
│   │   └── boards/
│   │       ├── zhengchen_eye/      *** 本机板级 ***
│   │       │   ├── config.h            S3 引脚定义
│   │       │   ├── zhengchen_eye.cc    板卡 + 情绪拦截 + 触摸
│   │       │   └── eye_motion.{cc,h}   动作语义层: MCP 工具 / 情绪映射 / 状态钩子
│   │       └── common/esp32c5_board.{cc,h}   平行于 Ml307Board 的 C5 板卡
│   └── components/c5_bridge/       S3 侧网桥驱动
│       ├── c5_bridge.cc            UART 帧引擎 + link(socket) 管理
│       ├── c5_tcp/udp/mqtt/network.cc  socket 抽象适配
│       ├── c5_motion.cc            动作下发 (MOTION_PLAY/POSE/STOP/TRIM/QUERY)
│       ├── c5_provision_portal.cc  配网门户 (热点跑在 S3 的 2.4G 射频上)
│       └── include/bridge_protocol.h   *** 帧协议 (副本, 与 C5 端逐字节一致) ***
└── c5_wifi_bridge/         【C5 网桥固件】
    ├── bridge_protocol.h           *** 帧协议 (源本) ***
    ├── sdkconfig.defaults          双频 / 高波特率 UART / TLS
    └── main/
        ├── main.c                  入口 + 帧分发
        ├── bridge_uart.c           UART 分帧收发 (magic+type+link+len+payload+crc16)
        ├── bridge_wifi.cc          WiFi STA 管理 (双频) + 扫描 / 凭据验证
        ├── bridge_sock.c           TCP/TLS/UDP socket 代理
        ├── servo_ctrl.{c,h}        舵机运动: LEDC PWM + 50Hz 插值 + 动作表
        └── bridge_internal.h       内部声明 + UART 引脚配置
```

> `bridge_protocol.h` 有两份副本，**改一份必须同步另一份**（`diff` 应无输出）。

## 编译烧录

需要 ESP-IDF **5.4+**（实测 5.5.2）。两个工程独立构建，各自 `set-target` 一次即可。

```bash
# C5 网桥
cd c5_wifi_bridge
idf.py set-target esp32c5
idf.py -p <C5端口> build flash monitor

# S3 主控
cd zhengchen-eye
idf.py set-target esp32s3
idf.py -p <S3端口> build flash monitor
```

两颗芯片各有独立的 USB 口，都是 Espressif 的 `VID_303A`。分不清哪个是哪个时用
`python -m esptool --port COMx chip_id` 探测，会打印 `Chip is ESP32-S3` 或 `ESP32-C5`。

## 引脚配置

硬件：主控 **ESP32-S3**（zhengchen_eye 板）+ 网桥 **Seeed XIAO ESP32C5**。
S3 的引脚已经用尽，所以舵机（四肢 + 尾巴）全部挂在 C5 上。

### 1. C5 ↔ S3 网桥 UART（关键：TX/RX 必须交叉）

| 信号 | C5 (XIAO pad) | C5 GPIO | → | S3 GPIO | S3 宏 |
|------|---------------|---------|---|---------|-------|
| C5 发 → S3 收 | D9  | **9**  | → | **48** | `ML307_RX_PIN` |
| C5 收 ← S3 发 | D10 | **10** | ← | **47** | `ML307_TX_PIN` |
| GND | GND | — | — | GND | 必须共地 |

- 波特率 **921600**，无硬件流控（`BRIDGE_UART_RTS_PIN/CTS_PIN = -1`）
- C5 侧改这里：`c5_wifi_bridge/main/bridge_internal.h` 的 `BRIDGE_UART_TX_PIN` / `BRIDGE_UART_RX_PIN`
- S3 侧改这里：`zhengchen-eye/main/boards/zhengchen_eye/config.h` 的 `ML307_TX_PIN` / `ML307_RX_PIN`
  （C5 与 ML307 4G 模组共用这组串口，互斥使用）

> IO48 是 S3 的**接收**脚（原来接 4G 模组的 TX），所以接 C5 的 **TX(D9)**；
> IO47 是 S3 的**发送**脚，所以接 C5 的 **RX(D10)**。

### 2. C5 舵机（四肢 + 尾巴）

```
                 ┌───[USB-C]───┐
      舵机0 FL ──┤ D0       5V ├── ✗ 不要用(USB VBUS 直通)
      舵机1 FR ──┤ D1      GND ├── ★ 与舵机电源单点共地
      舵机2 RL ──┤ D2      3V3 ├── ✗ 不要给舵机供电
      舵机3 RR ──┤ D3      D10 ├── ✗ 网桥 UART RX ← S3.IO47
         空着  ──┤ D4       D9 ├── ✗ 网桥 UART TX → S3.IO48
     舵机4 TAIL ─┤ D5       D8 ├── 备用
         备用  ──┤ D6       D7 ├── 备用
                 └─────────────┘
```

| 通道 | 部位 | XIAO pad | GPIO | LEDC ch | 机械行程 | 中位 | invert |
|:----:|------|:--------:|:----:|:-------:|----------|:----:|:------:|
| 0 | 左前肢 FL   | **D0** | 1  | 0 | 30°–150° | 90° | 否 |
| 1 | 右前肢 FR   | **D1** | 0  | 1 | 30°–150° | 90° | **是** |
| 2 | 左后肢 RL   | **D2** | 25 | 2 | 30°–150° | 90° | 否 |
| 3 | 右后肢 RR   | **D3** | 7  | 3 | 30°–150° | 90° | **是** |
| 4 | 尾巴 TAIL   | **D5** | 24 | 4 | 45°–135° | 90° | 否 |

- PWM：50Hz，500µs = 0° / 2500µs = 180°，LEDC 14bit（分辨率 ~0.11°）
- 右侧两肢 `invert = true`：左右对称安装时舵机朝向相反，同一个"抬腿"角度两边要反着给
- 全部定义在 `c5_wifi_bridge/main/servo_ctrl.c` 的 `SERVO_GPIO_*` 与 `s_cfg[]`。
  **装反了改 `invert`，不要去改动作表**

**C5 上不能给舵机用的脚：**

| pad | GPIO | 原因 |
|-----|:----:|------|
| D9  | 9  | 网桥 UART TX |
| D10 | 10 | 网桥 UART RX |
| —   | 13/14 | USB D-/D+，动了就烧不进去（XIAO 上未引出） |

> D2(GPIO25) 和 D3(GPIO7) 是 strapping 脚，但**都不参与启动模式**（C5 的启动模式由
> GPIO26/27/28 决定，XIAO 上没引出），所以可以用于舵机：
> GPIO25 只管 SDIO 采样/驱动时钟沿（本工程不用 SDIO slave）；
> GPIO7 管 JTAG 信号源选择，最坏情况是复位瞬间干扰 C5 自己的 USB-JTAG 调试。
> 实测接上后 C5 正常启动、WiFi 正常、USB 串口日志照常可读。

### 3. 舵机供电（必读）

```
 舵机信号(橙/白) ──[220Ω]── XIAO 的 Dx
 舵机 V+  (红)   ──────────  外部 5V DC-DC 输出   ← 不是 XIAO 的 5V pad
 舵机 GND (棕/黑)──┬───────  DC-DC 的 GND
                    └───────  XIAO 的 GND pad     ← 单点共地，必须接
```

- DC-DC 输出端并 **470–1000µF 电解 + 0.1µF 陶瓷**
- **绝对不要用 XIAO 的 5V pad 带舵机**：那是 USB VBUS 直通（500mA 上限），
  5 路舵机堵转峰值可到 3–5A，会把 C5 的 LDO 输入一起拉垮 → 网桥复位 → 通话中断
- 固件侧已有三重保护（单动作 10s 超时、S3 链路 30s 看门狗、空闲 3s detach 松力），
  但那只能减少堵转时间，**替代不了独立供电**

### 4. S3 侧引脚占用总表

定义在 `zhengchen-eye/main/boards/zhengchen_eye/config.h`（触摸和电池检测在同目录的 `zhengchen_eye.cc`）。

| GPIO | 用途 | 宏 / 位置 |
|:----:|------|-----------|
| 0  | BOOT 按键 | `BOOT_BUTTON_GPIO` |
| 1  | 音频 codec I2C SDA | `AUDIO_CODEC_I2C_SDA_PIN` |
| 2  | 音频 codec I2C SCL | `AUDIO_CODEC_I2C_SCL_PIN` |
| 3  | 板载 LED | `BUILTIN_LED_GPIO` |
| 4  | 触摸（左头） | `TOUCH_PAD_NUM4` |
| 5  | 触摸（右头） | `TOUCH_PAD_NUM5` |
| 7  | 电池检测 / 充电状态 | `PowerManager(GPIO_NUM_7)` |
| 8  | LCD DC | `DISPLAY_DC` |
| 12 | I2S DIN | `AUDIO_I2S_GPIO_DIN` |
| 13 | I2S WS | `AUDIO_I2S_GPIO_WS` |
| 14 | I2S BCLK | `AUDIO_I2S_GPIO_BCLK` |
| 38 | I2S MCLK | `AUDIO_I2S_GPIO_MCLK` |
| 42 | LCD 背光 (PWM) | `DISPLAY_BACKLIGHT_PIN` |
| 43 | LCD SPI MOSI | `DISPLAY_SDA` |
| 44 | LCD SPI SCLK | `DISPLAY_SCL` |
| 45 | I2S DOUT | `AUDIO_I2S_GPIO_DOUT` |
| 46 | LCD RESET | `DISPLAY_RES` |
| **47** | **→ C5 UART RX (D10)** | `ML307_TX_PIN` |
| **48** | **← C5 UART TX (D9)** | `ML307_RX_PIN` |

LCD CS 未接（`DISPLAY_CS = GPIO_NUM_NC`），音量键未接。
**S3 已无空余引脚可用于舵机 —— 这正是舵机挂在 C5 侧的原因。**

## 帧协议

```
[0xAA][0x55][type(1)][link_id(1)][len(2 LE)][payload(len)][crc16(2 LE)]
```
- `crc16` = CRC16-CCITT，覆盖 `type..payload`
- `link_id` = socket 编号 0..5；非 socket 消息填 0xFF
- 消息类型见 `c5_wifi_bridge/bridge_protocol.h`

类型分段：`0x0x` 控制/WiFi，`0x1x` socket，`0x2x` 舵机，`0x8x`/`0x9x`/`0xAx` 事件，`0xEx` 日志。

典型音频通道流程（S3 视角）：
1. 上电后 C5 用已存凭据自主连 WiFi。S3 等 `EVT_WIFI_STATUS.connected=1`；
   若 C5 没有凭据会持续发 `EVT_NEED_PROVISION`，S3 转去开配网热点（见下节）
2. `SOCK_OPEN(link=0, proto=TLS, host, 443)` → 等 `EVT_SOCK_OPENED.ok=1`（WebSocket 用）
3. `SOCK_OPEN(link=1, proto=UDP, host, port)` → 音频包（MQTT+UDP 模式）
4. `SOCK_SEND` 发数据；C5 收到网络数据回 `EVT_SOCK_DATA`
5. 断开回 `EVT_SOCK_CLOSED`

## WiFi 配网：热点在 S3，验证在 C5

**配网热点跑在 S3 那颗闲置的 2.4G 射频上，不在 C5 上。** 这一点很反直觉，但是踩坑后
的必然结果：

> C5 是**单射频**。它自己开 SoftAP 做配网时，一旦 STA 去关联目标 AP（尤其 5G 的
> 信道 128），驱动会把 SoftAP 强行拖到同一信道，手机掉线数秒。实测手机会因此冻结
> 配网页的 JS，"验证结果"永远送不到页面上。同步等待、异步轮询、服务端状态注入、
> captive 探测重投都试过，全部败在这个根因上。
>
> 而 S3 在 C5 网络模式下那颗 2.4G 射频完全闲置。改由它出热点后：手机始终连在 S3 上、
> 信道固定、全程不掉线；C5 那边没有热点要维持，换信道随意；`/submit` 可以同步等 C5
> 的验证结果再返回，密码错就当场报错。详见 `c5_provision_portal.h` 头部注释。

| 职责 | 归谁 | 实现 |
|------|------|------|
| SoftAP + 配网网页 + DNS 劫持 | **S3** | `c5_bridge/c5_provision_portal.cc`，网页在 `assets/c5_portal*.html` |
| 5G AP 扫描列表 | **C5** | `CMD_WIFI_SCAN` → `EVT_WIFI_SCAN_RESULT`（S3 只有 2.4G 射频，看不见 5G） |
| 真实连接验证 | **C5** | `CMD_WIFI_CONFIG` → `EVT_WIFI_CONFIG_RESULT`（带中文错误文案） |
| 凭据存储 | **C5 的 NVS** | 验证通过才写盘 |

配网流程：

1. C5 上电发现无可用凭据 → 每 5 秒发一次 `EVT_NEED_PROVISION`
   （**必须周期重发**：C5 一般比 S3 先起来，开机那一发会打在 S3 的 UART 还没初始化的
   窗口里丢掉）
2. S3 收到后开热点 `Xiaozhi-XXXX` + 网页 `http://192.168.4.1`
3. 网页要列 SSID → S3 发 `CMD_WIFI_SCAN`，C5 扫回 5G 列表（按 RSSI 降序、同 SSID 去重）
4. 用户提交 → S3 发 `CMD_WIFI_CONFIG`，**同步等** C5 用自己的射频真连一次；
   失败则原样回显错误，热点不断，用户可当场重填
5. 成功 → C5 存 NVS 并发 `EVT_REBOOT_REQUEST`，两边一起重启

重置配网：**长按 BOOT 键** → S3 发 `CMD_WIFI_RESET` → C5 清凭据并重启进配网。

> C5 侧仍依赖 `78/esp-wifi-connect` **2.4.3**，但用的是本地 vendor 副本
> `c5_wifi_bridge/components/esp-wifi-connect`（`override_path`）。改了两处上游没有开关
> 的行为：`StartAccessPoint()` 写死 `WIFI_BAND_MODE_2G_ONLY`；`ConnectToWifi()` 收到首个
> `STA_DISCONNECTED` 就判失败，而 2.4G/5G 同名时驱动会先试 2.4G 失败再切 5G 成功
> （慢约 1.8s），会被误判。

## TLS 证书校验（安全提示）

`c5_wifi_bridge/main/bridge_sock.c` 中 TLS 默认**跳过服务器证书校验**（`skip_common_name=true`，未挂证书 bundle），
便于先跑通。生产环境应启用证书校验：
- `c5_wifi_bridge/main/CMakeLists.txt` 的 `REQUIRES` 增加 `esp-tls` 已含 bundle 支持
- 在 `bridge_sock_open` 的 TLS 分支设 `cfg.crt_bundle_attach = esp_crt_bundle_attach;`
  并 `#include "esp_crt_bundle.h"`，同时去掉 `skip_common_name`。

## S3 侧接入方式

`DualNetworkBoard` 在三种网络之间选，由 NVS `network.type` 决定：

| `network.type` | 板卡类 | 网络 |
|:---:|---|---|
| 0 | `WifiBoard` | S3 自带 2.4G |
| 1 | `Ml307Board` | ML307 4G |
| **2** | **`Esp32C5Board`** | **C5 网桥（含 5G）— 本机默认** |

C5 走独立的板卡类而不是复用 `Ml307Board`：ML307 是私有 AT 指令，本网桥是二进制帧协议，
分开可保证 **4G 代码路径零改动、零回归**。

> 多击 BOOT 切换网络类型的手势**已禁用**（`zhengchen_eye.cc` 里注释掉了）。
> 本机固定走 C5，且配网热点就跑在 S3 的 2.4G 射频上：误触切到 WIFI 模式会让那颗射频
> 改去做 STA、与配网热点冲突；切到 4G 模式则整个 C5 链路失效。需要切换时直接改 NVS。

S3 侧组件 `zhengchen-eye/components/c5_bridge/`：

| 文件 | 作用 |
|------|------|
| `c5_bridge.cc` | UART 帧引擎 + link(socket) 管理 + WiFi 状态 |
| `c5_network.cc` | 实现 esp-ml307 的 `NetworkInterface`，是上层的总入口 |
| `c5_tcp.cc` | 实现 `Tcp` 抽象（TCP/TLS），供 `WebSocket` 使用 |
| `c5_udp.cc` | 实现 `Udp` 抽象（音频 UDP） |
| `c5_mqtt.cc` | 实现 `Mqtt` 抽象 |
| `c5_motion.cc` | 舵机动作下发（`MOTION_PLAY/POSE/STOP/TRIM/QUERY`） |
| `c5_provision_portal.cc` | 配网门户（热点 + 网页 + DNS 劫持，跑在 S3 射频上） |
| `include/bridge_protocol.h` | 帧协议副本，与 `c5_wifi_bridge/bridge_protocol.h` 逐字节一致 |

板卡侧：`main/boards/common/esp32c5_board.{h,cc}`（平行于 `Ml307Board`）、
`dual_network_board.*`（`NetworkType::C5` 分支）。

> 依赖 `78/esp-ml307` **3.2.8**（`idf_component.yml` 声明 `~3.2.6`）。
> 该版本是 `NetworkInterface` 抽象体系，C5 适配层按它实现；HTTP/WebSocket 直接复用
> 组件自带的 `HttpClient` / `WebSocket`，所以本仓库没有 `c5_http.cc`。

## 动作系统（四肢 + 尾巴）

分层：**S3 决定做什么动作，C5 决定怎么走出来**。一次动作只发一条 ~10 字节的帧，
UART 上不跑角度流 —— 否则会和音频/socket 抢这根无硬件流控的串口，抖动直接变成舵机颤动。

S3 侧三个触发源（`zhengchen-eye/main/boards/zhengchen_eye/eye_motion.cc`）：

| 触发 | 来源 | 抢占 |
|------|------|:---:|
| MCP 工具调用 | 服务端 LLM 调 `self.pet.*`（摇尾巴/走/挥手/坐下/趴下/跳/跳舞/伸懒腰/发抖/点头/站起/停止） | ✅ |
| 情绪 | `{"type":"llm","emotion":"..."}`，19 条映射表；同情绪连发会去重 | ❌ |
| 设备状态 / 触摸 | 说话时轻摇尾、打断即停并归位、摸头挥对应侧前肢 | 触摸 ✅ |

C5 侧运动引擎（`c5_wifi_bridge/main/servo_ctrl.c`）：动作统一建模为每通道的正弦振荡
`angle = home + trim + offset·k + amp·k·sin(2π·t/T + φ)`，静态姿态就是 `amp=0` 的特例，
于是坐下/趴下和摇尾巴共用一套引擎。中位微调（trim）存在 **C5 的 NVS** 里，跟着舵机走，
换主板不用重标。

三重保护（舵机堵转会把 C5 的电拉垮进而断网，宁可少动也不能堵着）：单动作 10s 超时、
S3 链路 30s 看门狗（收不到帧就归位 + 松力）、空闲 3s 自动 detach。
LEDC 初始化失败只记日志不 abort —— 这颗芯片的主职是 WiFi 网桥。

## 待办 / 下一步

- [ ] TLS 证书校验开启 (`c5_wifi_bridge/main/bridge_sock.c`, 见上文)
- [x] 在 IDF 环境实际 `idf.py build` 两端做编译验证与联调
- [x] 按硬件实际修改 C5/S3 两侧 UART 引脚 (C5 D9/D10 ↔ S3 IO48/IO47)
- [ ] 接上真舵机验证机械行程 (`s_cfg` 的 min/max/invert) 与满载功耗
- [ ] 静态姿态 (坐下/趴下) 3s 后会因 idle detach 松力塌掉, 视实机效果决定是否按动作区分
