# RoPet 固件烧录 GUI 工具

一个跨平台的图形化烧录工具, 底层使用 `esptool` (与 ESP-IDF 相同的烧录器)。

CI 打包出的 exe **已内置该版本的两个固件** (S3 主控 + C5 WiFi Bridge), 运行时
不联网、不下载, 选串口后直接烧。

## 功能

- **内置固件**: exe 自带本版本的 `v{版本}_zhengchen_eye.bin` 与 `v{版本}_c5-wifi-bridge.bin`,
  版本下拉框显示为 `v{版本} (内置)`, "刷新版本" 按钮禁用
- 未检测到内置固件时 (如本地直接跑脚本) 自动回退到 GitHub Release 在线获取
- 目标选择:
  - 小智主控 (征辰 AI-EYE / ESP32-S3)
  - C5 WiFi Bridge (ESP32-C5)
- 串口选择 (自动枚举)
- 波特率选择 (烧录波特率 / 调试波特率分开), 擦除 Flash
- 实时日志输出
- **串口调试 (Console)**: 打开串口监视器查看设备实时 log, 支持向设备发送命令
  - 烧录成功后自动打开串口调试, 直接查看启动日志
  - 调试波特率默认 115200 (ESP-IDF console log 默认值)
  - 发送框可选行尾结束符 (LF / CRLF / CR / 无)
  - 烧录/擦除与串口调试互斥, 工具会自动关闭串口再烧录, 烧完自动重开

## 安装依赖

```bash
pip install -r scripts/flasher/requirements.txt
```

## 运行

```bash
python scripts/flasher/flash_gui.py
```

## 说明

- 固件为 `merge-bin` 合并后的完整镜像, 直接烧录到偏移 `0x0`。
- 本地想复现"内置固件"效果: 在 `scripts/flasher/firmware/` 下放两个
  `v{版本}_zhengchen_eye.bin` / `v{版本}_c5-wifi-bridge.bin` 即可 (该目录已 gitignore)。
- 仅在**没有**内置固件时才会访问 GitHub; 此时若为私有仓库或触发 API 限流,
  可设置环境变量 `GITHUB_TOKEN` 后再运行。
- 若枚举不到串口, 请确认已安装对应的 USB-UART 驱动 (CH34x / CP210x 等)。
- ESP32-C5 需要较新的 esptool (requirements 已锁定 `>=4.8`)。

## 打包为 exe (可选)

```bash
pip install pyinstaller
# 把 firmware/ 下的固件一起打进 exe (Windows 用 ';' 分隔, macOS/Linux 用 ':')
pyinstaller --noconsole --onefile \
  --paths scripts/flasher --collect-all esptool \
  --add-data "scripts/flasher/firmware;firmware" \
  scripts/flasher/flash_gui.py
```

CI (`.github/workflows/build-firmware.yml`) 中 `flasher` job 依赖 `build` job,
先下载两个固件产物到 `scripts/flasher/firmware/`, 校验齐全后再打包。
