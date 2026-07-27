"""临时诊断脚本: 复位 C5 并抓取启动日志, 定位 SoftAP 不出现的原因。"""
import serial, time, sys

PORT = "COM22"
s = serial.Serial(PORT, 115200, timeout=0.1)

# 用 DTR/RTS 硬复位, 保证抓到从 0 开始的启动日志
s.setDTR(False)
s.setRTS(True)
time.sleep(0.15)
s.setRTS(False)
time.sleep(0.05)
s.reset_input_buffer()

print(f"=== C5 {PORT} boot log, 25s ===", flush=True)
end = time.time() + 25
while time.time() < end:
    d = s.read(4096)
    if d:
        sys.stdout.write(d.decode("utf-8", "replace"))
        sys.stdout.flush()
s.close()
