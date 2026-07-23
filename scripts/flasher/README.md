# RoPet 固件烧录 GUI 工具

一个跨平台的图形化烧录工具, 从 GitHub Release 拉取由 CI (ESP-IDF v5.5.2) 构建的固件,
选择目标芯片与串口后一键烧录。底层使用 `esptool` (与 ESP-IDF 相同的烧录器)。

## 功能

- 自动列出 GitHub Release 中的版本 (workflow 产出的 `v{版本}_{板型}.bin`)
- 目标选择:
  - 小智主控 (ESP32-S3 / 6824)
  - 小智主控 (ESP32-S3 / 8311)
  - C5 WiFi Bridge (ESP32-C5)
- 串口选择 (自动枚举)
- 波特率选择, 擦除 Flash
- 实时日志输出

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
- 若 Release 为私有仓库或触发 API 限流, 可设置环境变量 `GITHUB_TOKEN` 后再运行。
- 若枚举不到串口, 请确认已安装对应的 USB-UART 驱动 (CH34x / CP210x 等)。
- ESP32-C5 需要较新的 esptool (requirements 已锁定 `>=4.8`)。

## 打包为 exe (可选)

```bash
pip install pyinstaller
pyinstaller --noconsole --onefile scripts/flasher/flash_gui.py
```
