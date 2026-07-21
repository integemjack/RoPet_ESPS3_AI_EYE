# C5 WiFi Bridge

把 ESP32-C5 做成一个"UART WiFi 调制解调器"，插到主控 ESP32-S3 原本接 ML307 4G 模组的 TX/RX 上，
让小智通过 C5 的 **5GHz WiFi** 上网。**完全不影响原有 4G 方案**——ML307 和 C5 共用同一组串口，互斥使用。

## 目录结构

```
c5_wifi_bridge/
├── CMakeLists.txt          顶层工程
├── sdkconfig.defaults      默认配置(启用双频/高波特率UART/TLS)
├── bridge_protocol.h       *** S3 与 C5 共享的帧协议 *** (S3 端也引用这个文件)
└── main/
    ├── main.c              入口 + 帧分发
    ├── bridge_uart.c       UART 分帧收发 (magic+type+link+len+payload+crc16)
    ├── bridge_wifi.c       WiFi STA 管理 (双频)
    ├── bridge_sock.c       TCP/TLS/UDP socket 代理
    ├── bridge_internal.h   内部声明 + 引脚配置
    └── CMakeLists.txt
```

## 编译烧录 C5

```bash
cd c5_wifi_bridge
idf.py set-target esp32c5
idf.py build flash monitor
```
需要 ESP-IDF 5.4+（与主工程一致）。

## 接线（关键：TX/RX 交叉）

| C5            | S3 (原 ML307 引脚) |
|---------------|---------------------|
| C5_TX (GPIO5) | S3_RX (ML307_RX_PIN)|
| C5_RX (GPIO4) | S3_TX (ML307_TX_PIN)|
| GND           | GND                 |
| 3V3           | 3V3                 |

C5 引脚在 `main/bridge_internal.h` 里改：`BRIDGE_UART_TX_PIN` / `BRIDGE_UART_RX_PIN`。
波特率默认 921600，与 S3 侧 `Ml307Board::StartNetwork()` 里 `SetBaudRate(921600)` 一致。

## 帧协议

```
[0xAA][0x55][type(1)][link_id(1)][len(2 LE)][payload(len)][crc16(2 LE)]
```
- `crc16` = CRC16-CCITT，覆盖 `type..payload`
- `link_id` = socket 编号 0..5；非 socket 消息填 0xFF
- 消息类型见 `bridge_protocol.h`

典型音频通道流程（S3 视角）：
1. 上电后 C5 自主连 WiFi（或开热点配网）。S3 只需等 `EVT_WIFI_STATUS.connected=1`
   （必要时可发 `WIFI_CONNECT` 让 C5 重启网络流程）
2. `SOCK_OPEN(link=0, proto=TLS, host, 443)` → 等 `EVT_SOCK_OPENED.ok=1`（WebSocket 用）
3. `SOCK_OPEN(link=1, proto=UDP, host, port)` → 音频包（MQTT+UDP 模式）
4. `SOCK_SEND` 发数据；C5 收到网络数据回 `EVT_SOCK_DATA`
5. 断开回 `EVT_SOCK_CLOSED`

## WiFi 配网：完全在 C5 上（不占用 S3）

WiFi 的配网、凭据存储、连接三件事**全部由 C5 负责**，S3 完全不碰 WiFi：

| 职责 | 归谁 | 用什么 |
|------|------|--------|
| 配网网页 (SoftAP + captive portal) | C5 | `WifiConfigurationAp`（小智同款组件） |
| 凭据存储 | C5 的 NVS | `SsidManager`（同款） |
| 连 WiFi（含 5G） | C5 | `WifiStation`（同款，全信道扫描优先信号最强） |

C5 直接依赖 `78/esp-wifi-connect`（与主工程 S3 **同一版本 2.4.3**），所以配网网页和小智
**完全一样**——这就是"把小智的配网网页整套搬到 C5 上"。

C5 上电后的 WiFi 状态机（与小智纯 WiFi 版行为一致）：
1. 读 C5 NVS 里已保存的凭据，尝试连接（60s）
2. 连不上 / 没有凭据 → 自动开热点 `Xiaozhi-xxxx` + 配网网页（`http://192.168.4.1`）
3. 用户在网页填 WiFi（可选 5G）→ 存入 C5 NVS → C5 重启后自动连

> 首次使用：手机连 C5 的 `Xiaozhi-xxxx` 热点，浏览器打开 `192.168.4.1` 配网。
> 之后 C5 会记住凭据自动连，无需 S3 参与。

## S3 侧如何接入（不动 4G 的代码）

现有结构：`DualNetworkBoard` 在 `Ml307Board`(4G) 和 `WifiBoard`(S3自带2.4G) 之间切。
接入 C5 的推荐做法是**新增第三种网络类型，ML307 分支原样保留**：

1. 新建 `main/boards/common/esp32c5_board.{h,cc}`，仿照 `ml307_board.cc` 结构：
   - 内部持有一个 `C5Bridge` 类（新组件）驱动 UART，实现帧协议的 S3 端。
   - `CreateHttp/CreateWebSocket/CreateMqtt/CreateUdp` 返回基于 C5Bridge 的传输实现
     （这些类把 `Connect/Send/OnData` 翻译成 `SOCK_OPEN/SOCK_SEND/EVT_SOCK_DATA` 帧）。
   - `StartNetwork()` 发 `WIFI_CONFIG`+`WIFI_CONNECT`，等 `EVT_WIFI_STATUS`。
   - `GetNetworkStateIcon()` 用 `EVT_WIFI_STATUS.rssi` 映射 WiFi 图标。

2. 在 `dual_network_board.h` 的枚举加一项：
   ```cpp
   enum class NetworkType { WIFI, ML307, C5 };   // 新增 C5
   ```
   在 `LoadNetworkTypeFromSettings` / `InitializeCurrentBoard` 里加 `type==2 -> C5` 分支，
   **ML307(type==1) 分支完全不改**。这样：
   - NVS `network.type = 1` → 4G（原样）
   - NVS `network.type = 2` → C5 5GHz WiFi

3. WebSocket/MQTT 两种协议都能用：本网桥同时提供 TLS(给 wss/mqtts) 和 UDP(给音频)。

> 为什么不直接复用 `Ml307Board`？因为 ML307 的私有 AT 指令和本网桥的二进制帧协议不同。
> 新增独立 board 类可保证 4G 路径零改动、零回归风险。

## TLS 证书校验（安全提示）

`bridge_sock.c` 中 TLS 默认**跳过服务器证书校验**（`skip_common_name=true`，未挂证书 bundle），
便于先跑通。生产环境应启用证书校验：
- `main/CMakeLists.txt` 的 `REQUIRES` 增加 `esp-tls` 已含 bundle 支持
- 在 `bridge_sock_open` 的 TLS 分支设 `cfg.crt_bundle_attach = esp_crt_bundle_attach;`
  并 `#include "esp_crt_bundle.h"`，同时去掉 `skip_common_name`。

## S3 端接入现状 (已完成)

S3 端已实现, 位于:
- `components/c5_bridge/`  — S3 侧驱动组件:
  - `c5_bridge.{h,cc}`   UART 帧引擎 + link(socket) 管理 + WiFi 状态
  - `c5_transport.{h,cc}` 实现 esp-ml307 的 `Transport` (TCP/TLS), 供 `WebSocket` 使用
  - `c5_udp.{h,cc}`       实现 `Udp` 抽象 (音频 UDP)
  - `c5_mqtt.{h,cc}`      在 Transport 上实现精简 MQTT 3.1.1 客户端
  - `c5_http.{h,cc}`      在 Transport 上实现精简 HTTP/1.1 客户端 (OTA/配置)
  - `include/bridge_protocol.h` 与本工程根的协议头保持一致
- `main/boards/common/esp32c5_board.{h,cc}` — 平行于 `Ml307Board` 的 C5 板卡
- `main/boards/common/dual_network_board.*`  — 新增 `NetworkType::C5` 分支

网络类型 (NVS `network.type`): 0=WiFi(S3自带) 1=ML307(4G) 2=C5(WiFi网桥)。
`SwitchNetworkType()` 三态循环切换。**ML307(4G) 代码路径零改动。**

> 重要: 本项目锁定 `78/esp-ml307` **2.1.6**, 其 `WebSocket(Transport*)` 与
> 同步 `Transport` (Send/Receive) 接口。C5 适配层严格按 2.1.6 头文件实现,
> 不同于 GitHub main 的 `NetworkInterface` 新 API。

## 待办 / 下一步

- [ ] TLS 证书校验开启 (C5 侧 bridge_sock.c, 见上文)
- [ ] 在 IDF 环境实际 `idf.py build` 两端做编译验证与联调
- [ ] 按硬件实际修改 C5/S3 两侧 UART 引脚
