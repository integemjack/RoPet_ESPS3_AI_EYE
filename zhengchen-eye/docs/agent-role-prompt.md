# 角色提示词：动作触发规则

服务端控制台的角色介绍字段有 **2000 字符上限**（`roleConfig.vue:103` 的 `maxlength="2000"`，数据库列本身是 `TEXT`，所以是 UI 层限制）。

第 2 节实测 **1964 字符**，只剩 36 字符余量。想加自己的人设描述必须先腾地方，第 3 节列了哪些能砍。

## 1. 粘到哪里

控制台 → 智能体管理 → 编辑智能体 → **角色介绍**（对应模板的 `{{base_prompt}}`）。改完立即生效，不用重启服务、不用刷固件。

**三个前提**（不满足动作不会触发）：

1. 意图识别选 `function_call`（推荐）或 `intent_llm`，**不能是 `nointent`**
2. 用 `function_call` 时 LLM 要支持 function call（ChatGLMLLM 支持；求稳用 `DoubaoLLM` + `doubao-1-5-pro-32k-250115`）
3. 设备要在 C5 网络模式，否则工具返回"舵机链路不可用"

**工具名写下划线形式。** 服务端 `sanitize_tool_name()` 做 `re.sub(r"[^a-zA-Z0-9_\-\u4e00-\u9fff]", "_", name)`，`self.pet.walk` → `self_pet_walk`。写点号 LLM 会照抄然后调不到。

**不用重复写"必须调工具"。** `agent-base-prompt.txt` 的 `<tool_calling>` 和 `<action_priority>` 已经强制了这点。角色介绍只管"哪句话调哪个工具、参数填多少"。

---

## 2. 可直接粘贴（纯英文，1964 字符）

只面向英语场景，中文触发词和中文 ASR 错法已全部移除。回复语言由模板的 `<language>` 块单独控制，与这里无关。

```
You are a real robot pet dog with four legs and a tail. When the user asks
you to move, call the matching tool.

self_pet_walk direction REQUIRED 1=forward -1=backward
  (back, backward, reverse, retreat, step back = -1)
self_pet_turn direction REQUIRED -1=left 1=right
  spin, turn around, do a circle = repeat 6, direction 1 unless a side is named
self_pet_wave direction REQUIRED -1=left paw 1=right paw
  also greet, say hi, wave hello, shake hands, high five, give me your paw
self_pet_dance also perform, do a trick, show me something, entertain me,
  show off - any vague request to be entertaining
self_pet_lie_down lie down and hold; also sleep, go to sleep, take a rest
self_pet_sit sit and hold
self_pet_jump hop in place
self_pet_wag_tail, self_pet_nod, self_pet_shiver, self_pet_stretch
self_pet_stand_up, self_pet_stop - no parameters

DIRECTION: walk, turn and wave must ALWAYS include direction. If omitted it
silently becomes 1, so "turn left" turns RIGHT and nothing looks wrong.
OTHER PARAMS: repeat 1-50, speed 200-5000ms per cycle (lower is faster),
amount 0-100 percent. Defaults: walk 4/700/70, turn 3/700/75,
wave 3/450/75, everything else 3/600/70. A stated count sets repeat.
faster/quickly: speed x0.6. slower/slowly: x1.6.
hard/harder/big: amount 100. gently/softly/a little: amount 40.

NEGATION: call nothing when negated ("don't dance", "no need to walk").
But "stop" and "freeze" ARE commands - call self_pet_stop.

ASR FIXES. Speech recognition misfires in known ways. Read these as the
bracketed intent: welcome forward [walk forward], wet your tail [wag tail],
helthing / sounds for me / thanks for me / something for me [dance for me].
If a sentence reads oddly but clearly asks for movement, act on the
movement instead of skipping the call.

Chain calls in order for "sit then wag your tail". Call the tool first,
then say one short playful sentence. Never mention tool or parameter names.
```

---

## 3. 压缩过程 / 还能砍什么

最初的中英双语版约 3400 字符，超限 70%。压到 1964 靠三件事：

**砍掉全部中文内容。** 双语版每个工具都列了中英文触发词（`往前走、向前走、前进、走两步、走一走...`），占近一半篇幅。这些是从固件关键词表照搬的 —— 那张表必须穷举，因为它做**字符串匹配**，列不到就不认。LLM 理解语义，不需要穷举。纯英语场景下中文部分直接删。

**参数规则从列表压成句子。** 原版每条参数调整占一行，现在合成分号连接的段落，信息量不变。

**保留三类必须显式写的内容**（这些砍不掉，砍了会出错）：

- **ASR 错法** —— `welcome forward`、`wet your tail` 是反直觉映射，LLM 猜不出来
- **含糊表达归类** —— `perform`/`show me something` 归到 dance，是产品决策不是语义常识
- **方向判定词** —— `back`/`backward`/`reverse` 决定 `direction` 正负，错了动作就反

**只剩 36 字符余量。** 要加人设描述，按这个顺序腾地方（从影响最小的开始）：

1. `self_pet_stretch`、`self_pet_nod`、`self_pet_shiver` 这类单义工具从清单里删掉 —— 工具描述本身已经通过 `tools/list` 传给 LLM 了，提示词里不写它也知道有这个工具，只是少了强调
2. ASR FIXES 里的 `helthing / sounds for me / thanks for me / something for me` 四个别名压成两个最常见的
3. 参数默认值那段整段删 —— schema 里已有默认值，LLM 不传就用默认，代价是它更容易偷懒不做参数决策

**不要动的**：DIRECTION 那段（漏传 direction 是唯一从日志看不出来的失败模式）、NEGATION、以及"Call the tool first"那句。

---

## 4. 与原本地关键词表的差异

原表在固件 `eye_motion.cc` 的 `kIntentTable[]`（约 130 条），已删除。行为变化：

**顺序优先级不再需要。** 原表靠排序解决子串冲突：`shake hands` 必须排在 `shake` 前（否则握手变发抖）、`跳舞` 必须排在裸 `跳` 前。LLM 理解语义、不做子串匹配。

**词边界处理不再需要。** 原表对 ASCII 关键词查词边界，避免 `visit` 里的 `sit`、`sidewalk` 里的 `walk` 误伤。

**泛化兜底词的副作用消失。** 原表末尾有 `go`/`tail`/`走`/`尾巴` 这类单词兜底，代价是 "let's go" 这种闲聊也会触发动作。现在由 LLM 判断是否真是指令。

**响应慢 1-2 秒。** 原来 STT 落地即触发，现在等服务端一个来回。这是可配置化的固有代价。

**`nointent` 下完全失效**，原来本地关键词能兜底。

**容错别名从精确匹配降级为参考提示。** 原表命中即动，现在靠 LLM 判断，稳定性取决于模型。保留在提示词里的英文错法都是实测数据：

| 错误识别 | 真实意图 | 结论 |
|---------|---------|------|
| `welcome forward` | walk forward | 实测听成 "welcome forward all that" |
| `wet your tail` | wag your tail | 干净 TTS 音频也错，模型本身认不准 |
| `helthing` / `sounds for me` / `thanks for me` / `something for me` | dance for me | `dance` 词首 /d/ 弱，模型抓词尾 /ns/ 猜成同韵尾词 |

当初排查是逐项排除的：干净 TTS 喂同一个 SenseVoiceSmall 是 5/6 正确，排除模型；服务端实收 WAV 离线重跑与线上一字不差，排除 ASR 调用和分段；波形前有 400ms 静音，排除切头和 VAD；音量放大 8 倍结果不变，排除麦克风增益。结论是音频本身携带的信息不够。**错法跨会话会变**（`dance for me` 出过 4 种错法），同一会话内才稳定，所以别名只能治标。

中文错法（`左板`/`右板`，ASR 把"转"听成"板"）已从提示词移除。将来要支持中文的话，这条和中文触发词列表一起从 git 历史里捞：

```bash
git log --oneline -- zhengchen-eye/main/boards/zhengchen_eye/eye_motion.cc
```

**含糊表达要重点盯。** 旧代码注释记录：只说抽象要求不说具体动作的说法（中文"表演/才艺"，英文对应 `perform`/`show me something`），LLM 命中率约五成，而明确指令接近 100%。当初正因为此才把它加进本地表。提示词里我把这类并进了 `self_pet_dance` 那行并写了 "any vague request to be entertaining"，但仍是最可能退化的场景。

---

## 5. 验证

**服务端实时日志**：

```powershell
docker logs -f xiaozhi-esp32-server 2>&1 | Select-String "工具|self_pet"
```

INFO 级就能看到（`log_level: INFO` 已够用）：

```
执行工具: self_pet_turn，参数: {'repeat': 3, 'speed': 700, 'amount': 75, 'direction': -1}
发送客户端mcp工具调用请求: self.pet.turn，参数: {...}
客户端mcp工具调用 self.pet.turn 成功，原始结果: {'content': [...], 'isError': False}
```

**盯住 `direction` 有没有出现在参数里。** 缺失时固件会静默用默认值 1，日志一切正常但方向是错的 —— 这是唯一无法从日志发现的失败模式。

想看更细的（`调用函数:`、逐条工具清单、`处理MCP消息`）要把参数管理里的 `log.log_level` 改成 `DEBUG` 并 `docker restart xiaozhi-esp32-server`。

**设备端串口**（`EyeMotion` tag）：`registered self.pet.* motion tools`、`done: seq=... action=... result=...`

**建议测试句**（英语）：

- 方向：turn left / turn right / walk forward / go backward（重点看 `direction` 值对不对）
- 参数：walk three steps / jump faster / wag your tail gently / spin around twice
- 含糊：perform something / show me a trick / entertain me / do something
- 否定：don't dance / no need to walk / I don't want you to sit
- 容错：welcome forward / wet your tail / thanks for me
- 组合：sit then wag your tail
- 无参数：stand up / stop

**排查顺序**：没有 `客户端设备支持的工具数量` → 设备没上报 `features.mcp`；有清单无 `执行工具:` → LLM 没调，看提示词或 Intent 配置；有 `执行工具:` 但结果是"舵机链路不可用" → 设备不在 C5 模式；结果 `true` 但狗没动 → 看设备串口的 C5 侧。

---

## 相关文档

- [`keyword-and-mcp.md`](./keyword-and-mcp.md) —— 设备端工具清单、协议链路、如何加新工具
