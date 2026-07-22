import serial, time, sys
s = serial.Serial("COM21", 115200, timeout=0.1)
print("=== COM21 (S3) 30s ===", flush=True)
end = time.time() + 30
while time.time() < end:
    d = s.read(4096)
    if d:
        sys.stdout.write(d.decode('utf-8','replace')); sys.stdout.flush()
s.close()
