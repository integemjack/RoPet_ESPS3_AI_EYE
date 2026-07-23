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
        ttk.Button(frm, text="刷新串口", command=self.refresh_ports).grid(row=2, column=2, sticky="w")

        # 波特率
        ttk.Label(frm, text="波特率:").grid(row=3, column=0, sticky="w")
        self.baud_var = tk.StringVar(value=str(FLASH_BAUD))
        ttk.Combobox(
            frm, textvariable=self.baud_var,
            values=["115200", "460800", "921600", "1500000"],
            state="readonly", width=32,
        ).grid(row=3, column=1, sticky="w")

        # 资产提示
        self.asset_hint = ttk.Label(frm, text="", foreground="#666")
        self.asset_hint.grid(row=4, column=0, columnspan=3, sticky="w")

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
    def refresh_ports(self):
        if list_ports is None:
            self.log("[警告] 未安装 pyserial, 无法枚举串口。请 pip install pyserial")
            return
        ports = [p.device for p in list_ports.comports()]
        self.port_cb["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])
        self.log(f"检测到串口: {', '.join(ports) if ports else '(无)'}")

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
        if not self.port_var.get():
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
        self._set_busy(True)
        threading.Thread(target=self._flash_worker, args=(asset,), daemon=True).start()

    def _flash_worker(self, asset):
        try:
            target = TARGETS[self.target_var.get()]
            chip = target["chip"]
            port = self.port_var.get()
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
            args = [
                "--chip", target["chip"],
                "--port", self.port_var.get(),
                "erase_flash",
            ]
            self.log(f"开始擦除 {target['chip']} @ {self.port_var.get()} ...")
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
