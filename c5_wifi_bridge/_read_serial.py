import serial, time, sys
s = serial.Serial("COM20", 115200, timeout=0.1)
print("=== COM20 @115200 for 12s ===", flush=True)
end = time.time() + 12
while time.time() < end:
    d = s.read(4096)
    if d:
        sys.stdout.write(d.decode('utf-8','replace')); sys.stdout.flush()
s.close()
