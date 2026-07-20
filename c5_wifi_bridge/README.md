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
1. `WIFI_CONFIG` + `WIFI_CONNECT` → 等 `EVT_WIFI_STATUS.connected=1`
2. `SOCK_OPEN(link=0, proto=TLS, host, 443)` → 等 `EVT_SOCK_OPENED.ok=1`（WebSocket 用）
3. `SOCK_OPEN(link=1, proto=UDP, host, port)` → 音频包（MQTT+UDP 模式）
4. `SOCK_SEND` 发数据；C5 收到网络数据回 `EVT_SOCK_DATA`
5. 断开回 `EVT_SOCK_CLOSED`

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

## 待办 / 下一步

- [ ] S3 端 `C5Bridge` 驱动 + `Esp32C5Board`（可继续让我写）
- [ ] `DualNetworkBoard` 增加 C5 分支
- [ ] TLS 证书校验开启
- [ ] 可选：WiFi 配网凭据由 S3 通过 `WIFI_CONFIG` 下发（复用小智现有配网 UI）
