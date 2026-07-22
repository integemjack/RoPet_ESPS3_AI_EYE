# C5 WiFi 网桥 — 上机联调 Checklist

按阶段逐步验证，每一阶段通过后再进入下一阶段。遇到问题时，"预期日志"和"排查点"帮助快速定位。

两端日志 TAG：
- C5 固件：`c5_bridge` / `bridge_wifi` / `bridge_sock` / `bridge_uart`，以及转发到 S3 的 `C5` 前缀日志
- S3 端：`Esp32C5Board` / `C5Bridge` / `C5Transport` / `C5Mqtt` / `C5Http` / `Ota`

---

## 阶段 0：硬件与烧录

### C5 侧针脚（固定，定义在 `c5_wifi_bridge/main/bridge_internal.h`）

| 信号 | C5 GPIO | 宏 |
|------|---------|-----|
| UART TX | **GPIO4** | `BRIDGE_UART_TX_PIN` |
| UART RX | **GPIO3** | `BRIDGE_UART_RX_PIN` |

> ⚠️ ESP32-C5 的 **GPIO13=USB D-、GPIO14=USB D+**，绝不能用作 UART，否则一启动就掐断 USB（烧录后 esptool/monitor 全部失联、也没热点）。已避开。
| 端口 | UART_NUM_1 | `BRIDGE_UART_PORT` |
| 波特率 | 921600 | `BRIDGE_UART_BAUD` |

### S3 侧针脚（随所选板卡不同！定义在各板卡 `config.h` 的 `ML307_TX_PIN` / `ML307_RX_PIN`）

| S3 板卡 | ML307_RX_PIN (S3 收) | ML307_TX_PIN (S3 发) |
|---------|----------------------|----------------------|
| xingzhi-cube-0.96oled-ml307 | **GPIO11** | **GPIO12** |
| xingzhi-cube-1.54tft-ml307  | GPIO11 | GPIO12 |
| xingzhi-cube-0.85tft-ml307  | GPIO11 | GPIO12 |
| zhengchen-1.54tft-ml307     | GPIO11 | GPIO12 |
| bread-compact-ml307         | GPIO11 | GPIO12 |
| minsi-k08-dual              | GPIO11 | GPIO12 |
| kevin-sp-v3/v4-dev          | GPIO12 | GPIO13 |
| magiclick-2p5               | GPIO42 | GPIO44 |
| kevin-box-1                 | GPIO20 | GPIO19 |
| kevin-box-2 / tudouzi       | GPIO5  | GPIO6  |

| **doit-esp32s3-eye-8311**（本项目当前板卡） | **GPIO48** | **GPIO47** |

> 以你实际选的板卡 `config.h` 为准。下面以本项目板卡 **doit-esp32s3-eye-8311（RX=48, TX=47）** 为例。
> 该板卡 `XiaoZhiEyeBoard` 已经是 `DualNetworkBoard`，无需改动即可支持 C5。

### 接线（TX/RX 必须交叉！）— doit-esp32s3-eye-8311

```
   C5                         S3 (doit-esp32s3-eye-8311)
 ┌──────────┐               ┌───────────────────────────┐
 │ TX GPIO4 │──────────────>│ IO48  ML307_RX_PIN (S3 收) │
 │ RX GPIO3 │<──────────────│ IO47  ML307_TX_PIN (S3 发) │
 │ GND      │───────────────│ GND                       │
 │ 3V3      │───────────────│ 3V3                       │
 └──────────┘               └───────────────────────────┘
```

> 方向说明：`ML307_RX_PIN`/`ML307_TX_PIN` 是**从 S3 角度**命名的。
> IO48 是 S3 的**接收**脚（原来接 4G 模组的 TX），所以现在接 C5 的 **TX(GPIO4)**；
> IO47 是 S3 的**发送**脚（原来接 4G 模组的 RX），所以现在接 C5 的 **RX(GPIO3)**。

- [ ] **C5.TX(GPIO4) → S3.IO48(ML307_RX_PIN)**
- [ ] **C5.RX(GPIO3) → S3.IO47(ML307_TX_PIN)**
- [ ] **GND 共地**（必接，否则通信不稳定）
- [ ] **供电**：3V3 共用或各自独立供电（独立供电时仍需共地）
- [ ] **烧录 C5**：`cd c5_wifi_bridge; idf.py -p <C5口> flash monitor`
- [ ] **S3 选双网络板卡**：改用 ML307 双网络板子（如 `xingzhi-cube-0.96oled-ml307`），编译烧录
- [ ] **切到 C5 网络**：NVS `network.type = 2`（或按键循环 `SwitchNetworkType()` 切到 C5）

> 波特率两端都是 921600，务必一致。
> 若 C5 的 GPIO4/5 与你的 C5 开发板上其它功能冲突，改 `bridge_internal.h` 里的
> `BRIDGE_UART_TX_PIN` / `BRIDGE_UART_RX_PIN` 即可，重新编译烧录。

---

## 阶段 1：链路联通（PING / READY）

**目标**：确认 UART 分帧协议通、CRC 正确。

- [ ] C5 上电，monitor 看到 `c5_bridge: C5 WiFi Bridge starting...` → `ready`
- [ ] C5 启动时会主动发 `BRIDGE_EVT_READY`；S3 端日志应出现 `C5Bridge: C5 ready`
- [ ] S3 端若打印 `C5Bridge: CRC mismatch` → 波特率不一致 / 接线接触不良 / 干扰

**排查点**
- 完全没有任何 C5 日志转发到 S3（`C5:` 前缀）→ 检查 C5.TX→S3.RX 方向
- S3 发指令 C5 无反应 → 检查 C5.RX→S3.TX 方向
- 偶发 CRC 错 → 降低波特率试（两端同步改）或检查线长/地线

---

## 阶段 2：C5 配网 + WiFi 连接（含 5G）

**目标**：C5 自主完成配网并连上 WiFi。

- [ ] 首次上电无凭据：C5 日志 `bridge_wifi: no saved ssid, enter config mode`
- [ ] C5 开热点，日志 `config AP: Xiaozhi-XXXX  http://192.168.4.1`
- [ ] 手机连 `Xiaozhi-XXXX` 热点 → 浏览器打开 `192.168.4.1`
- [ ] 配网页选 **5G SSID** + 填密码 +（可选）填 OTA 地址 → 提交
- [ ] C5 保存后重启，用凭据连接，日志 `bridge_wifi: connected to <ssid>`
- [ ] S3 端收到状态：`C5Bridge: WiFi connected rssi=.. band=5G ip=x.x.x.x`

**排查点**
- 连不上 5G → 确认路由器 5G 已开、SSID 正确；`band=2G` 说明连的是 2.4G（同名双频时正常，优先信号强的）
- S3 一直等不到 connected → 看阶段 1 链路是否真的通

---

## 阶段 3：OTA 地址同步（每台单独配置）

**目标**：C5 配网页填的 OTA 地址正确同步到 S3 并被使用。

- [ ] S3 网络就绪后拉取：`Esp32C5Board: synced ota_url from C5 to S3 NVS: <url>`
- [ ] 末尾 `/` 自动补全：填 `https://x.com/ota` → 存为 `https://x.com/ota/`
- [ ] 未填 OTA：`Esp32C5Board: C5 has no ota_url, cleared S3 setting (use default)`
- [ ] 后续 `Ota` 用同步后的地址：`Ota: Current version...`，检查请求 URL 正确

**获取失败时的重试与探活日志**
- C5 活着但丢帧：`ota_url fetch timeout but C5 is ALIVE (PONG ok), likely a dropped frame`
- C5 无响应：`ota_url fetch timeout and C5 NOT responding to PING, check wiring / C5 firmware`
  → 出现后者说明是接线/C5 固件问题，回到阶段 1

> 注意：OTA 地址获取失败会**无限重试**，设备会停在"注册网络"直到成功。这是预期行为。

---

## 阶段 4：单条连接（TCP/TLS）

**目标**：验证 socket 代理与 TLS。

- [ ] OTA 请求本身走 `C5Http`（TLS），能成功返回版本 JSON 即证明 TLS 链路通
- [ ] S3 日志 `C5Transport: connected link=.. host:port tls=1`
- [ ] 若 TLS 失败：先确认 C5 侧 `bridge_sock.c` 的证书校验（当前默认跳过，生产需开启）

**排查点**
- `C5Transport: connect timeout` → C5 侧 socket 打开失败，看 C5 日志 `bridge_sock`
- link 耗尽 `no free link` → 检查是否有连接未释放（`BRIDGE_MAX_LINKS=6`）

---

## 阶段 5：完整语音链路

**目标**：端到端跑通小智对话。

- [ ] 协议按 OTA 配置自动选择（MQTT+UDP 或 WebSocket）
- [ ] WebSocket 模式：`WS: Connecting to websocket server...` → server hello 成功
- [ ] MQTT 模式：`C5Mqtt: MQTT connected to <broker>:8883` + UDP 音频通道打开
- [ ] 唤醒 → 说话 → 有回复音频播放
- [ ] 长时间对话稳定，无频繁断连

**排查点**
- 能连上但没声音 → 音频 UDP 通道（`C5Udp`）或采样率不匹配
- MQTT 连不上 → 确认 broker 地址/端口/账号（来自 OTA 响应，存 S3 NVS `mqtt`）
- 频繁断连 → UART 吞吐或丢帧，看是否有 CRC 错误

---

## 阶段 6：4G 回归验证（确认没破坏原功能）

- [ ] 插回 ML307 模组，NVS `network.type = 1`
- [ ] 正常走 4G 上网、对话（ML307 代码路径零改动，应与改造前完全一致）

---

## 附：网络类型对照

| NVS `network.type` | 网络 | 板卡类 |
|--------------------|------|--------|
| 0 | S3 自带 2.4G WiFi | WifiBoard |
| 1 | ML307 4G | Ml307Board |
| 2 | C5 网桥（含 5G WiFi） | Esp32C5Board |

`SwitchNetworkType()` 循环顺序：WIFI → ML307 → C5 → WIFI
