# 动作触发：MCP 工具 与 服务端配置

面向 `zhengchen-eye`（ESP32-S3 小智端）+ `xiaozhi-esp32-server`。

**设备端不做本地关键词识别。** 所有动作指令由服务端 LLM 通过 MCP 工具调用（`self.pet.*`）下发，触发词、话术、调用策略全部在服务端配置，改词不用重新编译固件。

设备端剩三个触发源：

| 触发源 | 来源 | 抢占 | 说明 |
|--------|------|------|------|
| MCP 工具调用 | 服务端 LLM `tools/call` | ✔ | 唯一的显式动作入口 |
| 情绪 | `{"type":"llm","emotion":"happy"}` | ✘ | 19 个情绪的伴随动作 |
| 设备状态 / 触摸 | 本地事件 | 部分 | 说话时轻摇尾巴、被摸时反应 |

代码位置：

- `main/boards/zhengchen_eye/eye_motion.cc` / `.h` —— MCP 工具注册 + 情绪表
- `main/boards/zhengchen_eye/zhengchen_eye.cc` —— `EyeLcdDisplay::SetEmotion` 拦情绪
- `main/mcp_server.cc` / `.h` —— MCP 协议实现与通用工具
- `main/application.cc:459` —— 收到 `type=="mcp"` 的消息转给 `McpServer::ParseMessage`

服务端位置（本机部署在 `C:\Users\Administrator\Desktop\xiaozhi`）：

- `xiaozhi-esp32-server/main/xiaozhi-server/` —— Python 主服务
- `xiaozhi-esp32-server/main/manager-web/` —— Web 控制台
- `data/.config.yaml` —— 运行时配置（当前为 manager-api 模式）

---

## 1. 设备端 MCP 工具

### 1.1 调用链路

MCP 是 JSON-RPC 2.0 over WebSocket/MQTT。设备端在握手时声明 `"features":{"mcp":true}`（`websocket_protocol.cc:211`、`mqtt_protocol.cc:253`）。

**工具是自动发现的，服务端不需要也无法配置工具定义。** 服务端 `helloHandle.py:50-58` 看到 `features.mcp` 就自动建 `MCPClient` 并发起发现：

```python
if features.get("mcp"):
    conn.mcp_client = MCPClient()
    asyncio.create_task(send_mcp_initialize_message(conn))
```

之后 `mcp_handler.py` 走三步（全自动，无人工干预）：`id=1 initialize` → 等 1 秒 → `id=2 tools/list` → 逐条 `add_tool()`，有 `nextCursor` 就继续拉，拉完 `set_ready(True)` 并 `refresh_tools()`。

所以工具的名字、描述、参数、范围 100% 由固件的 `AddTool` 决定。服务端能控制的只有两件事：意图识别开关（决定工具是否喂给 LLM）和角色提示词（决定 LLM 何时调用、参数填多少）。要改工具本身必须动固件重新编译。

```
服务端 → {"type":"mcp","payload":{JSON-RPC}} → application.cc:459
                                             → McpServer::ParseMessage()
设备端 ← {"session_id":"...","type":"mcp","payload":{JSON-RPC}} ← Protocol::SendMcpMessage()
```

支持的三个方法（其它一律回 `Method not implemented`）：

**initialize** —— 回 `protocolVersion: 2024-11-05` + `serverInfo`（板名 + 固件版本）。`params.capabilities.vision.{url,token}` 会被取出来配给摄像头。

**tools/list** —— 分页返回工具列表。单包上限 8000 字节，超了在响应里带 `nextCursor`（下一个工具名），服务端拿它作为 `params.cursor` 继续拉。

**tools/call** —— 参数校验后**开独立线程**执行回调，避免阻塞主线程。栈大小由 `params.stackSize` 指定，默认 6144。

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.pet.walk",
    "arguments": { "direction": 1, "repeat": 4, "speed": 700, "amount": 70 }
  },
  "id": 2
}
```

返回统一包成 MCP content 格式：

```json
{"content":[{"type":"text","text":"true"}],"isError":false}
```

`ReturnValue` 是 `std::variant<bool,int,std::string>`，bool 转 `"true"`/`"false"`，int 转十进制串。

参数校验：没给值且**没有默认值**的参数 → 回 `Missing valid argument: xxx`；整数超出 `[min,max]` → 抛异常回错误。有默认值的参数缺失就用默认值。

### 1.2 已注册的工具

**通用工具**（`McpServer::AddCommonTools()`，在 `application.cc:370` 协议初始化前调用，故意插在列表最前面以利用 prompt cache）：

| 工具 | 参数 | 说明 |
|------|------|------|
| `self.get_device_status` | 无 | 音量/屏幕/电池/网络实时状态，改设备状态前必须先调这个 |
| `self.get_head_status` | 无 | 左侧头部触摸值，>30000 视为被摸，值越大力度越大 |
| `self.get_body_status` | 无 | 右侧头部触摸值，同上 |
| `self.audio_speaker.set_volume` | `volume` 0-100 | 设音量 |
| `self.screen.set_brightness` | `brightness` 0-100 | 有背光才注册 |
| `self.screen.set_theme` | `theme` = `light`/`dark` | 有主题才注册 |
| `self.camera.take_photo` | `question` string | 有摄像头才注册，拍照 + 视觉问答 |

**动作工具**（`EyeMotion::RegisterMcpTools()`，板卡构造期调用）。共用三个参数：`repeat`(1-50)、`speed`(200-5000 毫秒，越小越快)、`amount`(0-100 幅度%)：

| 工具 | 额外参数 | hold | 默认 repeat/speed/amount |
|------|---------|------|--------------------------|
| `self.pet.wag_tail` | | | 3 / 600 / 70 |
| `self.pet.jump` | | | 3 / 600 / 70 |
| `self.pet.dance` | | | 3 / 600 / 70 |
| `self.pet.stretch` | | | 3 / 600 / 70 |
| `self.pet.shiver` | | | 3 / 600 / 70 |
| `self.pet.nod` | | | 3 / 600 / 70 |
| `self.pet.sit` | | ✔ | 3 / 600 / 70 |
| `self.pet.lie_down` | | ✔ | 3 / 600 / 70 |
| `self.pet.walk` | `direction` 1=前 / -1=后 | | 4 / 700 / 70 |
| `self.pet.turn` | `direction` -1=左 / 1=右 | | 3 / 700 / 75 |
| `self.pet.wave` | `direction` -1=左肢 / 1=右肢 | | 3 / 450 / 75 |
| `self.pet.stand_up` | 无参数 | | 四肢回中位 |
| `self.pet.stop` | 无参数 | | 停止并回中位 |

`hold=true` 表示走完不回中位（`home_end = false`），姿态保持到下一个动作。动作工具全部 `preempt = true`，服务端显式要求的动作打断当前动作。

舵机链路不可用（当前网络模式不是 C5）时返回字符串 `"舵机链路不可用"`，让 LLM 知道失败原因。

### 1.3 情绪表 `kEmotionTable[]`

服务端下发 `{"type":"llm","emotion":"happy"}` 时映射到动作，`EyeLcdDisplay::SetEmotion` 拦下来转给 `EyeMotion::OnEmotion`。

同一情绪连续来只播第一条（每句话都会带情绪，重复播会让机器人一直抽搐）。`OnDeviceStateChanged` 里说话结束会清掉 `last_emotion_`，让下一轮能重新触发。

已映射的 19 个情绪：

- 摇尾巴：`happy` `laughing` `delicious` `loving` `cool`
- 跳舞：`funny` `silly`
- 点头：`kissy` `confident` `thinking`
- 挥手：`winking`
- 蹦跳：`surprised` `shocked`
- 发抖：`angry` `embarrassed` `confused`
- 趴下：`sad` `crying` `sleepy`

没列的情绪（`neutral`/`relaxed` 等）不做动作。情绪动作 `preempt = false`，不抢占 MCP 显式调用的动作。

### 1.4 添加一个设备端 MCP 工具

签名：

```cpp
void McpServer::AddTool(
    const std::string& name,          // 唯一标识，用 "模块.功能" 风格，如 self.pet.walk
    const std::string& description,   // 自然语言描述，LLM 靠这个决定调不调
    const PropertyList& properties,   // 参数表，可空
    std::function<ReturnValue(const PropertyList&)> callback);
```

`Property` 四种构造：

```cpp
Property("theme", kPropertyTypeString);                  // 必填，无默认值
Property("enabled", kPropertyTypeBoolean, true);         // 带默认值
Property("volume", kPropertyTypeInteger, 0, 100);        // 必填 + 范围
Property("repeat", kPropertyTypeInteger, 3, 1, 50);      // 默认值 3 + 范围 [1,50]
```

范围限制只对整数有效，对其它类型构造会抛 `std::invalid_argument`。

板卡内添加，参考 `eye_motion.cc`：

```cpp
#include "mcp_server.h"

void MyBoard::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.pet.shake_head",
                "左右摇头, 表示否定。repeat: 次数(1-20); speed: 周期毫秒(200-2000)",
                PropertyList({
                    Property("repeat", kPropertyTypeInteger, 3, 1, 20),
                    Property("speed",  kPropertyTypeInteger, 500, 200, 2000),
                }),
                [this](const PropertyList& p) -> ReturnValue {
                    int repeat = p["repeat"].value<int>();
                    int speed  = p["speed"].value<int>();
                    if (!DoShakeHead(repeat, speed)) {
                        return std::string("舵机链路不可用");
                    }
                    return true;
                });
}
```

注意点：

- **注册时机**：板卡构造期就能调 `AddTool`，`AddCommonTools()` 在 `application.cc:370` 协议初始化前执行。需要运行期才可用的资源（如 `EyeMotion` 的 C5 UART 链路）用 resolver 闭包惰性获取，别在注册时求值。
- **重名会被丢弃**并打 warning，不是覆盖。
- **描述要写清参数含义和取值范围**，这是 LLM 唯一的依据。现有代码把公共参数说明抽成 `kParamDoc` 字符串拼接，省得每个工具抄一遍。
- **回调跑在独立线程**，默认栈 6144 字节。栈需求大的操作让服务端在 `params.stackSize` 里指定更大值。
- **失败返回描述性字符串**而不是 `false`，LLM 能据此向用户解释。
- **回调内不要长时间阻塞**，服务端在等这个 JSON-RPC 响应。
- 加完工具需要**重新编译刷固件**。只想调触发话术不动工具本身，改服务端就够了（见第 2 部分）。

---

## 2. 服务端配置

本机部署在 `C:\Users\Administrator\Desktop\xiaozhi`，`data/.config.yaml` 里配了 `manager-api.url` + `secret`，即**从 Web 控制台读配置**的全模块部署模式。这种模式下 `xiaozhi-server/config.yaml` 只是模板，实际生效的是数据库里的配置，改 yaml 不生效。

### 2.1 前提：意图识别必须开

设备端 MCP 工具只有在意图识别启用时才会被喂给 LLM。三个选项：

| Intent | 设备 MCP 工具可用 | 说明 |
|--------|:---:|------|
| `function_call` | ✔ | **推荐**。按需调用、速度快，要求 LLM 支持 function call |
| `intent_llm` | ✔ | 串行前置意图识别模块，慢一些但通用性强 |
| `nointent` | ✘ | 完全不调工具，动作永远不会触发 |

`intent_llm` 走 `intent_llm.py:163-169`，把 `func_handler.get_functions()` 和 `mcp_client.get_available_tools()` 合并后喂给意图模型。`function_call` 走 `unified_tool_handler.py` 的 `ToolType.DEVICE_MCP` 执行器。两条路都能拿到 `self.pet.*`。

配置位置：控制台 → **智能体管理** → 编辑智能体 → 意图识别，选 `function_call`。

用 `function_call` 时 LLM 本身必须支持 function call。免费的 ChatGLMLLM 支持；追求稳定官方推荐 `DoubaoLLM` + `doubao-1-5-pro-32k-250115`。

### 2.2 调触发话术：改角色提示词

这是替代本地关键词表的主要手段。控制台 → **智能体管理** → 编辑智能体 → 角色介绍（对应 `agent-base-prompt.txt` 里的 `{{base_prompt}}` 占位符）。

**完整的、可直接粘贴的提示词见 [`agent-role-prompt.md`](./agent-role-prompt.md)**，那里迁移了原关键词表的全部 130 条规则、参数推断、否定判断和 ASR 容错别名。

改提示词**立即生效，不用重启服务、不用刷固件**。这是相比原来硬编码关键词表最大的好处。

两个容易踩的坑：

**工具名在提示词里必须写下划线形式**，即 `self_pet_walk` 而不是 `self.pet.walk`。服务端 `sanitize_tool_name()` 做了 `re.sub(r"[^a-zA-Z0-9_\-\u4e00-\u9fff]", "_", name)`，点号会变下划线，LLM 看到的是下划线版本。提示词里写点号，LLM 会照抄然后调不到工具。

**不用重复写"必须调工具"这类强约束。** `agent-base-prompt.txt` 的 `<tool_calling>` 和 `<action_priority>` 两个块已经写了：工具列表里有 `self_pet_*` 时，任何语言、任何间接说法的动作请求都必须先调工具，光在回复里描述动作算失败。角色介绍只需要补"哪句话对应哪个工具、参数填多少"。

原关键词表的完整内容也可以从 git 历史里捞：

```bash
git log --oneline -- main/boards/zhengchen_eye/eye_motion.cc
git show <commit>:main/boards/zhengchen_eye/eye_motion.cc
```

### 2.3 扩展外部工具：三种 MCP 接入方式

除了设备端 `self.pet.*`，服务端还支持三类工具来源，都在 `core/providers/tools/` 下：

| 类型 | 目录 | 配置位置 |
|------|------|---------|
| `device_mcp` | `tools/device_mcp/` | 无需配置，设备连上自动发现 |
| `server_mcp` | `tools/server_mcp/` | `data/.mcp_server_settings.json` |
| `mcp_endpoint` | `tools/mcp_endpoint/` | 控制台参数 `mcp_endpoint`，或 config 的 `mcp_endpoint` |
| `server_plugins` | `tools/server_plugins/` | 智能体的 `functions` 列表 |

**server_mcp（服务端本地拉起 MCP 进程）**：把 `xiaozhi-server/mcp_server_settings.json` 复制到 `data/` 目录（注意加点：`data/.mcp_server_settings.json`），删掉 `des` 和 `link` 说明字段。支持三种传输：

```json
{
  "mcpServers": {
    "my-stdio-server": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/some/dir"],
      "env": { "API_ACCESS_TOKEN": "xxx" }
    },
    "my-sse-server": {
      "url": "http://localhost:8080/sse",
      "headers": { "Authorization": "Bearer YOUR_TOKEN" }
    },
    "my-http-server": {
      "url": "http://localhost:8000/mcp",
      "transport": "streamable-http",
      "headers": { "Authorization": "Bearer YOUR_TOKEN" }
    }
  }
}
```

三种传输：`stdio`（标准输入输出，默认用 `command`）、`sse`（Server-Sent Events，默认用 `url`）、`streamable-http`（流式 HTTP，生产环境 Web 部署推荐，需显式写 `transport`）。改完重启 xiaozhi-server。

**mcp_endpoint（外部 MCP 接入点，WebSocket 反向接入）**：适合把工具跑在另一台机器上。地址格式 `ws://你的接入点ip或域名:端口号/mcp/?token=你的token`。控制台 → 参数管理 → `server.mcp_endpoint`。详见服务端 `docs/mcp-endpoint-integration.md` 和 `docs/mcp-endpoint-enable.md`。

**server_plugins（服务端内置插件）**：`plugins_func/functions/` 下的模块，在智能体配置的 `functions` 列表里勾选。系统默认已加载 `handle_exit_intent`（退出识别）和 `play_music`（音乐播放），**不要重复加载**。可选的有 `get_weather`、`get_news_from_newsnow`、`change_role`、`hass_*`（Home Assistant）等。注意 `play_music` 和 `hass_play_music` 只能留一个。

### 2.4 验证

启动服务后看日志：

- 设备连上应该有 `tools/list` 的往返，服务端拿到 13 个 `self.pet.*` + 若干 `self.*` 通用工具
- 工具执行时 `unified_tool_manager.py` 打 `执行工具: xxx，参数: {...}`
- 设备端串口日志 `EyeMotion` tag 打 `registered self.pet.* motion tools`，以及每次动作的 `done: seq=... action=... result=...`

工具名在服务端会过一遍 `sanitize_tool_name()`，所以日志里看到的名字可能和 `self.pet.walk` 略有差异（点号处理），`name_mapping` 里存着原名的映射。

服务端 `docs/mcp-get-device-info.md` 有查设备已注册工具的方法。

---

## 3. 这次改动做了什么

移除的部分（`eye_motion.cc` 净减约 460 行）：

- `kIntentTable[]` 关键词表（约 130 条中英文规则）
- `OnUserText()` 及其辅助函数：`LowerAscii` / `IsAsciiKeyword` / `FindKeyword` / `NegatedBefore` / `ParseCount` / `Clamp`
- `MarkPlayedLocally()` / `RecentlyPlayedLocally()` 以及 `local_action_` / `local_action_us_` 成员——本地和 MCP 两条路的 8 秒去重窗口，现在只剩一条路，不需要了
- 每个 MCP 工具回调开头的 `if (RecentlyPlayedLocally(...)) return true;`
- `EyeLcdDisplay::SetChatMessage` 覆写（`zhengchen_eye.cc`）
- 随之不再需要的 include：`<cctype>` `<cstdlib>` `<esp_timer.h>`

保留的部分：情绪映射、设备状态、触摸反应、全部 13 个 `self.pet.*` MCP 工具。

行为变化：

- 动作响应**慢 1-2 秒**（要等服务端 LLM 一个来回），这是转到服务端配置的固有代价
- 服务端 Intent 设成 `nointent` 时动作**完全不会触发**，原来本地关键词还能兜底
- 换取的是触发话术在服务端可配、改词不用刷固件、LLM 理解语义而不是子串匹配

---

## 相关文档

设备端：

- [`agent-role-prompt.md`](./agent-role-prompt.md) —— **可直接粘贴的角色提示词**（替代原本地关键词表）
- [`mcp-protocol.md`](./mcp-protocol.md) —— MCP 协议完整交互流程
- [`mcp-usage.md`](./mcp-usage.md) —— MCP 工具注册通用说明
- [`websocket.md`](./websocket.md) / [`mqtt-udp.md`](./mqtt-udp.md) —— 承载 MCP 的底层协议

服务端（`xiaozhi-esp32-server/docs/`）：

- `mcp-endpoint-enable.md` / `mcp-endpoint-integration.md` —— MCP 接入点
- `mcp-get-device-info.md` —— 查设备已注册工具
- `mcp-vision-integration.md` —— 视觉分析接入
- `Deployment_all.md` —— 全模块部署（当前用的模式）
