#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RoPet 固件烧录 GUI 工具

功能:
  - 从 GitHub Release 拉取版本列表 (workflow 上传的 v{ver}_{board}.bin)
  - 选择目标: 小智主控 (ESP32-S3) 或 C5 WiFi Bridge (ESP32-C5)
  - 选择串口
  - 使用 esptool (与 ESP-IDF v5.5.2 相同的底层烧录器) 将 merged-binary 烧录到 0x0

依赖: esptool, pyserial, requests  (见 requirements.txt)
用法: python flash_gui.py
"""

import os
import sys
import io
import contextlib
import threading
import tempfile
import queue
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import esptool
except ImportError:
    esptool = None

try:
    import requests
except ImportError:
    requests = None

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    serial = None
    list_ports = None

# ---- 配置 ----
# 打包版本号: CI 打包前会生成 _version.py 写入实际版本 (见 build-firmware.yml),
# 本地直接运行时该文件不存在, 回退为 "dev"。
try:
    from _version import APP_VERSION
except ImportError:
    APP_VERSION = "dev"

GITHUB_OWNER = "integemjack"
GITHUB_REPO = "RoPet_ESPS3_AI_EYE"
RELEASES_API = f"https://api.github.com/repos/{GITHUB_OWNER}/{GITHUB_REPO}/releases"

# 目标定义: 显示名 -> (chip, 资产名匹配关键字, 烧录波特率)
# 资产名形如 v1.8.5_zhengchen_eye.bin / v1.8.5_c5-wifi-bridge.bin
TARGETS = {
    "小智主控 (征辰 AI-EYE / ESP32-S3)": {"chip": "esp32s3", "match": "zhengchen_eye"},
    "C5 WiFi Bridge (ESP32-C5)": {"chip": "esp32c5", "match": "c5-wifi-bridge"},
}

FLASH_BAUD = 921600
# ESP-IDF 默认 console log 波特率 (与烧录波特率不同)
CONSOLE_BAUD = 115200


class _LogStream(io.TextIOBase):
    """把 esptool 的 stdout/stderr 实时转发到日志区的伪文件对象。

    esptool 的进度是用 '\\r' 覆盖同一行输出的 (如 "Writing at 0x1000... (37 %)"),
    因此这里同时按 '\\n' 和 '\\r' 切分:
      - '\\n' 结尾的整行 -> 追加一行
      - '\\r' 结尾的片段 -> 标记 replace=True, 让 UI 替换上一行 (实现进度原地刷新)
    同时把所有输出累积到 buffer, 供调用方在结束后取完整文本 (芯片识别用)。
    """

    def __init__(self, emit):
        super().__init__()
        self.emit = emit          # emit(text, replace: bool)
        self.buffer_all = []      # 完整输出累积
        self._pending = ""        # 未遇到行结束符的残留

    def write(self, s):
        if not s:
            return 0
        self.buffer_all.append(s)
        self._pending += s
        # 依次扫描, 遇到 \n 或 \r 就输出一段
        while True:
            idx_n = self._pending.find("\n")
            idx_r = self._pending.find("\r")
            if idx_n == -1 and idx_r == -1:
                break
            # 取最靠前的结束符
            if idx_n == -1 or (idx_r != -1 and idx_r < idx_n):
                seg = self._pending[:idx_r]
                self._pending = self._pending[idx_r + 1:]
                # \r 表示原地刷新 (进度)
                if seg.strip():
                    self.emit(seg.rstrip(), True)
            else:
                seg = self._pending[:idx_n]
                self._pending = self._pending[idx_n + 1:]
                if seg.strip():
                    self.emit(seg.rstrip("\r").rstrip(), False)
        return len(s)

    def flush(self):
        # 输出残留 (无结束符的最后一段)
        if self._pending.strip():
            self.emit(self._pending.rstrip(), False)
        self._pending = ""

    def isatty(self):
        # 让 esptool 认为不是终端, 避免使用 ANSI 控制序列
        return False

    def getvalue(self):
        return "".join(self.buffer_all)


class SerialConsole:
    """后台串口监视器: 打开串口, 起线程持续读取并回调输出; 支持写入数据。

    仅负责串口 I/O, 不直接碰 UI; 通过 on_data / on_closed 回调把数据交给上层。
    """

    def __init__(self, port, baud, on_data, on_closed):
        self.port = port
        self.baud = int(baud)
        self.on_data = on_data          # 收到文本时回调 (str)
        self.on_closed = on_closed      # 串口异常/关闭时回调 (str reason)
        self.ser = None
        self._running = False
        self._thread = None

    def open(self):
        if serial is None:
            raise RuntimeError("未安装 pyserial, 无法打开串口调试")
        # timeout 用于让读线程能周期性检查退出标志
        self.ser = serial.Serial(self.port, self.baud, timeout=0.1)
        self._running = True
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def _read_loop(self):
        reason = None
        try:
            while self._running:
                try:
                    data = self.ser.read(4096)
                except Exception as e:
                    reason = str(e)
                    break
                if data:
                    text = data.decode("utf-8", errors="replace")
                    self.on_data(text)
        finally:
            if self._running:
                # 非正常退出 (设备拔出等)
                self._running = False
                self.on_closed(reason or "串口已断开")

    def write(self, data: bytes):
        if self.ser and self.ser.is_open:
            self.ser.write(data)

    def close(self):
        self._running = False
        t = self._thread
        if t and t.is_alive() and t is not threading.current_thread():
            t.join(timeout=1.0)
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def is_open(self):
        return self._running and self.ser is not None


class FlasherApp:
    def __init__(self, root):
        self.root = root
        root.title(f"RoPet 固件烧录工具  v{APP_VERSION}")
        root.geometry("720x560")
        root.minsize(640, 480)

        # releases: list of dict {tag, name, assets:[{name,url}]}
        self.releases = []
        self.detected_chip = None  # 当前选中端口检测到的芯片 (esptool chip 标识)
        self.port_chips = {}       # port -> chip 标识 (None 表示未识别)
        self.target_manual = False # 用户是否手动选过固件 (手动优先于自动匹配)
        self.probed_ports = set()  # 已完成芯片探测的端口
        # esptool 在进程内调用时会重定向全局 stdout, 必须串行化, 避免并发互相干扰
        self.esptool_lock = threading.Lock()
        self.log_queue = queue.Queue()

        # 串口调试 (console) 状态
        self.console = None          # 当前 SerialConsole 实例
        self.busy = False            # 是否正在烧录/擦除
        # 上一条日志是否为可被覆盖的进度行 (供 _poll_log 原地刷新使用)
        self._last_line_replaceable = False

        self._build_ui()
        self._poll_log()
        self.log(f"RoPet 固件烧录工具 v{APP_VERSION}")
        self.refresh_releases()
        self.refresh_ports()

        # 关闭窗口时清理串口资源
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---------------- UI ----------------
    def _build_ui(self):
        pad = {"padx": 8, "pady": 6}
        frm = ttk.Frame(self.root)
        frm.pack(fill="x", **pad)

        # 固件 (烧录目标) 选择: 默认按串口芯片自动选, 也可手动选择
        ttk.Label(frm, text="固件:").grid(row=0, column=0, sticky="w")
        self.target_var = tk.StringVar(value=list(TARGETS.keys())[0])
        self.target_cb = ttk.Combobox(
            frm, textvariable=self.target_var, values=list(TARGETS.keys()),
            state="readonly", width=32,
        )
        self.target_cb.grid(row=0, column=1, sticky="w")
        self.target_cb.bind("<<ComboboxSelected>>", lambda e: self._on_target_picked())

        # 版本选择
        ttk.Label(frm, text="版本:").grid(row=1, column=0, sticky="w")
        self.version_var = tk.StringVar()
        self.version_cb = ttk.Combobox(
            frm, textvariable=self.version_var, values=[], state="readonly", width=32,
        )
        self.version_cb.grid(row=1, column=1, sticky="w")
        self.version_cb.bind("<<ComboboxSelected>>", lambda e: self._update_asset_hint())
        ttk.Button(frm, text="刷新版本", command=self.refresh_releases).grid(row=1, column=2, sticky="w")

        # 串口选择
        ttk.Label(frm, text="串口:").grid(row=2, column=0, sticky="w")
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(frm, textvariable=self.port_var, values=[], width=32)
        self.port_cb.grid(row=2, column=1, sticky="w")
        # 选择串口后, 根据该端口已识别的芯片自动选中匹配的固件目标
        self.port_cb.bind("<<ComboboxSelected>>", lambda e: self._on_selected_port_changed())
        ttk.Button(frm, text="刷新串口", command=self.refresh_ports).grid(row=2, column=2, sticky="w")

        # 波特率
        ttk.Label(frm, text="烧录波特率:").grid(row=3, column=0, sticky="w")
        self.baud_var = tk.StringVar(value=str(FLASH_BAUD))
        ttk.Combobox(
            frm, textvariable=self.baud_var,
            values=["115200", "460800", "921600", "1500000"],
            state="readonly", width=32,
        ).grid(row=3, column=1, sticky="w")

        # 串口调试波特率 (console log, 默认 115200)
        ttk.Label(frm, text="调试波特率:").grid(row=4, column=0, sticky="w")
        self.console_baud_var = tk.StringVar(value=str(CONSOLE_BAUD))
        ttk.Combobox(
            frm, textvariable=self.console_baud_var,
            values=["74880", "115200", "230400", "460800", "921600"],
            state="readonly", width=32,
        ).grid(row=4, column=1, sticky="w")

        # 芯片检测提示
        self.chip_hint = ttk.Label(frm, text="", foreground="#666")
        self.chip_hint.grid(row=5, column=0, columnspan=3, sticky="w")

        # 资产提示
        self.asset_hint = ttk.Label(frm, text="", foreground="#666")
        self.asset_hint.grid(row=6, column=0, columnspan=3, sticky="w")

        frm.columnconfigure(1, weight=1)

        # 按钮
        btn_frm = ttk.Frame(self.root)
        btn_frm.pack(fill="x", **pad)
        self.flash_btn = ttk.Button(btn_frm, text="开始烧录", command=self.start_flash)
        self.flash_btn.pack(side="left")
        self.erase_btn = ttk.Button(btn_frm, text="擦除 Flash", command=self.start_erase)
        self.erase_btn.pack(side="left", padx=8)
        self.console_btn = ttk.Button(btn_frm, text="打开串口调试", command=self.toggle_console)
        self.console_btn.pack(side="left")

        # 日志
        log_frm = ttk.LabelFrame(self.root, text="日志 / 串口输出")
        log_frm.pack(fill="both", expand=True, **pad)
        self.log_text = tk.Text(log_frm, wrap="word", height=16, state="disabled")
        self.log_text.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(log_frm, command=self.log_text.yview)
        sb.pack(side="right", fill="y")
        self.log_text.config(yscrollcommand=sb.set)

        # 串口发送输入框 (仅在 console 打开时可用)
        send_frm = ttk.Frame(self.root)
        send_frm.pack(fill="x", **pad)
        ttk.Label(send_frm, text="发送:").pack(side="left")
        self.send_var = tk.StringVar()
        self.send_entry = ttk.Entry(send_frm, textvariable=self.send_var, state="disabled")
        self.send_entry.pack(side="left", fill="x", expand=True, padx=6)
        self.send_entry.bind("<Return>", lambda e: self.send_console())
        # 行尾结束符选择
        self.eol_var = tk.StringVar(value="LF")
        ttk.Combobox(
            send_frm, textvariable=self.eol_var,
            values=["LF", "CRLF", "CR", "无"], state="readonly", width=6,
        ).pack(side="left")
        self.send_btn = ttk.Button(send_frm, text="发送", command=self.send_console, state="disabled")
        self.send_btn.pack(side="left", padx=6)

    # ---------------- 日志 ----------------
    def log(self, msg):
        self.log_queue.put(msg)

    def _poll_log(self):
        """从队列取日志写入文本区。

        队列元素可以是:
          - str            : 普通整行追加
          - (text, replace): replace=True 时替换最后一行 (用于 esptool 进度原地刷新)
        """
        wrote = False
        try:
            while True:
                item = self.log_queue.get_nowait()
                if isinstance(item, tuple):
                    text, replace = item
                else:
                    text, replace = item, False

                if not wrote:
                    self.log_text.config(state="normal")
                    wrote = True

                if replace and self._last_line_replaceable:
                    # 删除上一行 (进度行), 用新内容替换, 实现原地刷新
                    self.log_text.delete("end-2l linestart", "end-1c")
                    self.log_text.insert("end", text + "\n")
                else:
                    self.log_text.insert("end", text + "\n")
                self._last_line_replaceable = replace
        except queue.Empty:
            pass
        if wrote:
            self.log_text.see("end")
            self.log_text.config(state="disabled")
        # 进度刷新需要较快的响应, 40ms 轮询
        self.root.after(40, self._poll_log)

    # ---------------- 串口 ----------------
    # esptool 输出中的芯片名 -> 对应的 chip 标识
    CHIP_KEYWORDS = {
        "ESP32-C5": "esp32c5",
        "ESP32-S3": "esp32s3",
    }
    # chip 标识 -> 友好显示名
    CHIP_DISPLAY = {
        "esp32s3": "ESP32-S3",
        "esp32c5": "ESP32-C5",
    }

    # Espressif 官方 USB VID。内置 USB-Serial-JTAG 的芯片 (S3/C3/C5/C6...)
    # 会直接以该 VID 出现, PID 0x1001 为 USB-Serial-JTAG。
    ESPRESSIF_VID = 0x303A
    # 常见 USB-UART 桥接芯片的 VID (CP210x / CH34x / FTDI / PL2303)
    # 这些板子需要真正握手才能确定芯片型号。
    UART_BRIDGE_VIDS = {0x10C4, 0x1A86, 0x0403, 0x067B}
    # 明显不是 ESP 设备的端口描述关键字 (蓝牙虚拟串口等), 直接跳过探测
    SKIP_DESC_KEYWORDS = ("蓝牙", "bluetooth", "标准串行", "通信端口", "communications port")

    def _make_port_label(self, port, chip_display):
        """把端口和芯片名组合成列表显示串, 如 'COM21 - ESP32-S3'。"""
        return f"{port} - {chip_display}" if chip_display else port

    def _port_from_label(self, label):
        """从列表显示串反解出真实端口号 (取 ' - ' 前的部分)。"""
        return label.split(" - ", 1)[0].strip() if label else ""

    def refresh_ports(self):
        if list_ports is None:
            self.log("[警告] 未安装 pyserial, 无法枚举串口。请 pip install pyserial")
            return
        # console 打开时串口被占用, 芯片探测(esptool)会失败, 先关闭再刷新
        if self.console and self.console.is_open():
            self.log("[提示] 刷新串口需先关闭串口调试")
            self._console_close()
        infos = list(list_ports.comports())
        ports = [p.device for p in infos]
        # port -> 检测到的 chip 标识 (None 表示未识别)
        self.port_chips = {p: None for p in ports}
        self.probed_ports = set()

        self.log(f"检测到串口: {', '.join(ports) if ports else '(无)'}")
        if not ports:
            self.port_cb["values"] = []
            self.chip_hint.config(text="未检测到串口", foreground="#a00")
            return

        # ---- 第一轮: 按 USB VID/PID 与描述做零成本快速判定 ----
        # 目的: 避免对蓝牙虚拟串口等做昂贵的 esptool 握手 (每个要等好几秒)
        need_probe = []      # 需要真正握手探测的端口
        labels = []
        for info in infos:
            port = info.device
            vid = info.vid
            desc = (info.description or "")
            desc_l = desc.lower()

            if vid == self.ESPRESSIF_VID:
                # Espressif 原生 USB: 无需握手即可确定厂商, 但具体型号仍需握手,
                # 先标 "Espressif" 并加入探测队列 (优先探测)。
                need_probe.insert(0, port)
                labels.append(self._make_port_label(port, "Espressif 检测中..."))
            elif vid in self.UART_BRIDGE_VIDS:
                # USB-UART 桥接芯片: 可能接着 ESP, 需要握手
                need_probe.append(port)
                labels.append(self._make_port_label(port, "检测中..."))
            elif vid is None and any(k in desc_l or k in desc for k in self.SKIP_DESC_KEYWORDS):
                # 蓝牙/板载通信端口: 明确跳过, 不做握手
                self.port_chips[port] = None
                self.probed_ports.add(port)
                labels.append(self._make_port_label(port, "非 ESP 设备"))
            else:
                need_probe.append(port)
                labels.append(self._make_port_label(port, "检测中..."))

        self.port_cb["values"] = labels
        if labels and not self.port_var.get():
            # 默认选中第一个待探测(最可能是 ESP)的端口
            prefer = need_probe[0] if need_probe else None
            pick = next((l for l in labels if self._port_from_label(l) == prefer), labels[0])
            self.port_var.set(pick)

        skipped = len(ports) - len(need_probe)
        if skipped:
            self.log(f"跳过 {skipped} 个明显非 ESP 的端口 (蓝牙/板载串口)")
        if not need_probe:
            self.chip_hint.config(text="未发现可能的 ESP 设备端口", foreground="#a00")
            return

        self.chip_hint.config(
            text=f"正在并行识别 {len(need_probe)} 个端口的芯片型号 ...", foreground="#666")
        # ---- 第二轮: 并行握手探测 (每个端口一个线程, 独立串口互不干扰) ----
        threading.Thread(target=self._probe_all_ports, args=(need_probe,), daemon=True).start()

    def _probe_all_ports(self, ports):
        """并行探测多个端口的芯片型号。

        用 esptool.detect_chip() 而非 esptool.main(["chip_id"]):
          - 不经过 argparse / 不重定向全局 stdout, 因此**可以并发**
          - connect_attempts=1 避免默认 7 次重试拖慢无设备的端口
        """
        threads = []
        for port in ports:
            t = threading.Thread(target=self._probe_one_port, args=(port,), daemon=True)
            t.start()
            threads.append(t)
        for t in threads:
            t.join()

    def _probe_one_port(self, port):
        chip = self._detect_chip_on_port(port)
        self.root.after(0, lambda p=port, c=chip: self._on_port_probed(p, c))

    def _detect_chip_on_port(self, port):
        """探测某端口的芯片型号, 返回 chip 标识 (如 'esp32s3') 或 None。

        用 esptool.detect_chip() 直连底层, 相比 esptool.main(["chip_id"]):
          - 不重定向全局 stdout, 可并发调用 (无需 esptool_lock 串行化)
          - connect_attempts=1: 默认是 7 次重试, 对没有 ESP 的端口会白等很久
          - 探完立刻关串口, 不做 chip_id 之外的额外读取
        注意: 必须进程内调用而非 sys.executable 起子进程 —— 打包成 exe 后
        sys.executable 指向 exe 本身, 会导致无限重开 GUI 窗口。
        """
        if esptool is None:
            return None
        esp = None
        try:
            # 探测阶段用标准 115200 即可, 只需握手不传数据
            esp = esptool.detect_chip(port=port, baud=115200, connect_attempts=1)
            name = (esp.CHIP_NAME or "").upper()
        except Exception:
            return None
        finally:
            if esp is not None:
                try:
                    esp._port.close()
                except Exception:
                    pass
        for keyword, c in self.CHIP_KEYWORDS.items():
            if keyword in name:
                return c
        # 识别到 ESP 芯片但不在支持列表内, 仍返回 None (无匹配固件)
        return None

    def _invoke_esptool(self, argv, echo=False):
        """进程内调用 esptool.main(argv), 捕获其输出并返回。

        用 esptool_lock 串行化, 因为期间会重定向全局 sys.stdout/stderr。
        echo=True 时把输出**实时**转发到日志区 (进度行原地刷新)。
        """
        if esptool is None:
            raise RuntimeError("未打包 esptool 模块")
        if echo:
            emit = lambda text, replace: self.log_queue.put((text, replace))
        else:
            emit = lambda text, replace: None
        stream = _LogStream(emit)
        with self.esptool_lock:
            try:
                with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
                    esptool.main(argv)
            finally:
                try:
                    stream.flush()
                except Exception:
                    pass
                out = stream.getvalue()
        return out

    def _on_port_probed(self, port, chip):
        # 端口可能在探测期间已被刷新掉
        if port not in getattr(self, "port_chips", {}):
            return
        self.port_chips[port] = chip
        chip_display = self.CHIP_DISPLAY.get(chip, "未知芯片")
        new_label = self._make_port_label(port, chip_display)

        # 更新下拉列表里对应项的文字
        labels = list(self.port_cb["values"])
        for i, lbl in enumerate(labels):
            if self._port_from_label(lbl) == port:
                # 若该项正被选中, 同步更新选中文本
                was_selected = (self.port_var.get() == lbl)
                labels[i] = new_label
                if was_selected:
                    self.port_var.set(new_label)
                break
        self.port_cb["values"] = labels

        if chip:
            self.log(f"{port}: 识别为 {chip_display}")
        else:
            self.log(f"{port}: 未识别到 ESP 芯片")

        # 若更新的是当前选中端口, 自动匹配固件目标
        if self._port_from_label(self.port_var.get()) == port:
            self._on_selected_port_changed()

        # 记录已探测完成的端口
        self.probed_ports.add(port)
        # 所有端口都探测完毕后, 若一个都没识别出来给出总提示
        if self.probed_ports >= set(self.port_chips.keys()):
            identified = sum(1 for v in self.port_chips.values() if v is not None)
            if identified == 0 and not self._port_from_label(self.port_var.get()):
                self.chip_hint.config(
                    text="未能识别任何端口的芯片 (设备是否已进入烧录模式?)",
                    foreground="#a00",
                )

    def _on_target_picked(self):
        """用户手动选择固件: 标记为手动 (后续不再被自动匹配覆盖)。"""
        self.target_manual = True
        self._update_asset_hint()
        # 提示手动选择的固件与当前端口芯片是否匹配
        port = self._port_from_label(self.port_var.get())
        chip = self.port_chips.get(port)
        target = TARGETS.get(self.target_var.get())
        if chip and target and target["chip"] != chip:
            self.chip_hint.config(
                text=f"注意: 已手动选择 {target['chip']} 固件, 但 {port} 检测为 "
                     f"{self.CHIP_DISPLAY.get(chip, chip)}, 烧录前请确认。",
                foreground="#a00",
            )
        else:
            self.chip_hint.config(
                text=f"已手动选择固件 [{self.target_var.get()}]", foreground="#666",
            )

    def _on_selected_port_changed(self):
        """选中的端口变化时, 根据其芯片型号自动选择匹配的固件目标。

        仅在用户未手动选择固件时执行自动匹配; 手动选择优先。
        """
        port = self._port_from_label(self.port_var.get())
        chip = getattr(self, "port_chips", {}).get(port)
        self.detected_chip = chip

        # 若 console 开在另一个端口, 切换端口时关闭它 (避免占用旧端口)
        if self.console and self.console.is_open() and self.console.port != port:
            self._console_close()

        # 用户已手动选过固件, 不覆盖, 仅在不匹配时提示
        if self.target_manual:
            target = TARGETS.get(self.target_var.get())
            if chip and target and target["chip"] != chip:
                self.chip_hint.config(
                    text=f"注意: 当前固件为 {target['chip']}, 但 {port} 检测为 "
                         f"{self.CHIP_DISPLAY.get(chip, chip)}, 烧录前请确认。",
                    foreground="#a00",
                )
            return

        if not chip:
            self.chip_hint.config(
                text=f"{port}: 芯片型号未知, 请手动选择固件", foreground="#a00",
            )
            return

        matching = [name for name, t in TARGETS.items() if t["chip"] == chip]
        current = TARGETS.get(self.target_var.get())
        if not matching:
            self.chip_hint.config(
                text=f"检测到 {chip}, 但没有对应的固件", foreground="#a00",
            )
            return

        # 当前所选目标芯片已匹配则保持 (不覆盖已选的具体板型)
        if current and current["chip"] == chip:
            selected = self.target_var.get()
        else:
            selected = matching[0]
            self.target_var.set(selected)
            self._update_asset_hint()

        chip_display = self.CHIP_DISPLAY.get(chip, chip)
        if len(matching) > 1:
            hint = (f"{port} = {chip_display}, 已自动选择 [{selected}]。"
                    f"该芯片有多个板型, 如不符请手动选择。")
        else:
            hint = f"{port} = {chip_display}, 已自动选择 [{selected}]。"
        self.chip_hint.config(text=hint, foreground="#080")

    # ---------------- Releases ----------------
    def refresh_releases(self):
        if requests is None:
            self.log("[错误] 未安装 requests, 无法获取 Release。请 pip install requests")
            return
        self.log("正在获取 Release 列表 ...")
        threading.Thread(target=self._fetch_releases, daemon=True).start()

    def _fetch_releases(self):
        try:
            headers = {"Accept": "application/vnd.github+json"}
            token = os.environ.get("GITHUB_TOKEN")
            if token:
                headers["Authorization"] = f"Bearer {token}"
            resp = requests.get(RELEASES_API, headers=headers, timeout=20)
            resp.raise_for_status()
            data = resp.json()
            self.releases = []
            for rel in data:
                assets = [
                    {"name": a["name"], "url": a["browser_download_url"], "size": a["size"]}
                    for a in rel.get("assets", [])
                    if a["name"].endswith(".bin")
                ]
                if assets:
                    self.releases.append({
                        "tag": rel["tag_name"],
                        "name": rel.get("name") or rel["tag_name"],
                        "assets": assets,
                    })
            tags = [r["tag"] for r in self.releases]
            self.root.after(0, lambda: self._set_versions(tags))
            self.log(f"获取到 {len(tags)} 个版本: {', '.join(tags) if tags else '(无)'}")
        except Exception as e:
            self.log(f"[错误] 获取 Release 失败: {e}")

    def _set_versions(self, tags):
        self.version_cb["values"] = tags
        if tags and not self.version_var.get():
            self.version_var.set(tags[0])
        self._update_asset_hint()

    def _find_asset(self):
        """根据当前选择的版本与目标, 找到对应的 .bin 资产。"""
        tag = self.version_var.get()
        target = TARGETS.get(self.target_var.get())
        if not tag or not target:
            return None
        rel = next((r for r in self.releases if r["tag"] == tag), None)
        if not rel:
            return None
        match = target["match"]
        return next((a for a in rel["assets"] if match in a["name"]), None)

    def _update_asset_hint(self):
        asset = self._find_asset()
        if asset:
            self.asset_hint.config(
                text=f"固件: {asset['name']}  ({asset['size'] // 1024} KB)",
                foreground="#080",
            )
        else:
            self.asset_hint.config(
                text="该版本下未找到匹配所选目标的固件", foreground="#a00",
            )

    # ---------------- 下载 ----------------
    def _download_asset(self, asset):
        headers = {}
        token = os.environ.get("GITHUB_TOKEN")
        if token:
            headers["Authorization"] = f"Bearer {token}"
        tmp_dir = tempfile.gettempdir()
        path = os.path.join(tmp_dir, asset["name"])
        self.log(f"下载 {asset['name']} ...")
        with requests.get(asset["url"], headers=headers, stream=True, timeout=60) as r:
            r.raise_for_status()
            with open(path, "wb") as f:
                for chunk in r.iter_content(chunk_size=65536):
                    f.write(chunk)
        self.log(f"已下载到: {path}")
        return path

    # ---------------- 烧录 ----------------
    def _validate(self):
        if not self._port_from_label(self.port_var.get()):
            messagebox.showwarning("提示", "请先选择串口")
            return False
        return True

    def start_flash(self):
        if not self._validate():
            return
        asset = self._find_asset()
        if not asset:
            messagebox.showerror("错误", "未找到匹配的固件, 请检查版本与目标选择")
            return
        # 若已检测到芯片且与所选目标不一致, 烧错固件风险很高, 先让用户确认
        target = TARGETS.get(self.target_var.get())
        if self.detected_chip and target and self.detected_chip != target["chip"]:
            if not messagebox.askyesno(
                "芯片不匹配",
                f"检测到端口芯片为 {self.detected_chip}, 但所选固件目标是 "
                f"{target['chip']}。\n烧录不匹配的固件可能导致设备无法启动。\n\n仍要继续吗?",
            ):
                return
        # 烧录前释放串口 (console 与 esptool 互斥); 记录是否需要烧完自动重开
        self._console_was_open = bool(self.console and self.console.is_open())
        if self._console_was_open:
            self._console_close()
        self._set_busy(True)
        threading.Thread(target=self._flash_worker, args=(asset,), daemon=True).start()

    def _flash_worker(self, asset):
        try:
            target = TARGETS[self.target_var.get()]
            chip = target["chip"]
            port = self._port_from_label(self.port_var.get())
            baud = self.baud_var.get()
            path = self._download_asset(asset)
            args = [
                "--chip", chip,
                "--port", port,
                "--baud", baud,
                "write_flash", "0x0", path,
            ]
            self.log(f"开始烧录 {chip} @ {port} ...")
            self._run_esptool(args)
            self.log("✅ 烧录完成")
            # 烧录成功后自动打开串口调试, 直接查看启动日志
            cbaud = self.console_baud_var.get()
            self.root.after(0, lambda: self._console_open(port, cbaud))
        except Exception as e:
            self.log(f"[错误] 烧录失败: {e}")
        finally:
            self.root.after(0, lambda: self._set_busy(False))

    def start_erase(self):
        if not self._validate():
            return
        if not messagebox.askyesno("确认", "确定要擦除整个 Flash 吗?"):
            return
        # 擦除前释放串口 (与 esptool 互斥)
        if self.console and self.console.is_open():
            self._console_close()
        self._set_busy(True)
        threading.Thread(target=self._erase_worker, daemon=True).start()

    def _erase_worker(self):
        try:
            target = TARGETS[self.target_var.get()]
            port = self._port_from_label(self.port_var.get())
            args = [
                "--chip", target["chip"],
                "--port", port,
                "erase_flash",
            ]
            self.log(f"开始擦除 {target['chip']} @ {port} ...")
            self._run_esptool(args)
            self.log("✅ 擦除完成")
        except Exception as e:
            self.log(f"[错误] 擦除失败: {e}")
        finally:
            self.root.after(0, lambda: self._set_busy(False))

    def _run_esptool(self, args):
        """进程内调用 esptool 执行烧录/擦除, 输出写入日志。

        不能用 sys.executable 起子进程 —— 打包成 exe 后 sys.executable 是 exe
        本身, 会无限重开 GUI 窗口。esptool.main() 出错时会抛异常或 SystemExit。
        """
        self.log("$ esptool " + " ".join(args))
        try:
            self._invoke_esptool(list(args), echo=True)
        except SystemExit as e:
            # esptool 出错时可能调用 sys.exit(非0)
            if e.code not in (0, None):
                raise RuntimeError(f"esptool 退出码 {e.code}")

    def _set_busy(self, busy):
        self.busy = busy
        state = "disabled" if busy else "normal"
        self.flash_btn.config(state=state)
        self.erase_btn.config(state=state)
        # 烧录/擦除期间禁止手动开关 console (串口被 esptool 占用)
        self.console_btn.config(state=state)

    # ---------------- 串口调试 (console) ----------------
    def _console_open(self, port, baud):
        """打开串口监视器 (幂等: 先关旧的)。成功返回 True。"""
        if serial is None:
            self.log("[错误] 未安装 pyserial, 无法打开串口调试")
            return False
        self._console_close()
        try:
            self.console = SerialConsole(
                port, baud,
                on_data=lambda text: self.log_queue.put(text.rstrip("\n")) if text.strip() else None,
                on_closed=lambda reason: self.root.after(0, lambda: self._on_console_closed(reason)),
            )
            self.console.open()
        except Exception as e:
            self.log(f"[错误] 打开串口失败: {e}")
            self.console = None
            return False
        self.log(f"===== 串口调试已打开 {port} @ {baud} =====")
        self.console_btn.config(text="关闭串口调试")
        self.send_entry.config(state="normal")
        self.send_btn.config(state="normal")
        return True

    def _console_close(self):
        """关闭当前串口监视器 (若有)。"""
        if self.console:
            self.console.close()
            self.console = None
            self.log("===== 串口调试已关闭 =====")
        self.console_btn.config(text="打开串口调试")
        self.send_entry.config(state="disabled")
        self.send_btn.config(state="disabled")

    def _on_console_closed(self, reason):
        """串口被动断开 (设备拔出等) 时的回调。"""
        self.log(f"[串口] {reason}")
        self.console = None
        self.console_btn.config(text="打开串口调试")
        self.send_entry.config(state="disabled")
        self.send_btn.config(state="disabled")

    def toggle_console(self):
        if self.busy:
            return
        if self.console and self.console.is_open():
            self._console_close()
            return
        port = self._port_from_label(self.port_var.get())
        if not port:
            messagebox.showwarning("提示", "请先选择串口")
            return
        self._console_open(port, self.console_baud_var.get())

    def send_console(self):
        if not (self.console and self.console.is_open()):
            return
        text = self.send_var.get()
        eol = {"LF": "\n", "CRLF": "\r\n", "CR": "\r", "无": ""}.get(self.eol_var.get(), "\n")
        try:
            self.console.write((text + eol).encode("utf-8"))
            self.log(f">>> {text}")
            self.send_var.set("")
        except Exception as e:
            self.log(f"[错误] 发送失败: {e}")

    def on_close(self):
        """窗口关闭: 清理串口线程与句柄。"""
        self._console_close()
        self.root.destroy()


def main():
    root = tk.Tk()
    FlasherApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
