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
import threading
import tempfile
import queue
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import requests
except ImportError:
    requests = None

try:
    import serial.tools.list_ports as list_ports
except ImportError:
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
# 资产名形如 v1.7.2_doit-esp32s3-eye-6824.bin / v1.7.2_c5-wifi-bridge.bin
TARGETS = {
    "小智主控 (ESP32-S3 / 6824)": {"chip": "esp32s3", "match": "doit-esp32s3-eye-6824"},
    "小智主控 (ESP32-S3 / 8311)": {"chip": "esp32s3", "match": "doit-esp32s3-eye-8311"},
    "C5 WiFi Bridge (ESP32-C5)": {"chip": "esp32c5", "match": "c5-wifi-bridge"},
}

FLASH_BAUD = 921600


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
        self.probed_ports = set()  # 已完成芯片探测的端口
        self.log_queue = queue.Queue()

        self._build_ui()
        self._poll_log()
        self.log(f"RoPet 固件烧录工具 v{APP_VERSION}")
        self.refresh_releases()
        self.refresh_ports()

    # ---------------- UI ----------------
    def _build_ui(self):
        pad = {"padx": 8, "pady": 6}
        frm = ttk.Frame(self.root)
        frm.pack(fill="x", **pad)

        # 目标选择
        ttk.Label(frm, text="烧录目标:").grid(row=0, column=0, sticky="w")
        self.target_var = tk.StringVar(value=list(TARGETS.keys())[0])
        self.target_cb = ttk.Combobox(
            frm, textvariable=self.target_var, values=list(TARGETS.keys()),
            state="readonly", width=32,
        )
        self.target_cb.grid(row=0, column=1, sticky="w")
        self.target_cb.bind("<<ComboboxSelected>>", lambda e: self._update_asset_hint())

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
        ttk.Label(frm, text="波特率:").grid(row=3, column=0, sticky="w")
        self.baud_var = tk.StringVar(value=str(FLASH_BAUD))
        ttk.Combobox(
            frm, textvariable=self.baud_var,
            values=["115200", "460800", "921600", "1500000"],
            state="readonly", width=32,
        ).grid(row=3, column=1, sticky="w")

        # 芯片检测提示
        self.chip_hint = ttk.Label(frm, text="", foreground="#666")
        self.chip_hint.grid(row=4, column=0, columnspan=3, sticky="w")

        # 资产提示
        self.asset_hint = ttk.Label(frm, text="", foreground="#666")
        self.asset_hint.grid(row=5, column=0, columnspan=3, sticky="w")

        frm.columnconfigure(1, weight=1)

        # 按钮
        btn_frm = ttk.Frame(self.root)
        btn_frm.pack(fill="x", **pad)
        self.flash_btn = ttk.Button(btn_frm, text="开始烧录", command=self.start_flash)
        self.flash_btn.pack(side="left")
        self.erase_btn = ttk.Button(btn_frm, text="擦除 Flash", command=self.start_erase)
        self.erase_btn.pack(side="left", padx=8)

        # 日志
        log_frm = ttk.LabelFrame(self.root, text="日志")
        log_frm.pack(fill="both", expand=True, **pad)
        self.log_text = tk.Text(log_frm, wrap="word", height=16, state="disabled")
        self.log_text.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(log_frm, command=self.log_text.yview)
        sb.pack(side="right", fill="y")
        self.log_text.config(yscrollcommand=sb.set)

    # ---------------- 日志 ----------------
    def log(self, msg):
        self.log_queue.put(msg)

    def _poll_log(self):
        try:
            while True:
                msg = self.log_queue.get_nowait()
                self.log_text.config(state="normal")
                self.log_text.insert("end", msg + "\n")
                self.log_text.see("end")
                self.log_text.config(state="disabled")
        except queue.Empty:
            pass
        self.root.after(100, self._poll_log)

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
        ports = [p.device for p in list_ports.comports()]
        # port -> 检测到的 chip 标识 (None 表示未识别)
        self.port_chips = {p: None for p in ports}
        self.probed_ports = set()
        # 先用 "检测中" 占位显示
        labels = [self._make_port_label(p, "检测中...") for p in ports]
        self.port_cb["values"] = labels
        if labels and not self.port_var.get():
            self.port_var.set(labels[0])
        self.log(f"检测到串口: {', '.join(ports) if ports else '(无)'}")
        if not ports:
            self.port_cb["values"] = []
            self.chip_hint.config(text="未检测到串口", foreground="#a00")
            return
        self.chip_hint.config(text="正在识别各端口芯片型号 ...", foreground="#666")
        # 并发探测所有端口
        for p in ports:
            threading.Thread(target=self._probe_port, args=(p,), daemon=True).start()

    def _probe_port(self, port):
        """后台探测单个端口的芯片型号, 完成后更新列表项。"""
        import subprocess
        chip = None
        try:
            # 不指定 --chip, 让 esptool 自动识别; 低波特率更稳, 超时后跳过
            cmd = [sys.executable, "-m", "esptool", "--port", port, "chip_id"]
            proc = subprocess.run(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=20,
            )
            out = (proc.stdout or "").upper()
            for keyword, c in self.CHIP_KEYWORDS.items():
                if keyword in out:
                    chip = c
                    break
        except Exception:
            chip = None
        self.root.after(0, lambda: self._on_port_probed(port, chip))

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

    def _on_selected_port_changed(self):
        """选中的端口变化时, 根据其芯片型号自动选择匹配的固件目标。"""
        port = self._port_from_label(self.port_var.get())
        chip = getattr(self, "port_chips", {}).get(port)
        self.detected_chip = chip
        if not chip:
            self.chip_hint.config(
                text=f"{port}: 芯片型号未知, 请手动选择固件目标", foreground="#a00",
            )
            return

        matching = [name for name, t in TARGETS.items() if t["chip"] == chip]
        current = TARGETS.get(self.target_var.get())
        if not matching:
            self.chip_hint.config(
                text=f"检测到 {chip}, 但没有对应的固件目标", foreground="#a00",
            )
            return

        # 当前所选目标芯片已匹配则保持 (不覆盖用户已选的具体板型)
        if current and current["chip"] == chip:
            selected = self.target_var.get()
        else:
            selected = matching[0]
            self.target_var.set(selected)
            self._update_asset_hint()

        chip_display = self.CHIP_DISPLAY.get(chip, chip)
        if len(matching) > 1:
            hint = (f"{port} = {chip_display}, 已选 [{selected}]。"
                    f"该芯片有多个板型, 如不符请手动选择。")
        else:
            hint = f"{port} = {chip_display}, 已选 [{selected}]。"
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
        except Exception as e:
            self.log(f"[错误] 烧录失败: {e}")
        finally:
            self.root.after(0, lambda: self._set_busy(False))

    def start_erase(self):
        if not self._validate():
            return
        if not messagebox.askyesno("确认", "确定要擦除整个 Flash 吗?"):
            return
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
        """调用 esptool。优先用 Python 模块, 逐行输出到日志。"""
        import subprocess
        cmd = [sys.executable, "-m", "esptool"] + args
        self.log("$ " + " ".join(cmd))
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )
        for line in proc.stdout:
            self.log(line.rstrip())
        proc.wait()
        if proc.returncode != 0:
            raise RuntimeError(f"esptool 退出码 {proc.returncode}")

    def _set_busy(self, busy):
        state = "disabled" if busy else "normal"
        self.flash_btn.config(state=state)
        self.erase_btn.config(state=state)


def main():
    root = tk.Tk()
    FlasherApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
