import serial, time, sys
s = serial.Serial("COM9", 115200, timeout=0.1)
# 硬复位: 用 RTS 拉低触发 EN 复位
s.setDTR(False); s.setRTS(True); time.sleep(0.1)
s.setRTS(False); time.sleep(0.1)
s.reset_input_buffer()
print("=== S3 COM9 boot 18s ===", flush=True)
end = time.time() + 18
while time.time() < end:
    d = s.read(4096)
    if d:
        sys.stdout.write(d.decode('utf-8','replace')); sys.stdout.flush()
s.close()
